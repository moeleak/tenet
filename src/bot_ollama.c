#include "bot_ollama.h"
#include "bot_http.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_error(char *error, size_t error_size, const char *message)
{
    if (error_size > 0) {
        snprintf(error, error_size, "%s", message != NULL ? message : "未知错误");
    }
}

static char *find_ascii_case_insensitive(char *haystack, const char *needle)
{
    size_t needle_len = strlen(needle);

    if (needle_len == 0) {
        return haystack;
    }
    for (; *haystack != '\0'; haystack++) {
        size_t index;

        for (index = 0; index < needle_len; index++) {
            if (haystack[index] == '\0' ||
                tolower((unsigned char)haystack[index]) !=
                tolower((unsigned char)needle[index])) {
                break;
            }
        }
        if (index == needle_len) {
            return haystack;
        }
    }
    return NULL;
}

static void strip_think_blocks(char *text)
{
    char *start;

    if (text == NULL) {
        return;
    }
    while ((start = find_ascii_case_insensitive(text, "<think")) != NULL) {
        char *end = find_ascii_case_insensitive(start, "</think>");

        if (end == NULL) {
            *start = '\0';
            break;
        }
        end += strlen("</think>");
        memmove(start, end, strlen(end) + 1);
    }
}

void bot_vector_free(bot_vector_t *vector)
{
    free(vector->values);
    vector->values = NULL;
    vector->count = 0;
}

int bot_vector_to_json(const bot_vector_t *vector, bot_str_t *json)
{
    size_t i;

    if (bot_str_append_char(json, '[') != 0) {
        return -1;
    }
    for (i = 0; i < vector->count; i++) {
        if (i > 0 && bot_str_append_char(json, ',') != 0) {
            return -1;
        }
        if (bot_str_appendf(json, "%.17g", vector->values[i]) != 0) {
            return -1;
        }
    }
    return bot_str_append_char(json, ']');
}

static const char *skip_ws(const char *cursor)
{
    while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
        cursor++;
    }
    return cursor;
}

static const char *find_json_key(const char *json, const char *key)
{
    bot_str_t quoted;
    const char *found;

    bot_str_init(&quoted);
    if (bot_json_append_string(&quoted, key) != 0) {
        bot_str_free(&quoted);
        return NULL;
    }
    found = strstr(json, quoted.data);
    bot_str_free(&quoted);
    return found;
}

static int extract_json_string_value(const char *json, const char *key, bot_str_t *out)
{
    const char *keypos = find_json_key(json, key);
    const char *cursor;
    const char *start;
    int escaped = 0;

    if (keypos == NULL) {
        return -1;
    }
    cursor = strchr(keypos, ':');
    if (cursor == NULL) {
        return -1;
    }
    cursor = skip_ws(cursor + 1);
    if (*cursor != '"') {
        return -1;
    }
    start = ++cursor;
    while (*cursor != '\0') {
        if (escaped) {
            escaped = 0;
        } else if (*cursor == '\\') {
            escaped = 1;
        } else if (*cursor == '"') {
            return bot_json_unescape_string(start, (size_t)(cursor - start), out);
        }
        cursor++;
    }
    return -1;
}

static int parse_embed_vector(const char *json, bot_vector_t *vector)
{
    const char *cursor = find_json_key(json, "embeddings");
    double *values = NULL;
    size_t count = 0;
    size_t cap = 0;

    if (cursor == NULL) {
        return -1;
    }
    cursor = strchr(cursor, '[');
    if (cursor == NULL) {
        return -1;
    }
    cursor = strchr(cursor + 1, '[');
    if (cursor == NULL) {
        return -1;
    }
    cursor++;
    for (;;) {
        char *endptr;
        double value;

        cursor = skip_ws(cursor);
        if (*cursor == ']') {
            break;
        }
        errno = 0;
        value = strtod(cursor, &endptr);
        if (errno != 0 || endptr == cursor) {
            free(values);
            return -1;
        }
        if (count == cap) {
            double *next;
            cap = cap > 0 ? cap * 2 : 512;
            next = realloc(values, cap * sizeof(values[0]));
            if (next == NULL) {
                free(values);
                return -1;
            }
            values = next;
        }
        values[count++] = value;
        cursor = skip_ws(endptr);
        if (*cursor == ',') {
            cursor++;
            continue;
        }
        if (*cursor == ']') {
            break;
        }
        free(values);
        return -1;
    }
    if (count == 0) {
        free(values);
        return -1;
    }
    vector->values = values;
    vector->count = count;
    return 0;
}

