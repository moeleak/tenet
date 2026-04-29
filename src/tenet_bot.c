#include "bot_config.h"
#include "bot_memory.h"
#include "bot_ollama.h"
#include "bot_protocol.h"
#include "bot_screen.h"
#include "bot_util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define BOT_REPLY_CHUNK 1500

static int sender_is_bot(const bot_config_t *config, const char *sender)
{
    return strcasecmp(sender, config->username) == 0 ||
           strcasecmp(sender, config->display_name) == 0;
}

static int message_mentions_bot(const bot_config_t *config, const char *text)
{
    char mention_user[96];
    char mention_display[288];

    snprintf(mention_user, sizeof(mention_user), "@%s", config->username);
    snprintf(mention_display, sizeof(mention_display), "@%s", config->display_name);
    return strstr(text, mention_user) != NULL || strstr(text, mention_display) != NULL;
}

static void remove_token(char *text, const char *token)
{
    size_t token_len = strlen(token);
    char *pos;

    if (token_len == 0) {
        return;
    }
    while ((pos = strstr(text, token)) != NULL) {
        memmove(pos, pos + token_len, strlen(pos + token_len) + 1);
    }
}

static void build_question(const bot_config_t *config, const char *message, char *out, size_t size)
{
    char token[288];

    bot_copy_string(out, size, message);
    snprintf(token, sizeof(token), "@%s", config->username);
    remove_token(out, token);
    snprintf(token, sizeof(token), "@%s", config->display_name);
    remove_token(out, token);
    bot_sanitize_line(out);
    if (out[0] == '\0') {
        bot_copy_string(out, size, "请简单介绍你能帮我做什么。");
    }
}

static int append_prompt_context(const bot_config_t *config,
                                 bot_memory_t *memory,
                                 const char *sender,
                                 const char *question,
                                 const char *embedding_json,
                                 bot_str_t *prompt)
{
    char error[512];

    if (bot_str_appendf(prompt, "当前发言者: %s\n当前问题: %s\n\n", sender, question) != 0) {
        return -1;
    }
    (void)bot_memory_append_summary(memory, "global", "", prompt);
    (void)bot_memory_append_summary(memory, "user", sender, prompt);
    if (embedding_json != NULL && *embedding_json != '\0') {
        if (bot_memory_search(memory, "global", "", embedding_json,
                              config->memory_top_k, prompt, error, sizeof(error)) != 0) {
            fprintf(stderr, "tenet-bot: 全局向量检索失败: %s\n", error);
        }
        if (bot_memory_search(memory, "user", sender, embedding_json,
                              config->memory_top_k, prompt, error, sizeof(error)) != 0) {
            fprintf(stderr, "tenet-bot: 用户向量检索失败: %s\n", error);
        }
    }
    (void)bot_memory_append_recent_context(memory, sender, config->context_messages, prompt);
    if (bot_str_append(prompt,
                       "\n请基于以上 context、长期记忆和当前问题回答。"
                       "如果记忆不相关，不要强行引用。\n") != 0) {
        return -1;
    }
    return 0;
}

static int send_reply_chunks(int fd, const char *prefix, const char *answer)
{
    size_t prefix_len = strlen(prefix);
    size_t answer_len = strlen(answer);
    size_t pos = 0;
    int chunk_index = 0;

    if (answer_len == 0) {
        return bot_protocol_send_message(fd, prefix);
    }
    while (pos < answer_len) {
        size_t limit = BOT_REPLY_CHUNK;
        size_t take;
        size_t end;
        bot_str_t line;
        int rc;

        if (limit <= prefix_len + 16) {
            limit = prefix_len + 512;
        }
        take = answer_len - pos;
        if (take > limit - prefix_len - 16) {
            take = limit - prefix_len - 16;
            end = pos + take;
            while (end > pos && (answer[end] & 0xc0) == 0x80) {
                end--;
            }
            while (end > pos && answer[end - 1] != ' ' && answer[end - 1] != '\n') {
                end--;
            }
            if (end <= pos) {
                end = pos + take;
                while (end > pos && (answer[end] & 0xc0) == 0x80) {
                    end--;
                }
            }
            take = end > pos ? end - pos : take;
        }
        bot_str_init(&line);
        if (chunk_index == 0) {
            rc = bot_str_append(&line, prefix);
        } else {
            rc = bot_str_append(&line, prefix) == 0 ? bot_str_append(&line, "(续) ") : -1;
        }
        if (rc == 0) {
            rc = bot_str_append_len(&line, answer + pos, take);
        }
        if (rc == 0) {
            bot_sanitize_line(line.data);
            rc = bot_protocol_send_message(fd, line.data);
        }
        bot_str_free(&line);
        if (rc != 0) {
            return -1;
        }
        pos += take;
        while (pos < answer_len && answer[pos] == ' ') {
            pos++;
        }
        chunk_index++;
    }
    return 0;
}

