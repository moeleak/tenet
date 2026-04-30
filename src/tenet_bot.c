#include "bot_config.h"
#include "bot_memory.h"
#include "bot_ollama.h"
#include "bot_protocol.h"
#include "bot_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define BOT_REPLY_CHUNK 1500
#define BOT_RECENT_CONTEXT_HARD_CAP 16

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

static int build_reference_context(const bot_config_t *config,
                                   bot_memory_t *memory,
                                   const char *sender,
                                   const char *embedding_json,
                                   bot_str_t *context)
{
    char error[512];

    if (bot_str_appendf(context, "当前发言者: %s\n", sender) != 0) {
        return -1;
    }
    (void)bot_memory_append_summary(memory, "global", "", context);
    (void)bot_memory_append_summary(memory, "user", sender, context);
    if (embedding_json != NULL && *embedding_json != '\0') {
        if (bot_memory_search(memory, "global", "", embedding_json,
                              config->memory_top_k, context, error, sizeof(error)) != 0) {
            fprintf(stderr, "tenet-bot: 全局向量检索失败: %s\n", error);
        }
        if (bot_memory_search(memory, "user", sender, embedding_json,
                              config->memory_top_k, context, error, sizeof(error)) != 0) {
            fprintf(stderr, "tenet-bot: 用户向量检索失败: %s\n", error);
        }
    }
    return 0;
}

static int build_system_message(const char *base_prompt,
                                const bot_str_t *reference_context,
                                bot_str_t *system_message)
{
    if (bot_str_append(system_message, base_prompt) != 0 ||
        bot_str_append(system_message,
                       "\n历史对话会通过 API messages 数组传入；按 messages 的先后顺序理解上下文。"
                       "不要把参考资料当成用户原话，不要复读历史 assistant 回复。") != 0) {
        return -1;
    }
    if (reference_context != NULL && reference_context->data != NULL && reference_context->len > 0) {
        if (bot_str_append(system_message, "\n\n参考资料（只用于辅助理解当前问题，不要逐字复述）:\n") != 0 ||
            bot_str_append(system_message, reference_context->data) != 0) {
            return -1;
        }
    }
    return 0;
}

static int load_chat_messages(const bot_config_t *config,
                              bot_memory_t *memory,
                              const bot_event_t *event,
                              const char *fallback_question,
                              const char *system_message,
                              bot_memory_chat_history_t *history,
                              bot_ollama_message_t **messages_out,
                              size_t *message_count_out,
                              char *error,
                              size_t error_size)
{
    bot_ollama_message_t *messages;
    size_t index;
    size_t count;
    int history_has_current = 0;
    int recent_limit = config->context_messages;

    *messages_out = NULL;
    *message_count_out = 0;
    if (recent_limit > BOT_RECENT_CONTEXT_HARD_CAP) {
        recent_limit = BOT_RECENT_CONTEXT_HARD_CAP;
    }
    if (bot_memory_load_recent_chat_history(memory, event->sender, event->private_chat,
                                            recent_limit, history, error, error_size) != 0) {
        return -1;
    }
    if (history->count > 0 &&
        strcmp(history->items[history->count - 1].role, "user") == 0 &&
        strstr(history->items[history->count - 1].content, event->text) != NULL) {
        history_has_current = 1;
    }
    count = 1 + history->count + (history_has_current ? 0 : 1);
    messages = calloc(count, sizeof(messages[0]));
    if (messages == NULL) {
        snprintf(error, error_size, "构造聊天 messages 失败: 内存不足");
        return -1;
    }
    messages[0].role = "system";
    messages[0].content = system_message;
    for (index = 0; index < history->count; index++) {
        messages[index + 1].role = history->items[index].role;
        messages[index + 1].content = history->items[index].content;
    }
    if (history_has_current) {
        messages[history->count].content = fallback_question;
    } else {
        messages[history->count + 1].role = "user";
        messages[history->count + 1].content = fallback_question;
    }
    *messages_out = messages;
    *message_count_out = count;
    return 0;
}

static int send_reply_line(int fd, int private_chat, const char *target_username, const char *line)
{
    if (private_chat) {
        return bot_protocol_send_private(fd, target_username, line);
    }
    return bot_protocol_send_chat(fd, line);
}

