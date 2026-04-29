#include "bot_config.h"
#include "bot_util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int bot_config_set_ollama_url(bot_config_t *config, const char *url, char *error, size_t error_size)
{
    char work[512];
    char *cursor;
    char *slash;
    char *colon;
    char *end = NULL;
    long port = BOT_DEFAULT_OLLAMA_PORT;

    if (url == NULL || *url == '\0') {
        return 0;
    }
    bot_copy_string(work, sizeof(work), url);
    bot_trim(work);
    cursor = work;
    if (strncmp(cursor, "http://", 7) == 0) {
        cursor += 7;
    } else if (strncmp(cursor, "https://", 8) == 0) {
        if (error_size > 0) {
            snprintf(error, error_size, "tenet-bot 只支持 HTTP Ollama URL: %s", url);
        }
        return -1;
    }
    slash = strchr(cursor, '/');
    if (slash != NULL) {
        *slash = '\0';
    }
    if (*cursor == '\0') {
        if (error_size > 0) {
            snprintf(error, error_size, "Ollama URL 缺少 host: %s", url);
        }
        return -1;
    }
    colon = strrchr(cursor, ':');
    if (colon != NULL && strchr(cursor, ':') == colon) {
        *colon++ = '\0';
        if (*colon == '\0') {
            if (error_size > 0) {
                snprintf(error, error_size, "Ollama URL 端口为空: %s", url);
            }
            return -1;
        }
        port = strtol(colon, &end, 10);
        if (end == colon || *end != '\0' || port < 1 || port > 65535) {
            if (error_size > 0) {
                snprintf(error, error_size, "Ollama URL 端口无效: %s", url);
            }
            return -1;
        }
    }
    if (*cursor == '\0') {
        if (error_size > 0) {
            snprintf(error, error_size, "Ollama URL host 为空: %s", url);
        }
        return -1;
    }
    bot_copy_string(config->ollama_host, sizeof(config->ollama_host), cursor);
    config->ollama_port = (int)port;
    snprintf(config->ollama_url, sizeof(config->ollama_url), "http://%s:%d",
             config->ollama_host, config->ollama_port);
    return 0;
}

static void strip_optional_quotes(char *text)
{
    size_t len;

    bot_trim(text);
    len = strlen(text);
    if (len >= 2 && ((text[0] == '"' && text[len - 1] == '"') ||
                     (text[0] == '\'' && text[len - 1] == '\''))) {
        memmove(text, text + 1, len - 2);
        text[len - 2] = '\0';
    }
}


static void load_env_file(const char *path)
{
    FILE *file = fopen(path, "r");
    char line[2048];

    if (file == NULL) {
        return;
    }
    while (fgets(line, sizeof(line), file) != NULL) {
        char *eq;
        char *key = line;
        char *value;

        bot_trim(line);
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }
        if (strncmp(line, "export ", 7) == 0) {
            key = line + 7;
            while (isspace((unsigned char)*key)) {
                key++;
            }
        }
        eq = strchr(key, '=');
        if (eq == NULL) {
            continue;
        }
        *eq = '\0';
        value = eq + 1;
        bot_trim(key);
        strip_optional_quotes(value);
        if (*key != '\0' && getenv(key) == NULL) {
            (void)setenv(key, value, 0);
        }
    }
    fclose(file);
}

static void copy_env_string(const char *name, char *dest, size_t size)
{
    const char *value = getenv(name);

    if (value != NULL && *value != '\0') {
        bot_copy_string(dest, size, value);
    }
}

static void copy_env_int(const char *name, int min_value, int max_value, int *dest)
{
    const char *value = getenv(name);
    int parsed;

    if (value != NULL && *value != '\0' &&
        bot_parse_positive_int(value, min_value, max_value, &parsed) == 0) {
        *dest = parsed;
    }
}

static void copy_env_url(bot_config_t *config, const char *name)
{
    const char *value = getenv(name);
    char error[256];

    if (value != NULL && *value != '\0' &&
        bot_config_set_ollama_url(config, value, error, sizeof(error)) != 0) {
        fprintf(stderr, "tenet-bot: 忽略无效 %s: %s\n", name, error);
    }
}

