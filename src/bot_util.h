#ifndef TENET_BOT_UTIL_H
#define TENET_BOT_UTIL_H

#include <stddef.h>
#include <stdint.h>

typedef struct bot_str {
    char *data;
    size_t len;
    size_t cap;
} bot_str_t;

void bot_str_init(bot_str_t *str);
void bot_str_free(bot_str_t *str);
int bot_str_reserve(bot_str_t *str, size_t needed);
int bot_str_append_len(bot_str_t *str, const char *text, size_t len);
int bot_str_append(bot_str_t *str, const char *text);
int bot_str_append_char(bot_str_t *str, char ch);
int bot_str_appendf(bot_str_t *str, const char *fmt, ...);
int bot_json_append_string(bot_str_t *str, const char *text);

void bot_copy_string(char *dest, size_t size, const char *src);
void bot_trim(char *text);
void bot_collapse_spaces(char *text);
void bot_sanitize_line(char *text);
uint64_t bot_hash64(const char *text);
void bot_hash_hex(const char *text, char out[17]);
int bot_write_all(int fd, const void *data, size_t len);
int bot_parse_positive_int(const char *text, int min_value, int max_value, int *out);
char *bot_strdup_safe(const char *text);
int bot_json_unescape_string(const char *start, size_t len, bot_str_t *out);

#endif
