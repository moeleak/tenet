#include "bot_http.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

static void set_error(char *error, size_t error_size, const char *message)
{
    if (error_size > 0) {
        snprintf(error, error_size, "%s", message != NULL ? message : "未知错误");
    }
}

static int connect_tcp(const char *host, int port, int timeout_sec, char *error, size_t error_size)
{
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct addrinfo *cursor;
    char port_text[16];
    int fd = -1;
    int gai;

    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    snprintf(port_text, sizeof(port_text), "%d", port);
    gai = getaddrinfo(host, port_text, &hints, &result);
    if (gai != 0) {
        snprintf(error, error_size, "getaddrinfo: %s", gai_strerror(gai));
        return -1;
    }

    for (cursor = result; cursor != NULL; cursor = cursor->ai_next) {
        struct timeval timeout;

        fd = socket(cursor->ai_family, cursor->ai_socktype, cursor->ai_protocol);
        if (fd < 0) {
            continue;
        }
        timeout.tv_sec = timeout_sec;
        timeout.tv_usec = 0;
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        if (connect(fd, cursor->ai_addr, cursor->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(result);
    if (fd < 0) {
        set_error(error, error_size, "无法连接 Ollama HTTP 服务");
        return -1;
    }
    return fd;
}

static const char *find_header_end(const char *data, size_t len)
{
    size_t i;

    for (i = 0; i + 3 < len; i++) {
        if (data[i] == '\r' && data[i + 1] == '\n' && data[i + 2] == '\r' && data[i + 3] == '\n') {
            return data + i + 4;
        }
    }
    return NULL;
}

static int header_contains_chunked(const char *headers, size_t len)
{
    const char needle[] = "transfer-encoding:";
    size_t i;

    for (i = 0; i + sizeof(needle) - 1 < len; i++) {
        size_t j;
        for (j = 0; j < sizeof(needle) - 1; j++) {
            if (tolower((unsigned char)headers[i + j]) != needle[j]) {
                break;
            }
        }
        if (j == sizeof(needle) - 1) {
            const char *line = headers + i;
            const char *end = memchr(line, '\n', len - i);
            size_t line_len = end != NULL ? (size_t)(end - line) : len - i;
            size_t k;
            for (k = 0; k + 7 <= line_len; k++) {
                if (tolower((unsigned char)line[k]) == 'c' &&
                    tolower((unsigned char)line[k + 1]) == 'h' &&
                    tolower((unsigned char)line[k + 2]) == 'u' &&
                    tolower((unsigned char)line[k + 3]) == 'n' &&
                    tolower((unsigned char)line[k + 4]) == 'k' &&
                    tolower((unsigned char)line[k + 5]) == 'e' &&
                    tolower((unsigned char)line[k + 6]) == 'd') {
                    return 1;
                }
            }
        }
    }
    return 0;
}

static int decode_chunked(const char *data, size_t len, bot_str_t *out)
{
    size_t pos = 0;

    while (pos < len) {
        char *endptr;
        unsigned long chunk_len;
        size_t line_end = pos;

        while (line_end + 1 < len && !(data[line_end] == '\r' && data[line_end + 1] == '\n')) {
            line_end++;
        }
        if (line_end + 1 >= len) {
            return -1;
        }
        errno = 0;
        chunk_len = strtoul(data + pos, &endptr, 16);
        if (errno != 0 || endptr == data + pos) {
            return -1;
        }
        pos = line_end + 2;
        if (chunk_len == 0) {
            return 0;
        }
        if (chunk_len > len - pos) {
            return -1;
        }
        if (bot_str_append_len(out, data + pos, chunk_len) != 0) {
            return -1;
        }
        pos += chunk_len;
        if (pos + 1 < len && data[pos] == '\r' && data[pos + 1] == '\n') {
            pos += 2;
        }
    }
    return 0;
}

int bot_http_post_json(const char *host,
                       int port,
                       const char *path,
                       const char *body,
                       int timeout_sec,
                       int *status_out,
                       bot_str_t *response_body,
                       char *error,
                       size_t error_size)
{
    int fd;
    bot_str_t request;
    bot_str_t response;
    char buffer[8192];
    ssize_t got;
    const char *body_start;
    size_t header_len;
    int status = 0;
    int rc = -1;

    bot_str_init(&request);
    bot_str_init(&response);
    fd = connect_tcp(host, port, timeout_sec, error, error_size);
    if (fd < 0) {
        goto out;
    }
    if (bot_str_appendf(&request,
                        "POST %s HTTP/1.1\r\n"
                        "Host: %s:%d\r\n"
                        "Content-Type: application/json\r\n"
                        "Accept: application/json\r\n"
                        "Connection: close\r\n"
                        "Content-Length: %zu\r\n\r\n",
                        path, host, port, strlen(body)) != 0 ||
        bot_str_append(&request, body) != 0) {
        set_error(error, error_size, "构造 HTTP 请求失败");
        goto out_fd;
    }
    if (bot_write_all(fd, request.data, request.len) != 0) {
        set_error(error, error_size, "发送 HTTP 请求失败");
        goto out_fd;
    }
    for (;;) {
        got = read(fd, buffer, sizeof(buffer));
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            set_error(error, error_size, "读取 HTTP 响应失败");
            goto out_fd;
        }
        if (got == 0) {
            break;
        }
        if (bot_str_append_len(&response, buffer, (size_t)got) != 0) {
            set_error(error, error_size, "HTTP 响应过大");
            goto out_fd;
        }
    }
    body_start = find_header_end(response.data != NULL ? response.data : "", response.len);
    if (body_start == NULL) {
        set_error(error, error_size, "HTTP 响应缺少 header");
        goto out_fd;
    }
    if (sscanf(response.data, "HTTP/%*s %d", &status) != 1) {
        set_error(error, error_size, "HTTP 状态行无效");
        goto out_fd;
    }
    header_len = (size_t)(body_start - response.data);
    *status_out = status;
    if (header_contains_chunked(response.data, header_len)) {
        if (decode_chunked(body_start, response.len - header_len, response_body) != 0) {
            set_error(error, error_size, "解析 chunked HTTP 响应失败");
            goto out_fd;
        }
    } else if (bot_str_append_len(response_body, body_start, response.len - header_len) != 0) {
        set_error(error, error_size, "保存 HTTP 响应失败");
        goto out_fd;
    }
    rc = 0;

out_fd:
    close(fd);
out:
    bot_str_free(&request);
    bot_str_free(&response);
    return rc;
}