static void update_one_summary(const bot_config_t *config,
                               bot_memory_t *memory,
                               const char *scope,
                               const char *username)
{
    bot_str_t source;
    bot_str_t prior;
    bot_str_t prompt;
    bot_str_t answer;
    char error[512];
    long long max_id = 0;
    const char *system_prompt =
        "你负责为 tenet-bot 压缩长期记忆。"
        "请保留稳定事实、用户偏好、重要上下文，删除寒暄和重复信息。"
        "输出中文要点摘要，不要编造。";

    if (!bot_memory_summary_due(memory, scope, username, config->summary_threshold)) {
        return;
    }
    bot_str_init(&source);
    bot_str_init(&prior);
    bot_str_init(&prompt);
    bot_str_init(&answer);
    if (bot_memory_collect_summary_source(memory, scope, username, 200, &source, &max_id) != 0 ||
        source.len == 0) {
        goto out;
    }
    (void)bot_memory_append_summary(memory, scope, username, &prior);
    (void)bot_str_appendf(&prompt,
                          "范围: %s %s\n"
                          "已有长期摘要:\n%s\n"
                          "请把下面的新对话合并进摘要，输出完整更新后的长期摘要。\n\n%s",
                          scope,
                          username != NULL ? username : "",
                          prior.data != NULL ? prior.data : "（暂无）",
                          source.data);
    if (bot_ollama_chat(config, system_prompt, prompt.data, &answer, error, sizeof(error)) != 0) {
        fprintf(stderr, "tenet-bot: 更新摘要失败: %s\n", error);
        goto out;
    }
    if (bot_memory_save_summary(memory, scope, username, answer.data, max_id) != 0) {
        fprintf(stderr, "tenet-bot: 保存摘要失败\n");
    }

out:
    bot_str_free(&source);
    bot_str_free(&prior);
    bot_str_free(&prompt);
    bot_str_free(&answer);
}

static void remember_exchange(const bot_config_t *config,
                              bot_memory_t *memory,
                              const char *sender,
                              const char *question,
                              const char *answer)
{
    bot_vector_t vector;
    bot_str_t memory_text;
    bot_str_t vector_json;
    char error[512];

    if (bot_memory_store_exchange(memory, sender, question, answer, error, sizeof(error)) != 0) {
        fprintf(stderr, "tenet-bot: 保存问答失败: %s\n", error);
        return;
    }

    bot_str_init(&memory_text);
    bot_str_init(&vector_json);
    vector.values = NULL;
    vector.count = 0;
    if (bot_str_appendf(&memory_text, "用户 %s 问: %s\nbot 答: %s", sender, question, answer) != 0) {
        goto out;
    }
    if (bot_ollama_embed(config, memory_text.data, &vector, error, sizeof(error)) != 0) {
        fprintf(stderr, "tenet-bot: 生成记忆 embedding 失败: %s\n", error);
        goto summaries;
    }
    if (bot_vector_to_json(&vector, &vector_json) != 0) {
        goto summaries;
    }
    if (bot_memory_add_item(memory, "global", "", memory_text.data,
                            vector_json.data, vector.count, error, sizeof(error)) != 0) {
        fprintf(stderr, "tenet-bot: 写入全局记忆失败: %s\n", error);
    }
    if (bot_memory_add_item(memory, "user", sender, memory_text.data,
                            vector_json.data, vector.count, error, sizeof(error)) != 0) {
        fprintf(stderr, "tenet-bot: 写入用户记忆失败: %s\n", error);
    }

summaries:
    update_one_summary(config, memory, "global", "");
    update_one_summary(config, memory, "user", sender);

out:
    bot_vector_free(&vector);
    bot_str_free(&memory_text);
    bot_str_free(&vector_json);
}

