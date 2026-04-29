#include "bot_screen.h"
#include "bot_util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void bot_screen_init(bot_screen_t *screen)
{
    memset(screen, 0, sizeof(*screen));
    screen->cursor_row = 1;
    screen->cursor_col = 1;
}

static void row_clear(bot_screen_t *screen, int row)
{
    if (row < 1 || row > BOT_SCREEN_ROWS) {
        return;
    }
    screen->rows[row][0] = '\0';
    screen->row_lens[row] = 0;
}

static void screen_clear(bot_screen_t *screen)
{
    int row;

    for (row = 1; row <= BOT_SCREEN_ROWS; row++) {
        row_clear(screen, row);
    }
    screen->cursor_row = 1;
    screen->cursor_col = 1;
}

static void row_append(bot_screen_t *screen, unsigned char byte)
{
    char *row;
    size_t len;
    size_t index;
    size_t cap;

    if (screen->cursor_row < 1 || screen->cursor_row > BOT_SCREEN_ROWS) {
        return;
    }
    if (screen->cursor_col < 1) {
        screen->cursor_col = 1;
    }
    row = screen->rows[screen->cursor_row];
    len = screen->row_lens[screen->cursor_row];
    index = (size_t)(screen->cursor_col - 1);
    cap = sizeof(screen->rows[screen->cursor_row]);
    if (index + 1 >= cap) {
        return;
    }
    while (len < index) {
        row[len++] = ' ';
    }
    row[index] = (char)byte;
    if (index >= len) {
        len = index + 1;
    }
    row[len] = '\0';
    screen->row_lens[screen->cursor_row] = len;
    screen->cursor_col++;
}

static int parse_int_param(const char *text, int fallback)
{
    if (text == NULL || *text == '\0' || *text == '?') {
        return fallback;
    }
    return atoi(text);
}

static void handle_csi(bot_screen_t *screen, const char *seq)
{
    char final;
    char params[128];
    char *semi;
    int row;
    int col;
    size_t len = strlen(seq);

    if (len == 0) {
        return;
    }
    final = seq[len - 1];
    if (len - 1 >= sizeof(params)) {
        return;
    }
    memcpy(params, seq, len - 1);
    params[len - 1] = '\0';

    if (final == 'H' || final == 'f') {
        semi = strchr(params, ';');
        if (semi != NULL) {
            *semi = '\0';
            row = parse_int_param(params, 1);
            col = parse_int_param(semi + 1, 1);
        } else {
            row = parse_int_param(params, 1);
            col = 1;
        }
        if (row < 1) row = 1;
        if (row > BOT_SCREEN_ROWS) row = BOT_SCREEN_ROWS;
        if (col < 1) col = 1;
        screen->cursor_row = row;
        screen->cursor_col = col;
        if (col == 1) {
            row_clear(screen, row);
        }
    } else if (final == 'J') {
        if (params[0] == '2' || params[0] == '\0') {
            screen_clear(screen);
        }
    } else if (final == 'K') {
        row_clear(screen, screen->cursor_row);
    }
}

static int is_csi_final(unsigned char byte)
{
    return byte >= 0x40 && byte <= 0x7e;
}

static void handle_escape(bot_screen_t *screen, unsigned char byte)
{
    if (screen->esc_len + 1 >= sizeof(screen->esc)) {
        screen->esc_active = 0;
        screen->esc_len = 0;
        return;
    }
    screen->esc[screen->esc_len++] = (char)byte;
    screen->esc[screen->esc_len] = '\0';
    if (screen->esc_len == 1) {
        if (byte != '[') {
            screen->esc_active = 0;
            screen->esc_len = 0;
        }
        return;
    }
    if (is_csi_final(byte)) {
        handle_csi(screen, screen->esc + 1);
        screen->esc_active = 0;
        screen->esc_len = 0;
    }
}

