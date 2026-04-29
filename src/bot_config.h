#ifndef TENET_BOT_CONFIG_H
#define TENET_BOT_CONFIG_H

#include <stddef.h>

#define BOT_DEFAULT_SOCKET_PATH "/tmp/tenet.sock"
#define BOT_DEFAULT_MEMORY_DB "tenet-bot-memory.sqlite3"
#define BOT_DEFAULT_USERNAME "tenet-bot"
#define BOT_DEFAULT_DISPLAY_NAME "tenet-bot"
#define BOT_DEFAULT_OLLAMA_URL "http://127.0.0.1:11434"
#define BOT_DEFAULT_OLLAMA_HOST "127.0.0.1"
#define BOT_DEFAULT_OLLAMA_PORT 11434
#define BOT_DEFAULT_CHAT_MODEL "qwen3.5:9b"
#define BOT_DEFAULT_EMBED_MODEL "qwen3-embedding:4b"
#define BOT_DEFAULT_CONTEXT_MESSAGES 50
#define BOT_DEFAULT_MEMORY_TOP_K 6
#define BOT_DEFAULT_SUMMARY_THRESHOLD 20
#define BOT_SCREEN_ROWS 200
#define BOT_SCREEN_COLS 160

typedef struct bot_config {
    char socket_path[108];
    char memory_db_path[512];
    char vector_extension_path[512];
    char vss_extension_path[512];
    char username[64];
    char display_name[256];
    char ollama_url[512];
    char ollama_host[256];
    int ollama_port;
    char chat_model[128];
    char embed_model[128];
    int context_messages;
    int memory_top_k;
    int summary_threshold;
    int reset_memory;
} bot_config_t;

void bot_config_defaults(bot_config_t *config);
int bot_config_parse(bot_config_t *config, int argc, char **argv);
void bot_config_usage(const char *argv0);

#endif