void bot_config_defaults(bot_config_t *config)
{
    bot_copy_string(config->socket_path, sizeof(config->socket_path), BOT_DEFAULT_SOCKET_PATH);
    bot_copy_string(config->memory_db_path, sizeof(config->memory_db_path), BOT_DEFAULT_MEMORY_DB);
    config->vector_extension_path[0] = '\0';
    config->vss_extension_path[0] = '\0';
    bot_copy_string(config->username, sizeof(config->username), BOT_DEFAULT_USERNAME);
    bot_copy_string(config->display_name, sizeof(config->display_name), BOT_DEFAULT_DISPLAY_NAME);
    bot_copy_string(config->ollama_url, sizeof(config->ollama_url), BOT_DEFAULT_OLLAMA_URL);
    bot_copy_string(config->ollama_host, sizeof(config->ollama_host), BOT_DEFAULT_OLLAMA_HOST);
    config->ollama_port = BOT_DEFAULT_OLLAMA_PORT;
    bot_copy_string(config->chat_model, sizeof(config->chat_model), BOT_DEFAULT_CHAT_MODEL);
    bot_copy_string(config->embed_model, sizeof(config->embed_model), BOT_DEFAULT_EMBED_MODEL);
    config->context_messages = BOT_DEFAULT_CONTEXT_MESSAGES;
    config->memory_top_k = BOT_DEFAULT_MEMORY_TOP_K;
    config->summary_threshold = BOT_DEFAULT_SUMMARY_THRESHOLD;
    config->reset_memory = 0;

    load_env_file(".env");

    copy_env_string("TENET_BOT_SOCKET", config->socket_path, sizeof(config->socket_path));
    copy_env_string("TENET_BOT_MEMORY_DB", config->memory_db_path, sizeof(config->memory_db_path));
    copy_env_string("TENET_BOT_VECTOR_EXTENSION", config->vector_extension_path, sizeof(config->vector_extension_path));
    copy_env_string("TENET_BOT_VSS_EXTENSION", config->vss_extension_path, sizeof(config->vss_extension_path));
    copy_env_string("TENET_BOT_USERNAME", config->username, sizeof(config->username));
    copy_env_string("TENET_BOT_DISPLAY_NAME", config->display_name, sizeof(config->display_name));
    copy_env_url(config, "OLLAMA_HOST");
    copy_env_url(config, "OLLAMA_URL");
    copy_env_url(config, "TENET_BOT_OLLAMA_URL");
    copy_env_string("TENET_BOT_OLLAMA_HOST", config->ollama_host, sizeof(config->ollama_host));
    copy_env_int("TENET_BOT_OLLAMA_PORT", 1, 65535, &config->ollama_port);
    snprintf(config->ollama_url, sizeof(config->ollama_url), "http://%s:%d",
             config->ollama_host, config->ollama_port);
    copy_env_string("TENET_BOT_CHAT_MODEL", config->chat_model, sizeof(config->chat_model));
    copy_env_string("TENET_BOT_EMBED_MODEL", config->embed_model, sizeof(config->embed_model));
    copy_env_int("TENET_BOT_CONTEXT_MESSAGES", 1, 500, &config->context_messages);
    copy_env_int("TENET_BOT_MEMORY_TOP_K", 0, 100, &config->memory_top_k);
    copy_env_int("TENET_BOT_SUMMARY_THRESHOLD", 1, 1000, &config->summary_threshold);
}

void bot_config_usage(const char *argv0)
{
    fprintf(stdout,
            "用法: %s [选项]\n\n"
            "选项:\n"
            "  --socket PATH              tenet Unix socket，默认 %s\n"
            "  --memory-db PATH           SQLite 记忆库，默认 %s\n"
            "  --vector-extension PATH    sqlite-vss 的 vector0 扩展路径，可选\n"
            "  --vss-extension PATH       sqlite-vss 的 vss0 扩展路径，可选\n"
            "  --username NAME            bot 用户名，默认 %s\n"
            "  --display-name NAME        bot 显示名，默认 %s\n"
            "  --ollama-url URL           Ollama URL，默认 %s\n"
            "  --ollama-host HOST         Ollama 主机，默认 %s\n"
            "  --ollama-port PORT         Ollama 端口，默认 %d\n"
            "  --chat-model MODEL         聊天模型，默认 %s\n"
            "  --embed-model MODEL        embedding 模型，默认 %s\n"
            "  --context-messages N       最近上下文条数，默认 %d\n"
            "  --memory-top-k N           全局/用户各取 top K，默认 %d\n"
            "  --summary-threshold N      每 N 条问答压缩摘要，默认 %d\n"
            "  --reset-memory             启动前清空记忆库\n"
            "  -h, --help                 显示帮助\n",
            argv0,
            BOT_DEFAULT_SOCKET_PATH,
            BOT_DEFAULT_MEMORY_DB,
            BOT_DEFAULT_USERNAME,
            BOT_DEFAULT_DISPLAY_NAME,
            BOT_DEFAULT_OLLAMA_URL,
            BOT_DEFAULT_OLLAMA_HOST,
            BOT_DEFAULT_OLLAMA_PORT,
            BOT_DEFAULT_CHAT_MODEL,
            BOT_DEFAULT_EMBED_MODEL,
            BOT_DEFAULT_CONTEXT_MESSAGES,
            BOT_DEFAULT_MEMORY_TOP_K,
            BOT_DEFAULT_SUMMARY_THRESHOLD);
}

