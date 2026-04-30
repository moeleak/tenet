#ifndef TENET_BOT_OLLAMA_H
#define TENET_BOT_OLLAMA_H

#include "bot_config.h"
#include "bot_util.h"

#include <stddef.h>

typedef struct bot_vector {
    double *values;
    size_t count;
} bot_vector_t;

typedef struct bot_ollama_message {
    const char *role;
    const char *content;
} bot_ollama_message_t;

void bot_vector_free(bot_vector_t *vector);
int bot_vector_to_json(const bot_vector_t *vector, bot_str_t *json);
int bot_ollama_embed(const bot_config_t *config,
                     const char *text,
                     bot_vector_t *vector,
                     char *error,
                     size_t error_size);
int bot_ollama_chat_messages(const bot_config_t *config,
                             const bot_ollama_message_t *messages,
                             size_t message_count,
                             bot_str_t *answer,
                             char *error,
                             size_t error_size);
int bot_ollama_chat(const bot_config_t *config,
                    const char *system_prompt,
                    const char *user_prompt,
                    bot_str_t *answer,
                    char *error,
                    size_t error_size);

#endif
