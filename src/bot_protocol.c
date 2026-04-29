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

    written = snprintf(buffer, sizeof(buffer), "TENET/BOT/1\n%s\n%s\n",
                       config->username,
                       config->display_name);
    if (written < 0 || (size_t)written >= sizeof(buffer)) {
        return -1;
    }
    return bot_write_all(fd, buffer, (size_t)written);
}

static int read_protocol_line(int fd, bot_str_t *line, char *error, size_t error_size)
{
    bot_str_init(line);
    for (;;) {
        unsigned char byte;
        ssize_t got = read(fd, &byte, 1);

        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            snprintf(error, error_size, "读取 tenet 后端失败: %s", strerror(errno));
            bot_str_free(line);
            return -1;
        }
        if (got == 0) {
            bot_str_free(line);
            return 0;
        }
        if (byte == '\r') {
            continue;
        }
        if (byte == '\n') {
            return 1;
        }
        if (bot_str_append_char(line, (char)byte) != 0) {
            snprintf(error, error_size, "读取 tenet 事件失败: 内存不足");
            bot_str_free(line);
            return -1;
        }
    }
}

static int unescape_field(const char *text, char *out, size_t size)
{
    size_t used = 0;

    if (size == 0) {
        return -1;
    }
    while (*text != '\0') {
        unsigned char ch = (unsigned char)*text++;

        if (ch == '\\') {
            ch = (unsigned char)*text++;
            switch (ch) {
            case '\\': ch = '\\'; break;
            case 't': ch = '\t'; break;
            case 'n': ch = '\n'; break;
            case 'r': ch = '\r'; break;
            default: return -1;
            }
        }
        if (used + 1 >= size) {
            return -1;
        }
        out[used++] = (char)ch;
    }
    out[used] = '\0';
    return 0;
}

static int parse_event_line(char *line, bot_event_t *event)
{
    char *sender;
    char *display;
    char *message;

    memset(event, 0, sizeof(*event));
    if (strncmp(line, "MSG\t", 4) == 0) {
        event->type = BOT_EVENT_MESSAGE;
        event->private_chat = 0;
        sender = line + 4;
    } else if (strncmp(line, "PM\t", 3) == 0) {
        event->type = BOT_EVENT_PRIVATE;
        event->private_chat = 1;
        sender = line + 3;
    } else {
        return 0;
    }

    display = strchr(sender, '\t');
    if (display == NULL) {
        return 0;
    }
    *display++ = '\0';
    message = strchr(display, '\t');
    if (message == NULL) {
        return 0;
    }
    *message++ = '\0';

    if (unescape_field(sender, event->sender, sizeof(event->sender)) != 0 ||
        unescape_field(display, event->sender_display, sizeof(event->sender_display)) != 0 ||
        unescape_field(message, event->text, sizeof(event->text)) != 0) {
        return 0;
    }
    bot_sanitize_line(event->sender);
    bot_sanitize_line(event->sender_display);
    return event->sender[0] != '\0' && event->text[0] != '\0';
}

int bot_protocol_read_event(int fd, bot_event_t *event, char *error, size_t error_size)
{
    for (;;) {
        bot_str_t line;
        int rc = read_protocol_line(fd, &line, error, error_size);

        if (rc <= 0) {
            return rc;
        }
        if (line.data != NULL && parse_event_line(line.data, event)) {
            bot_str_free(&line);
            return 1;
        }
        bot_str_free(&line);
    }
}

static int append_escaped(bot_str_t *line, const char *text)
{
    const unsigned char *cursor = (const unsigned char *)(text != NULL ? text : "");

    while (*cursor != '\0') {
        unsigned char ch = *cursor++;

        switch (ch) {
        case '\\': if (bot_str_append(line, "\\\\") != 0) return -1; break;
        case '\t': if (bot_str_append(line, "\\t") != 0) return -1; break;
        case '\n': if (bot_str_append(line, "\\n") != 0) return -1; break;
        case '\r': if (bot_str_append(line, "\\r") != 0) return -1; break;
        default:
            if ((ch < 0x20 && ch != '\t') || ch == 0x7f) {
                if (bot_str_append_char(line, ' ') != 0) return -1;
            } else if (bot_str_append_char(line, (char)ch) != 0) {
                return -1;
            }
            break;
        }
    }
    return 0;
}

int bot_protocol_send_chat(int fd, const char *message)
{
    bot_str_t line;
    int rc;

    bot_str_init(&line);
    rc = bot_str_append(&line, "CHAT\t") == 0 &&
         append_escaped(&line, message) == 0 &&
         bot_str_append_char(&line, '\n') == 0 ? 0 : -1;
    if (rc == 0) {
        rc = bot_write_all(fd, line.data, line.len);
    }
    bot_str_free(&line);
    return rc;
}

int bot_protocol_send_private(int fd, const char *target_username, const char *message)
{
    bot_str_t line;
    int rc;

    bot_str_init(&line);
    rc = bot_str_append(&line, "PM\t") == 0 &&
         bot_str_append(&line, target_username != NULL ? target_username : "") == 0 &&
         bot_str_append_char(&line, '\t') == 0 &&
         append_escaped(&line, message) == 0 &&
         bot_str_append_char(&line, '\n') == 0 ? 0 : -1;
    if (rc == 0) {
        rc = bot_write_all(fd, line.data, line.len);
    }
    bot_str_free(&line);
    return rc;
}