static int need_arg(int argc, char **argv, int index)
{
    if (index + 1 >= argc) {
        fprintf(stderr, "%s 需要参数\n", argv[index]);
        return -1;
    }
    return 0;
}

int bot_config_parse(bot_config_t *config, int argc, char **argv)
{
    int index;

    for (index = 1; index < argc; index++) {
        if (strcmp(argv[index], "--socket") == 0) {
            if (need_arg(argc, argv, index) != 0) return -1;
            bot_copy_string(config->socket_path, sizeof(config->socket_path), argv[++index]);
        } else if (strcmp(argv[index], "--memory-db") == 0) {
            if (need_arg(argc, argv, index) != 0) return -1;
            bot_copy_string(config->memory_db_path, sizeof(config->memory_db_path), argv[++index]);
        } else if (strcmp(argv[index], "--vector-extension") == 0) {
            if (need_arg(argc, argv, index) != 0) return -1;
            bot_copy_string(config->vector_extension_path, sizeof(config->vector_extension_path), argv[++index]);
        } else if (strcmp(argv[index], "--vss-extension") == 0) {
            if (need_arg(argc, argv, index) != 0) return -1;
            bot_copy_string(config->vss_extension_path, sizeof(config->vss_extension_path), argv[++index]);
        } else if (strcmp(argv[index], "--username") == 0) {
            if (need_arg(argc, argv, index) != 0) return -1;
            bot_copy_string(config->username, sizeof(config->username), argv[++index]);
        } else if (strcmp(argv[index], "--display-name") == 0) {
            if (need_arg(argc, argv, index) != 0) return -1;
            bot_copy_string(config->display_name, sizeof(config->display_name), argv[++index]);
        } else if (strcmp(argv[index], "--ollama-url") == 0) {
            char error[256];
            if (need_arg(argc, argv, index) != 0) return -1;
            if (bot_config_set_ollama_url(config, argv[++index], error, sizeof(error)) != 0) {
                fprintf(stderr, "%s\n", error);
                return -1;
            }
        } else if (strcmp(argv[index], "--ollama-host") == 0) {
            if (need_arg(argc, argv, index) != 0) return -1;
            bot_copy_string(config->ollama_host, sizeof(config->ollama_host), argv[++index]);
            snprintf(config->ollama_url, sizeof(config->ollama_url), "http://%s:%d",
                     config->ollama_host, config->ollama_port);
        } else if (strcmp(argv[index], "--ollama-port") == 0) {
            if (need_arg(argc, argv, index) != 0) return -1;
            if (bot_parse_positive_int(argv[++index], 1, 65535, &config->ollama_port) != 0) {
                fprintf(stderr, "Ollama 端口无效: %s\n", argv[index]);
                return -1;
            }
            snprintf(config->ollama_url, sizeof(config->ollama_url), "http://%s:%d",
                     config->ollama_host, config->ollama_port);
        } else if (strcmp(argv[index], "--chat-model") == 0) {
            if (need_arg(argc, argv, index) != 0) return -1;
            bot_copy_string(config->chat_model, sizeof(config->chat_model), argv[++index]);
        } else if (strcmp(argv[index], "--embed-model") == 0) {
            if (need_arg(argc, argv, index) != 0) return -1;
            bot_copy_string(config->embed_model, sizeof(config->embed_model), argv[++index]);
        } else if (strcmp(argv[index], "--context-messages") == 0) {
            if (need_arg(argc, argv, index) != 0) return -1;
            if (bot_parse_positive_int(argv[++index], 1, 500, &config->context_messages) != 0) {
                fprintf(stderr, "上下文条数无效: %s\n", argv[index]);
                return -1;
            }
        } else if (strcmp(argv[index], "--memory-top-k") == 0) {
            if (need_arg(argc, argv, index) != 0) return -1;
            if (bot_parse_positive_int(argv[++index], 0, 100, &config->memory_top_k) != 0) {
                fprintf(stderr, "记忆 top K 无效: %s\n", argv[index]);
                return -1;
            }
        } else if (strcmp(argv[index], "--summary-threshold") == 0) {
            if (need_arg(argc, argv, index) != 0) return -1;
            if (bot_parse_positive_int(argv[++index], 1, 1000, &config->summary_threshold) != 0) {
                fprintf(stderr, "摘要阈值无效: %s\n", argv[index]);
                return -1;
            }
        } else if (strcmp(argv[index], "--reset-memory") == 0) {
            config->reset_memory = 1;
        } else if (strcmp(argv[index], "-h") == 0 || strcmp(argv[index], "--help") == 0) {
            bot_config_usage(argv[0]);
            return 1;
        } else {
            fprintf(stderr, "未知选项: %s\n", argv[index]);
            return -1;
        }
    }
    return 0;
}
