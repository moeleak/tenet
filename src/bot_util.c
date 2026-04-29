#include "bot_util.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BOT_FNV_OFFSET 1469598103934665603ULL
#define BOT_FNV_PRIME 1099511628211ULL

void bot_str_init(bot_str_t *str)
{
    str->data = NULL;
    str->len = 0;
    str->cap = 0;
}

void bot_str_free(bot_str_t *str)
{
    free(str->data);
    str->data = NULL;
    str->len = 0;
    str->cap = 0;
}

int bot_str_reserve(bot_str_t *str, size_t needed)
{
    char *next;
    size_t cap;

    if (needed + 1 <= str->cap) {
        return 0;
    }
    cap = str->cap > 0 ? str->cap : 256;
    while (cap < needed + 1) {
        if (cap > (size_t)-1 / 2) {
            return -1;
        }
        cap *= 2;
    }
    next = realloc(str->data, cap);
    if (next == NULL) {
        return -1;
    }
    str->data = next;
    str->cap = cap;
    if (str->len == 0) {
        str->data[0] = '\0';
    }
    return 0;
}

int bot_str_append_len(bot_str_t *str, const char *text, size_t len)
{
    if (len == 0) {
        return 0;
    }
    if (bot_str_reserve(str, str->len + len) != 0) {
        return -1;
    }
    memcpy(str->data + str->len, text, len);
    str->len += len;
    str->data[str->len] = '\0';
    return 0;
}

int bot_str_append(bot_str_t *str, const char *text)
{
    return bot_str_append_len(str, text != NULL ? text : "", strlen(text != NULL ? text : ""));
}

int bot_str_append_char(bot_str_t *str, char ch)
{
    return bot_str_append_len(str, &ch, 1);
}

int bot_str_appendf(bot_str_t *str, const char *fmt, ...)
{
    va_list ap;
    va_list copy;
    int needed;

    va_start(ap, fmt);
    va_copy(copy, ap);
    needed = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (needed < 0) {
        va_end(ap);
        return -1;
    }
    if (bot_str_reserve(str, str->len + (size_t)needed) != 0) {
        va_end(ap);
        return -1;
    }
    (void)vsnprintf(str->data + str->len, str->cap - str->len, fmt, ap);
    va_end(ap);
    str->len += (size_t)needed;
    return 0;
}

int bot_json_append_string(bot_str_t *str, const char *text)
{
    const unsigned char *cursor = (const unsigned char *)(text != NULL ? text : "");

    if (bot_str_append_char(str, '"') != 0) {
        return -1;
    }
    while (*cursor != '\0') {
        unsigned char ch = *cursor++;
        switch (ch) {
        case '"':
            if (bot_str_append(str, "\\\"") != 0) return -1;
            break;
        case '\\':
            if (bot_str_append(str, "\\\\") != 0) return -1;
            break;
        case '\b':
            if (bot_str_append(str, "\\b") != 0) return -1;
            break;
        case '\f':
            if (bot_str_append(str, "\\f") != 0) return -1;
            break;
        case '\n':
            if (bot_str_append(str, "\\n") != 0) return -1;
            break;
        case '\r':
            if (bot_str_append(str, "\\r") != 0) return -1;
            break;
        case '\t':
            if (bot_str_append(str, "\\t") != 0) return -1;
            break;
        default:
            if (ch < 0x20) {
                if (bot_str_appendf(str, "\\u%04x", ch) != 0) return -1;
            } else if (bot_str_append_char(str, (char)ch) != 0) {
                return -1;
            }
            break;
        }
    }
    return bot_str_append_char(str, '"');
}

void bot_copy_string(char *dest, size_t size, const char *src)
{
    if (size == 0) {
        return;
    }
    snprintf(dest, size, "%s", src != NULL ? src : "");
}

void bot_trim(char *text)
{
    size_t len;
    char *start;

    if (text == NULL) {
        return;
    }
    start = text;
    while (isspace((unsigned char)*start)) {
        start++;
    }
    if (start != text) {
        memmove(text, start, strlen(start) + 1);
    }
    len = strlen(text);
    while (len > 0 && isspace((unsigned char)text[len - 1])) {
        text[--len] = '\0';
    }
}

void bot_collapse_spaces(char *text)
{
    char *readp;
    char *writep;
    int in_space = 0;

    if (text == NULL) {
        return;
    }
    readp = text;
    writep = text;
    while (*readp != '\0') {
        unsigned char ch = (unsigned char)*readp++;
        if (ch == '\r' || ch == '\n' || ch == '\t' || ch == ' ') {
            if (!in_space) {
                *writep++ = ' ';
                in_space = 1;
            }
        } else if (ch < 0x20 || ch == 0x7f) {
            continue;
        } else {
            *writep++ = (char)ch;
            in_space = 0;
        }
    }
    *writep = '\0';
    bot_trim(text);
}

