#ifndef TENET_BOT_SCREEN_H
#define TENET_BOT_SCREEN_H

#include "bot_config.h"

#include <stddef.h>

#define BOT_MAX_CHAT_MESSAGES 256
#define BOT_MAX_MESSAGE_TEXT 4096
#define BOT_MAX_SENDER 256

typedef struct bot_chat_message {
    char sender[BOT_MAX_SENDER];
    char text[BOT_MAX_MESSAGE_TEXT];
    char fingerprint[17];
} bot_chat_message_t;

typedef struct bot_screen {
    char rows[BOT_SCREEN_ROWS + 1][BOT_SCREEN_COLS * 8];
    size_t row_lens[BOT_SCREEN_ROWS + 1];
    int cursor_row;
    int cursor_col;
    int esc_active;
    char esc[128];
    size_t esc_len;
} bot_screen_t;

void bot_screen_init(bot_screen_t *screen);
void bot_screen_feed(bot_screen_t *screen, const char *data, size_t len);
int bot_screen_chat_ready(const bot_screen_t *screen);
size_t bot_screen_extract_messages(const bot_screen_t *screen,
                                   bot_chat_message_t *messages,
                                   size_t max_messages);

#endif