static int send_reply_chunks(int fd,
                             const char *prefix,
                             const char *answer,
                             int private_chat,
                             const char *target_username)
{
    size_t prefix_len = strlen(prefix);
    size_t answer_len = strlen(answer);
    size_t pos = 0;
    int chunk_index = 0;

    if (answer_len == 0) {
        return send_reply_line(fd, private_chat, target_username, prefix);
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
            rc = send_reply_line(fd, private_chat, target_username, line.data);
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
                              const char *target_user,
                              const char *question,
                              const char *answer)
{
    bot_vector_t vector;
    bot_str_t memory_text;
    bot_str_t vector_json;
    char error[512];

    if (bot_memory_store_answer(memory, target_user, answer, error, sizeof(error)) != 0) {
        fprintf(stderr, "tenet-bot: 保存回复失败: %s\n", error);
        return;
    }

    bot_str_init(&memory_text);
    bot_str_init(&vector_json);
    vector.values = NULL;
    vector.count = 0;
    if (bot_str_appendf(&memory_text, "用户 %s 问过: %s", sender, question) != 0) {
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
                           const bot_event_t *message)
{
    char question[BOT_MAX_MESSAGE_TEXT];
    char prefix[BOT_MAX_SENDER + 8];
    char error[512];
    bot_vector_t query_vector;
    bot_str_t query_vector_json;
    bot_str_t reference_context;
    bot_str_t system_message;
    bot_str_t answer;
    bot_memory_chat_history_t history;
    bot_ollama_message_t *messages = NULL;
    size_t message_count = 0;
    const char *system_prompt =
        "你是 tenet-bot，一个运行在 tenet 终端聊天室里的 AI 助手。"
        "你能看到长期记忆、相关向量检索结果和通过 API messages 传入的最近对话。"
        "优先用中文简洁直接回答当前问题；不知道就说不知道。"
        "不要角色扮演、卖萌、自称宝宝或模仿历史回复语气；即使显示名像角色名也只当作昵称。"
        "不要复述无关上下文，不要假装看到了不存在的信息。"
        "不要输出 <think> 标签、推理过程或内部思考。";

    build_question(config, message->text, question, sizeof(question));
    if (message->private_chat) {
        prefix[0] = '\0';
    } else {
        snprintf(prefix, sizeof(prefix), "@%.200s ", message->sender);
    }

    query_vector.values = NULL;
    query_vector.count = 0;
    bot_str_init(&query_vector_json);
    bot_str_init(&reference_context);
    bot_str_init(&system_message);
    bot_str_init(&answer);
    bot_memory_chat_history_init(&history);

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

    if (build_reference_context(config, memory, message->sender,
                                query_vector_json.len > 0 ? query_vector_json.data : NULL,
                                &reference_context) != 0 ||
        build_system_message(system_prompt, &reference_context, &system_message) != 0) {
        char line[256];

        snprintf(line, sizeof(line), "%stenet-bot 内部错误：构造 system prompt 失败。", prefix);
        (void)send_reply_line(fd, message->private_chat, message->sender, line);
        goto out;
    }
    if (load_chat_messages(config, memory, message, question, system_message.data,
                           &history, &messages, &message_count,
                           error, sizeof(error)) != 0) {
        char line[768];

        snprintf(line, sizeof(line), "%stenet-bot 内部错误：读取上下文失败：%.500s", prefix, error);
        (void)send_reply_line(fd, message->private_chat, message->sender, line);
        goto out;
    }
    if (bot_ollama_chat_messages(config, messages, message_count, &answer, error, sizeof(error)) != 0) {
        char line[768];

        snprintf(line, sizeof(line), "%sOllama 调用失败：%.650s", prefix, error);
        (void)send_reply_line(fd, message->private_chat, message->sender, line);
        goto out;
    }
    if (send_reply_chunks(fd, prefix, answer.data, message->private_chat, message->sender) != 0) {
        fprintf(stderr, "tenet-bot: 发送回复失败\n");
        goto out;
    }
    remember_exchange(config, memory, message->sender,
                      message->private_chat ? message->sender : "",
                      question, answer.data);

out:
    free(messages);
    bot_memory_chat_history_free(&history);
    bot_vector_free(&query_vector);
    bot_str_free(&query_vector_json);
    bot_str_free(&reference_context);
    bot_str_free(&system_message);
    bot_str_free(&answer);
}

static void handle_event(const bot_config_t *config,
                         bot_memory_t *memory,
                         int fd,
                         const bot_event_t *event)
{
    char error[512];

    if (sender_is_bot(config, event->sender) || sender_is_bot(config, event->sender_display)) {
        return;
    }
    if (bot_memory_store_observed_message(memory, event->sender,
                                          event->private_chat ? event->sender : "",
                                          event->text,
                                          error, sizeof(error)) != 0) {
        fprintf(stderr, "tenet-bot: 保存最近上下文失败: %s\n", error);
    }
    if (!event->private_chat && !message_mentions_bot(config, event->text)) {
        return;
    }
    answer_message(config, memory, fd, event);
}

int main(int argc, char **argv)
{
    bot_config_t config;
    bot_memory_t memory;
    char error[512];
    int fd;
    int parse_rc;

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
    if (bot_protocol_send_hello(fd, &config) != 0) {
        fprintf(stderr, "tenet-bot: 发送 bot 协议握手失败\n");
        close(fd);
        bot_memory_close(&memory);
        return 1;
    }

    fprintf(stderr, "tenet-bot: 已连接，使用结构化 bot 协议，等待 @%s 或私聊\n", config.username);
    for (;;) {
        bot_event_t event;
        int rc = bot_protocol_read_event(fd, &event, error, sizeof(error));

        if (rc < 0) {
            fprintf(stderr, "tenet-bot: %s\n", error);
            break;
        }
        if (rc == 0) {
            fprintf(stderr, "tenet-bot: tenet 后端已断开\n");
            break;
        }
        handle_event(&config, &memory, fd, &event);
    }

    close(fd);
    bot_memory_close(&memory);
    return 1;
}
