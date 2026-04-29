#include "bot_protocol.h"
#include "bot_util.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int bot_protocol_connect(const char *path, char *error, size_t error_size)
{
    struct sockaddr_un addr;
    int fd;

    if (strlen(path) >= sizeof(addr.sun_path)) {
        snprintf(error, error_size, "socket 路径过长: %s", path);
        return -1;
    }
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(error, error_size, "socket: %s", strerror(errno));
        return -1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        snprintf(error, error_size, "connect %s: %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

int bot_protocol_send_hello(int fd, const bot_config_t *config)
{
    char buffer[512];
    int written;

    written = snprintf(buffer, sizeof(buffer), "TENET/3\n%s\n%s\n%d %d\n",
                       config->username,
                       config->display_name,
                       BOT_SCREEN_ROWS,
                       BOT_SCREEN_COLS);
    if (written < 0 || (size_t)written >= sizeof(buffer)) {
        return -1;
    }
    return bot_write_all(fd, buffer, (size_t)written);
}

int bot_protocol_send_enter(int fd)
{
    return bot_write_all(fd, "\n", 1);
}

int bot_protocol_send_message(int fd, const char *message)
{
    if (bot_write_all(fd, message, strlen(message)) != 0) {
        return -1;
    }
    return bot_write_all(fd, "\n", 1);
}
