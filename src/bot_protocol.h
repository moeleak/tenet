#ifndef TENET_BOT_PROTOCOL_H
#define TENET_BOT_PROTOCOL_H

#include "bot_config.h"

#include <stddef.h>

#define BOT_MAX_MESSAGE_TEXT 4096
#define BOT_MAX_SENDER 256

typedef enum bot_event_type {
    BOT_EVENT_MESSAGE = 1,
    BOT_EVENT_PRIVATE = 2
} bot_event_type_t;

typedef struct bot_event {
    bot_event_type_t type;
    char sender[BOT_MAX_SENDER];
    char sender_display[BOT_MAX_SENDER];
    char text[BOT_MAX_MESSAGE_TEXT];
    int private_chat;
} bot_event_t;

int bot_protocol_connect(const char *path, char *error, size_t error_size);
int bot_protocol_send_hello(int fd, const bot_config_t *config);
int bot_protocol_read_event(int fd, bot_event_t *event, char *error, size_t error_size);
int bot_protocol_send_chat(int fd, const char *message);
int bot_protocol_send_private(int fd, const char *target_username, const char *message);

#endif