int bot_ollama_embed(const bot_config_t *config,
                     const char *text,
                     bot_vector_t *vector,
                     char *error,
                     size_t error_size)
{
    bot_str_t body;
    bot_str_t response;
    int status = 0;
    int rc = -1;

    vector->values = NULL;
    vector->count = 0;
    bot_str_init(&body);
    bot_str_init(&response);
    if (bot_str_append(&body, "{\"model\":") != 0 ||
        bot_json_append_string(&body, config->embed_model) != 0 ||
        bot_str_append(&body, ",\"input\":") != 0 ||
        bot_json_append_string(&body, text) != 0 ||
        bot_str_append(&body, "}") != 0) {
        set_error(error, error_size, "构造 embedding 请求失败");
        goto out;
    }
    if (bot_http_post_json(config->ollama_host, config->ollama_port, "/api/embed",
                           body.data, 120, &status, &response, error, error_size) != 0) {
        goto out;
    }
    if (status < 200 || status >= 300) {
        snprintf(error, error_size, "Ollama embedding HTTP 状态 %d: %.200s", status,
                 response.data != NULL ? response.data : "");
        goto out;
    }
    if (parse_embed_vector(response.data != NULL ? response.data : "", vector) != 0) {
        set_error(error, error_size, "解析 embedding 向量失败");
        goto out;
    }
    rc = 0;

out:
    bot_str_free(&body);
    bot_str_free(&response);
    return rc;
}

int bot_ollama_chat(const bot_config_t *config,
                    const char *system_prompt,
                    const char *user_prompt,
                    bot_str_t *answer,
                    char *error,
                    size_t error_size)
{
    bot_str_t body;
    bot_str_t response;
    int status = 0;
    int rc = -1;

    bot_str_init(&body);
    bot_str_init(&response);
    if (bot_str_append(&body, "{\"model\":") != 0 ||
        bot_json_append_string(&body, config->chat_model) != 0 ||
        bot_str_append(&body, ",\"stream\":false,\"messages\":[{\"role\":\"system\",\"content\":") != 0 ||
        bot_json_append_string(&body, system_prompt) != 0 ||
        bot_str_append(&body, "},{\"role\":\"user\",\"content\":") != 0 ||
        bot_json_append_string(&body, user_prompt) != 0 ||
        bot_str_append(&body, "}]}") != 0) {
        set_error(error, error_size, "构造聊天请求失败");
        goto out;
    }
    if (bot_http_post_json(config->ollama_host, config->ollama_port, "/api/chat",
                           body.data, 300, &status, &response, error, error_size) != 0) {
        goto out;
    }
    if (status < 200 || status >= 300) {
        snprintf(error, error_size, "Ollama chat HTTP 状态 %d: %.200s", status,
                 response.data != NULL ? response.data : "");
        goto out;
    }
    if (extract_json_string_value(response.data != NULL ? response.data : "", "content", answer) != 0) {
        set_error(error, error_size, "解析聊天回复失败");
        goto out;
    }
    strip_think_blocks(answer->data);
    bot_sanitize_line(answer->data);
    if ((answer->data == NULL || answer->data[0] == '\0') &&
        bot_str_append(answer, "（模型没有返回可显示内容）") != 0) {
        set_error(error, error_size, "保存聊天回复失败");
        goto out;
    }
    answer->len = strlen(answer->data);
    rc = 0;

out:
    bot_str_free(&body);
    bot_str_free(&response);
    return rc;
}
