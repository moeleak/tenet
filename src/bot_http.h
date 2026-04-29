#ifndef TENET_BOT_HTTP_H
#define TENET_BOT_HTTP_H

#include "bot_util.h"

int bot_http_post_json(const char *host,
                       int port,
                       const char *path,
                       const char *body,
                       int timeout_sec,
                       int *status_out,
                       bot_str_t *response_body,
                       char *error,
                       size_t error_size);

#endif
