#ifndef TENET_BOT_PROTOCOL_H
#define TENET_BOT_PROTOCOL_H

#include "bot_config.h"

int bot_protocol_connect(const char *path, char *error, size_t error_size);
int bot_protocol_send_hello(int fd, const bot_config_t *config);
int bot_protocol_send_enter(int fd);
int bot_protocol_send_message(int fd, const char *message);

#endif
