#ifndef TENET_BOT_MEMORY_H
#define TENET_BOT_MEMORY_H

#include "bot_config.h"
#include "bot_util.h"

#include <stddef.h>
#include <sqlite3.h>

typedef struct bot_memory {
    sqlite3 *db;
    int vector_dim;
    int vss_enabled;
} bot_memory_t;

typedef struct bot_memory_chat_message {
    char role[16];
    char *content;
} bot_memory_chat_message_t;

typedef struct bot_memory_chat_history {
    bot_memory_chat_message_t *items;
    size_t count;
    size_t cap;
} bot_memory_chat_history_t;

int bot_memory_open(bot_memory_t *memory,
                    const bot_config_t *config,
                    char *error,
                    size_t error_size);
void bot_memory_close(bot_memory_t *memory);
void bot_memory_chat_history_init(bot_memory_chat_history_t *history);
void bot_memory_chat_history_free(bot_memory_chat_history_t *history);
int bot_memory_is_seen(bot_memory_t *memory, const char *fingerprint);
int bot_memory_mark_seen(bot_memory_t *memory, const char *fingerprint);
int bot_memory_store_observed_message(bot_memory_t *memory,
                                      const char *sender,
                                      const char *target_user,
                                      const char *content,
                                      char *error,
                                      size_t error_size);
int bot_memory_store_answer(bot_memory_t *memory,
                            const char *target_user,
                            const char *answer,
                            char *error,
                            size_t error_size);
int bot_memory_store_exchange(bot_memory_t *memory,
                              const char *sender,
                              const char *question,
                              const char *answer,
                              char *error,
                              size_t error_size);
int bot_memory_add_item(bot_memory_t *memory,
                        const char *scope,
                        const char *username,
                        const char *content,
                        const char *embedding_json,
                        size_t embedding_dim,
                        char *error,
                        size_t error_size);
int bot_memory_search(bot_memory_t *memory,
                      const char *scope,
                      const char *username,
                      const char *embedding_json,
                      int top_k,
                      bot_str_t *out,
                      char *error,
                      size_t error_size);
int bot_memory_append_summary(bot_memory_t *memory,
                              const char *scope,
                              const char *username,
                              bot_str_t *out);
int bot_memory_append_recent_context(bot_memory_t *memory,
                                     const char *username,
                                     int limit,
                                     bot_str_t *out);
int bot_memory_load_recent_chat_history(bot_memory_t *memory,
                                        const char *username,
                                        int include_private,
                                        int limit,
                                        bot_memory_chat_history_t *history,
                                        char *error,
                                        size_t error_size);
int bot_memory_summary_due(bot_memory_t *memory,
                           const char *scope,
                           const char *username,
                           int threshold);
int bot_memory_collect_summary_source(bot_memory_t *memory,
                                      const char *scope,
                                      const char *username,
                                      int limit,
                                      bot_str_t *out,
                                      long long *max_message_id);
int bot_memory_save_summary(bot_memory_t *memory,
                            const char *scope,
                            const char *username,
                            const char *summary,
                            long long updated_message_id);

#endif