void bot_screen_feed(bot_screen_t *screen, const char *data, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        unsigned char byte = (unsigned char)data[i];

        if (screen->esc_active) {
            handle_escape(screen, byte);
            continue;
        }
        if (byte == 0x1b) {
            screen->esc_active = 1;
            screen->esc_len = 0;
            continue;
        }
        if (byte == '\r') {
            screen->cursor_col = 1;
            continue;
        }
        if (byte == '\n') {
            if (screen->cursor_row < BOT_SCREEN_ROWS) {
                screen->cursor_row++;
            }
            screen->cursor_col = 1;
            continue;
        }
        if (byte < 0x20 || byte == 0x7f) {
            continue;
        }
        row_append(screen, byte);
    }
}

static void clean_box_line(const char *row, char *out, size_t size)
{
    size_t len;

    bot_copy_string(out, size, row);
    bot_trim(out);
    if (out[0] == '|') {
        char *cursor;

        memmove(out, out + 1, strlen(out));
        cursor = out;
        while ((cursor = strstr(cursor, " |")) != NULL) {
            char *after = cursor + 2;

            while (*after == ' ') {
                after++;
            }
            if (*after == '\0' || *after == '|' || *after == '+') {
                *cursor = '\0';
                break;
            }
            cursor += 2;
        }
    }
    bot_trim(out);
    len = strlen(out);
    if (len > 0 && out[len - 1] == '|') {
        out[len - 1] = '\0';
    }
    bot_trim(out);
}

int bot_screen_chat_ready(const bot_screen_t *screen)
{
    char row2[BOT_SCREEN_COLS * 8];
    char row_input[BOT_SCREEN_COLS * 8];

    clean_box_line(screen->rows[2], row2, sizeof(row2));
    clean_box_line(screen->rows[BOT_SCREEN_ROWS - 2], row_input, sizeof(row_input));
    return strstr(row2, "大厅") != NULL || strstr(row_input, "输入消息") != NULL;
}

int bot_screen_private_chat_active(const bot_screen_t *screen)
{
    char row2[BOT_SCREEN_COLS * 8];

    clean_box_line(screen->rows[2], row2, sizeof(row2));
    return strstr(row2, "大厅") != NULL &&
           strstr(row2, "[大厅]") == NULL &&
           strstr(row2, "[*大厅]") == NULL;
}

static int parse_chat_line(const char *line, int private_chat, bot_chat_message_t *message)
{
    char work[BOT_MAX_MESSAGE_TEXT];
    char fp_source[BOT_MAX_MESSAGE_TEXT + 16];
    char *right_bracket;
    char *rest;
    char *sep;

    bot_copy_string(work, sizeof(work), line);
    bot_trim(work);
    if (work[0] == '\0' || work[0] != '[') {
        return 0;
    }
    snprintf(fp_source, sizeof(fp_source), "%s\t%s", private_chat ? "private" : "lobby", work);
    right_bracket = strstr(work, "] ");
    if (right_bracket == NULL) {
        return 0;
    }
    rest = right_bracket + 2;
    bot_trim(rest);
    if (strncmp(rest, "SYSTEM:", 7) == 0 || strncmp(rest, "* ", 2) == 0) {
        return 0;
    }
    sep = strstr(rest, ": ");
    if (sep == NULL) {
        return 0;
    }
    *sep = '\0';
    bot_trim(rest);
    bot_trim(sep + 2);
    if (rest[0] == '\0' || sep[2] == '\0') {
        return 0;
    }
    bot_copy_string(message->sender, sizeof(message->sender), rest);
    bot_copy_string(message->text, sizeof(message->text), sep + 2);
    bot_sanitize_line(message->sender);
    bot_sanitize_line(message->text);
    bot_hash_hex(fp_source, message->fingerprint);
    return 1;
}

size_t bot_screen_extract_messages(const bot_screen_t *screen,
                                   bot_chat_message_t *messages,
                                   size_t max_messages)
{
    size_t count = 0;
    int row;
    int chat_last = BOT_SCREEN_ROWS - 3;
    int private_chat = bot_screen_private_chat_active(screen);

    for (row = 3; row <= chat_last && count < max_messages; row++) {
        char line[BOT_SCREEN_COLS * 8];

        clean_box_line(screen->rows[row], line, sizeof(line));
        if (parse_chat_line(line, private_chat, &messages[count])) {
            messages[count].private_chat = private_chat;
            count++;
        }
    }
    return count;
}