static void answer_message(const bot_config_t *config,
                           bot_memory_t *memory,
                           int fd,
                           const bot_chat_message_t *message)
{
    char question[BOT_MAX_MESSAGE_TEXT];
    char prefix[BOT_MAX_SENDER + 8];
    char error[512];
    bot_vector_t query_vector;
    bot_str_t query_vector_json;
    bot_str_t prompt;
    bot_str_t answer;
    const char *system_prompt =
        "你是 tenet-bot，一个运行在 tenet 终端聊天室里的 AI 助手。"
        "你能看到长期记忆、相关向量检索结果和最近上下文。"
        "优先用中文简洁回答；不知道就说不知道；不要假装看到了不存在的信息。";

    build_question(config, message->text, question, sizeof(question));
    snprintf(prefix, sizeof(prefix), "@%.200s ", message->sender);

    {
        char line[BOT_MAX_SENDER + 96];
        snprintf(line, sizeof(line), "%s收到，正在思考...", prefix);
        (void)bot_protocol_send_message(fd, line);
    }

    query_vector.values = NULL;
    query_vector.count = 0;
    bot_str_init(&query_vector_json);
    bot_str_init(&prompt);
    bot_str_init(&answer);

    if (bot_ollama_embed(config, question, &query_vector, error, sizeof(error)) == 0) {
        if (bot_vector_to_json(&query_vector, &query_vector_json) != 0) {
            query_vector_json.len = 0;
            if (query_vector_json.data != NULL) {
                query_vector_json.data[0] = '\0';
            }
        }
    } else {
        fprintf(stderr, "tenet-bot: embedding 失败，降级为摘要+最近上下文: %s\n", error);
    }

    if (append_prompt_context(config, memory, message->sender, question,
                              query_vector_json.len > 0 ? query_vector_json.data : NULL,
                              &prompt) != 0) {
        (void)bot_protocol_send_message(fd, "tenet-bot 内部错误：构造 prompt 失败。");
        goto out;
    }
    if (bot_ollama_chat(config, system_prompt, prompt.data, &answer, error, sizeof(error)) != 0) {
        char line[768];
        snprintf(line, sizeof(line), "@%.200s Ollama 调用失败：%.500s", message->sender, error);
        (void)bot_protocol_send_message(fd, line);
        goto out;
    }
    if (send_reply_chunks(fd, prefix, answer.data) != 0) {
        fprintf(stderr, "tenet-bot: 发送回复失败\n");
        goto out;
    }
    remember_exchange(config, memory, message->sender, question, answer.data);

out:
    bot_vector_free(&query_vector);
    bot_str_free(&query_vector_json);
    bot_str_free(&prompt);
    bot_str_free(&answer);
}

static void handle_screen_messages(const bot_config_t *config,
                                   bot_memory_t *memory,
                                   int fd,
                                   bot_screen_t *screen,
                                   int *primed)
{
    bot_chat_message_t messages[BOT_MAX_CHAT_MESSAGES];
    size_t count;
    size_t i;

    if (!bot_screen_chat_ready(screen)) {
        return;
    }
    count = bot_screen_extract_messages(screen, messages, BOT_MAX_CHAT_MESSAGES);
    if (!*primed) {
        for (i = 0; i < count; i++) {
            (void)bot_memory_mark_seen(memory, messages[i].fingerprint);
        }
        *primed = 1;
        fprintf(stderr, "tenet-bot: 已进入聊天室，等待 @%s\n", config->username);
        return;
    }
    for (i = 0; i < count; i++) {
        if (bot_memory_is_seen(memory, messages[i].fingerprint)) {
            continue;
        }
        (void)bot_memory_mark_seen(memory, messages[i].fingerprint);
        if (sender_is_bot(config, messages[i].sender)) {
            continue;
        }
        if (!message_mentions_bot(config, messages[i].text)) {
            continue;
        }
        answer_message(config, memory, fd, &messages[i]);
    }
}

int main(int argc, char **argv)
{
    bot_config_t config;
    bot_memory_t memory;
    bot_screen_t screen;
    char error[512];
    char buffer[8192];
    int fd;
    int parse_rc;
    int primed = 0;

    bot_config_defaults(&config);
    parse_rc = bot_config_parse(&config, argc, argv);
    if (parse_rc > 0) {
        return 0;
    }
    if (parse_rc < 0) {
        bot_config_usage(argv[0]);
        return 2;
    }

    if (bot_memory_open(&memory, &config, error, sizeof(error)) != 0) {
        fprintf(stderr, "tenet-bot: %s\n", error);
        return 1;
    }
    fd = bot_protocol_connect(config.socket_path, error, sizeof(error));
    if (fd < 0) {
        fprintf(stderr, "tenet-bot: %s\n", error);
        bot_memory_close(&memory);
        return 1;
    }
    if (bot_protocol_send_hello(fd, &config) != 0 || bot_protocol_send_enter(fd) != 0) {
        fprintf(stderr, "tenet-bot: 发送 tenet 握手失败\n");
        close(fd);
        bot_memory_close(&memory);
        return 1;
    }

    bot_screen_init(&screen);
    for (;;) {
        ssize_t got = read(fd, buffer, sizeof(buffer));
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "tenet-bot: 读取 tenet 后端失败: %s\n", strerror(errno));
            break;
        }
        if (got == 0) {
            fprintf(stderr, "tenet-bot: tenet 后端已断开\n");
            break;
        }
        bot_screen_feed(&screen, buffer, (size_t)got);
        handle_screen_messages(&config, &memory, fd, &screen, &primed);
    }

    close(fd);
    bot_memory_close(&memory);
    return 1;
}