void bot_sanitize_line(char *text)
{
    char *cursor;

    if (text == NULL) {
        return;
    }
    for (cursor = text; *cursor != '\0'; cursor++) {
        unsigned char ch = (unsigned char)*cursor;
        if (ch == '\r' || ch == '\n' || ch == '\t') {
            *cursor = ' ';
        } else if (ch < 0x20 || ch == 0x7f) {
            *cursor = ' ';
        }
    }
    bot_collapse_spaces(text);
}

uint64_t bot_hash64(const char *text)
{
    uint64_t hash = BOT_FNV_OFFSET;
    const unsigned char *cursor = (const unsigned char *)(text != NULL ? text : "");

    while (*cursor != '\0') {
        hash ^= (uint64_t)*cursor++;
        hash *= BOT_FNV_PRIME;
    }
    return hash;
}

void bot_hash_hex(const char *text, char out[17])
{
    snprintf(out, 17, "%016llx", (unsigned long long)bot_hash64(text));
}

int bot_write_all(int fd, const void *data, size_t len)
{
    const char *bytes = data;
    size_t sent = 0;

    while (sent < len) {
        ssize_t rc = write(fd, bytes + sent, len - sent);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (rc == 0) {
            return -1;
        }
        sent += (size_t)rc;
    }
    return 0;
}

int bot_parse_positive_int(const char *text, int min_value, int max_value, int *out)
{
    char *end = NULL;
    long value;

    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < min_value || value > max_value) {
        return -1;
    }
    *out = (int)value;
    return 0;
}

char *bot_strdup_safe(const char *text)
{
    size_t len = strlen(text != NULL ? text : "");
    char *copy = malloc(len + 1);

    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, text != NULL ? text : "", len + 1);
    return copy;
}

static int hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static int append_utf8(bot_str_t *out, unsigned int codepoint)
{
    char buf[4];

    if (codepoint <= 0x7f) {
        buf[0] = (char)codepoint;
        return bot_str_append_len(out, buf, 1);
    }
    if (codepoint <= 0x7ff) {
        buf[0] = (char)(0xc0 | (codepoint >> 6));
        buf[1] = (char)(0x80 | (codepoint & 0x3f));
        return bot_str_append_len(out, buf, 2);
    }
    if (codepoint <= 0xffff) {
        buf[0] = (char)(0xe0 | (codepoint >> 12));
        buf[1] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
        buf[2] = (char)(0x80 | (codepoint & 0x3f));
        return bot_str_append_len(out, buf, 3);
    }
    buf[0] = (char)(0xf0 | (codepoint >> 18));
    buf[1] = (char)(0x80 | ((codepoint >> 12) & 0x3f));
    buf[2] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
    buf[3] = (char)(0x80 | (codepoint & 0x3f));
    return bot_str_append_len(out, buf, 4);
}

int bot_json_unescape_string(const char *start, size_t len, bot_str_t *out)
{
    size_t pos = 0;

    while (pos < len) {
        unsigned char ch = (unsigned char)start[pos++];
        if (ch != '\\') {
            if (bot_str_append_char(out, (char)ch) != 0) {
                return -1;
            }
            continue;
        }
        if (pos >= len) {
            return -1;
        }
        ch = (unsigned char)start[pos++];
        switch (ch) {
        case '"': if (bot_str_append_char(out, '"') != 0) return -1; break;
        case '\\': if (bot_str_append_char(out, '\\') != 0) return -1; break;
        case '/': if (bot_str_append_char(out, '/') != 0) return -1; break;
        case 'b': if (bot_str_append_char(out, '\b') != 0) return -1; break;
        case 'f': if (bot_str_append_char(out, '\f') != 0) return -1; break;
        case 'n': if (bot_str_append_char(out, '\n') != 0) return -1; break;
        case 'r': if (bot_str_append_char(out, '\r') != 0) return -1; break;
        case 't': if (bot_str_append_char(out, '\t') != 0) return -1; break;
        case 'u': {
            unsigned int codepoint = 0;
            int i;
            if (pos + 4 > len) {
                return -1;
            }
            for (i = 0; i < 4; i++) {
                int value = hex_value(start[pos + (size_t)i]);
                if (value < 0) {
                    return -1;
                }
                codepoint = (codepoint << 4) | (unsigned int)value;
            }
            pos += 4;
            if (append_utf8(out, codepoint) != 0) {
                return -1;
            }
            break;
        }
        default:
            return -1;
        }
    }
    return 0;
}
