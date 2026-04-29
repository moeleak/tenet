#include "tenet.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#define LDAP_MAX_PACKET 8192
#define LDAP_SUCCESS 0
#define LDAP_OPERATIONS_ERROR 1
#define LDAP_PROTOCOL_ERROR 2
#define LDAP_INVALID_CREDENTIALS 49

#define LDAP_TAG_SEQUENCE 0x30
#define LDAP_TAG_INTEGER 0x02
#define LDAP_TAG_ENUM 0x0a
#define LDAP_TAG_BOOLEAN 0x01
#define LDAP_TAG_OCTET_STRING 0x04
#define LDAP_TAG_BIND_REQUEST 0x60
#define LDAP_TAG_BIND_RESPONSE 0x61
#define LDAP_TAG_UNBIND_REQUEST 0x42
#define LDAP_TAG_SEARCH_REQUEST 0x63
#define LDAP_TAG_SEARCH_ENTRY 0x64
#define LDAP_TAG_SEARCH_DONE 0x65
#define LDAP_TAG_SIMPLE_AUTH 0x80
#define LDAP_TAG_SET 0x31
#define LDAP_FILTER_OR 0xa1
#define LDAP_FILTER_EQUALITY 0xa3

typedef struct ber_buf {
    unsigned char data[LDAP_MAX_PACKET];
    size_t len;
} ber_buf_t;

typedef struct ber_reader {
    const unsigned char *data;
    size_t len;
    size_t pos;
} ber_reader_t;

typedef struct ldap_result {
    int message_id;
    int protocol_tag;
    int result_code;
    char diagnostic[256];
    char object_name[TENET_MAX_DN];
    char display_name[TENET_MAX_DISPLAY_NAME];
    int display_name_rank;
} ldap_result_t;

static void set_error(char *error, size_t error_size, const char *message)
{
    if (error_size == 0) {
        return;
    }
    snprintf(error, error_size, "%s", message != NULL ? message : "未知错误");
}


static const char *ldap_code_text(int code)
{
    switch (code) {
    case LDAP_SUCCESS:
        return "成功";
    case LDAP_OPERATIONS_ERROR:
        return "LDAP 操作错误";
    case LDAP_PROTOCOL_ERROR:
        return "LDAP 协议错误";
    case 32:
        return "对象不存在";
    case 34:
        return "DN 格式错误";
    case LDAP_INVALID_CREDENTIALS:
        return "用户名或密码错误";
    case 50:
        return "LDAP 权限不足";
    case 52:
        return "LDAP 不可用";
    default:
        return "LDAP 返回错误";
    }
}

static int append_bytes(ber_buf_t *buf, const void *data, size_t len)
{
    if (len > sizeof(buf->data) - buf->len) {
        return -1;
    }
    memcpy(buf->data + buf->len, data, len);
    buf->len += len;
    return 0;
}

static int append_byte(ber_buf_t *buf, unsigned char byte)
{
    return append_bytes(buf, &byte, 1);
}

static int append_length(ber_buf_t *buf, size_t len)
{
    unsigned char bytes[sizeof(size_t)];
    size_t count = 0;
    size_t value = len;
    size_t i;

    if (len < 128) {
        return append_byte(buf, (unsigned char)len);
    }

    while (value > 0 && count < sizeof(bytes)) {
        bytes[count++] = (unsigned char)(value & 0xffu);
        value >>= 8;
    }
    if (count > 126) {
        return -1;
    }
    if (append_byte(buf, (unsigned char)(0x80u | count)) != 0) {
        return -1;
    }
    for (i = 0; i < count; i++) {
        if (append_byte(buf, bytes[count - i - 1]) != 0) {
            return -1;
        }
    }
    return 0;
}

static int append_tlv(ber_buf_t *buf, unsigned char tag, const void *data, size_t len)
{
    if (append_byte(buf, tag) != 0 || append_length(buf, len) != 0) {
        return -1;
    }
    return append_bytes(buf, data, len);
}

static int append_container(ber_buf_t *buf, unsigned char tag, const ber_buf_t *inner)
{
    return append_tlv(buf, tag, inner->data, inner->len);
}

static int append_integer(ber_buf_t *buf, unsigned char tag, int value)
{
    unsigned char bytes[5];
    unsigned int v = (unsigned int)value;
    size_t len = 0;
    int i;

    if (value < 0) {
        return -1;
    }
    for (i = 3; i >= 0; i--) {
        unsigned char byte = (unsigned char)((v >> (i * 8)) & 0xffu);
        if (byte != 0 || len != 0) {
            bytes[len++] = byte;
        }
    }
    if (len == 0) {
        bytes[len++] = 0;
    }
    if ((bytes[0] & 0x80u) != 0) {
        memmove(bytes + 1, bytes, len);
        bytes[0] = 0;
        len++;
    }
    return append_tlv(buf, tag, bytes, len);
}

static int append_boolean(ber_buf_t *buf, int value)
{
    unsigned char byte = value ? 0xffu : 0x00u;
    return append_tlv(buf, LDAP_TAG_BOOLEAN, &byte, 1);
}

static int append_string(ber_buf_t *buf, unsigned char tag, const char *value)
{
    size_t len = value != NULL ? strlen(value) : 0;
    return append_tlv(buf, tag, value != NULL ? value : "", len);
}

static int read_exact(int fd, unsigned char *buf, size_t len)
{
    size_t read_len = 0;

    while (read_len < len) {
        ssize_t rc = recv(fd, buf + read_len, len - read_len, 0);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (rc == 0) {
            return 0;
        }
        read_len += (size_t)rc;
    }
    return 1;
}

static int decode_length_from_fd(int fd, size_t *len)
{
    unsigned char first;
    int rc = read_exact(fd, &first, 1);
    size_t count;
    size_t value = 0;

    if (rc <= 0) {
        return rc;
    }
    if ((first & 0x80u) == 0) {
        *len = first;
        return 1;
    }
    count = (size_t)(first & 0x7fu);
    if (count == 0 || count > sizeof(size_t)) {
        return -1;
    }
    while (count-- > 0) {
        unsigned char byte;
        rc = read_exact(fd, &byte, 1);
        if (rc <= 0) {
            return rc;
        }
        value = (value << 8) | byte;
    }
    *len = value;
    return 1;
}

static int read_ldap_packet(int fd, unsigned char *buffer, size_t size, size_t *packet_len)
{
    unsigned char tag;
    size_t content_len;
    int rc;

    rc = read_exact(fd, &tag, 1);
    if (rc <= 0) {
        return rc;
    }
    if (tag != LDAP_TAG_SEQUENCE) {
        return -1;
    }
    rc = decode_length_from_fd(fd, &content_len);
    if (rc <= 0) {
        return rc;
    }
    if (content_len > size) {
        return -1;
    }
    rc = read_exact(fd, buffer, content_len);
    if (rc <= 0) {
        return rc;
    }
    *packet_len = content_len;
    return 1;
}

static int reader_read_length(ber_reader_t *reader, size_t *len)
{
    unsigned char first;
    size_t count;
    size_t value = 0;

    if (reader->pos >= reader->len) {
        return -1;
    }
    first = reader->data[reader->pos++];
    if ((first & 0x80u) == 0) {
        *len = first;
        return 0;
    }
    count = (size_t)(first & 0x7fu);
    if (count == 0 || count > sizeof(size_t) || reader->len - reader->pos < count) {
        return -1;
    }
    while (count-- > 0) {
        value = (value << 8) | reader->data[reader->pos++];
    }
    *len = value;
    return 0;
}

static int reader_tlv(ber_reader_t *reader, int *tag, const unsigned char **value, size_t *len)
{
    if (reader->pos >= reader->len) {
        return -1;
    }
    *tag = reader->data[reader->pos++];
    if (reader_read_length(reader, len) != 0 || reader->len - reader->pos < *len) {
        return -1;
    }
    *value = reader->data + reader->pos;
    reader->pos += *len;
    return 0;
}

static int decode_int_value(const unsigned char *value, size_t len)
{
    int result = 0;
    size_t i;

    if (len == 0 || len > sizeof(int)) {
        return -1;
    }
    for (i = 0; i < len; i++) {
        result = (result << 8) | value[i];
    }
    return result;
}

static void copy_ber_string(char *dest, size_t dest_size, const unsigned char *value, size_t len)
{
    size_t copy_len;

    if (dest_size == 0) {
        return;
    }
    copy_len = len < dest_size - 1 ? len : dest_size - 1;
    memcpy(dest, value, copy_len);
    dest[copy_len] = '\0';
}

static int ascii_equal_ignore_case(const char *left, const char *right)
{
    while (*left != '\0' && *right != '\0') {
        if (tolower((unsigned char)*left) != tolower((unsigned char)*right)) {
            return 0;
        }
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

static int display_attribute_rank(const char *name)
{
    if (ascii_equal_ignore_case(name, "displayName")) {
        return 3;
    }
    if (ascii_equal_ignore_case(name, "cn")) {
        return 2;
    }
    if (ascii_equal_ignore_case(name, "name")) {
        return 1;
    }
    return 0;
}

static void parse_search_entry_attributes(ber_reader_t *entry, ldap_result_t *result)
{
    const unsigned char *value;
    size_t len;
    int tag;
    ber_reader_t attributes;

    if (reader_tlv(entry, &tag, &value, &len) != 0 || tag != LDAP_TAG_SEQUENCE) {
        return;
    }

    attributes.data = value;
    attributes.len = len;
    attributes.pos = 0;
    while (attributes.pos < attributes.len) {
        ber_reader_t attribute;
        ber_reader_t values;
        char name[128];
        int rank;

        if (reader_tlv(&attributes, &tag, &value, &len) != 0 || tag != LDAP_TAG_SEQUENCE) {
            return;
        }
        attribute.data = value;
        attribute.len = len;
        attribute.pos = 0;

        if (reader_tlv(&attribute, &tag, &value, &len) != 0 || tag != LDAP_TAG_OCTET_STRING) {
            continue;
        }
        copy_ber_string(name, sizeof(name), value, len);
        rank = display_attribute_rank(name);
        if (rank == 0 || rank <= result->display_name_rank) {
            continue;
        }

        if (reader_tlv(&attribute, &tag, &value, &len) != 0 || tag != LDAP_TAG_SET) {
            continue;
        }
        values.data = value;
        values.len = len;
        values.pos = 0;
        if (reader_tlv(&values, &tag, &value, &len) == 0 && tag == LDAP_TAG_OCTET_STRING) {
            copy_ber_string(result->display_name, sizeof(result->display_name), value, len);
            result->display_name_rank = rank;
        }
    }
}

static int parse_ldap_message(const unsigned char *packet, size_t packet_len, ldap_result_t *result)
{
    ber_reader_t reader;
    const unsigned char *value;
    size_t len;
    int tag;

    memset(result, 0, sizeof(*result));
    result->result_code = -1;
    reader.data = packet;
    reader.len = packet_len;
    reader.pos = 0;

    if (reader_tlv(&reader, &tag, &value, &len) != 0 || tag != LDAP_TAG_INTEGER) {
        return -1;
    }
    result->message_id = decode_int_value(value, len);

    if (reader_tlv(&reader, &tag, &value, &len) != 0) {
        return -1;
    }
    result->protocol_tag = tag;

    if (tag == LDAP_TAG_BIND_RESPONSE || tag == LDAP_TAG_SEARCH_DONE) {
        ber_reader_t inner;
        inner.data = value;
        inner.len = len;
        inner.pos = 0;
        if (reader_tlv(&inner, &tag, &value, &len) != 0 || tag != LDAP_TAG_ENUM) {
            return -1;
        }
        result->result_code = decode_int_value(value, len);
        if (reader_tlv(&inner, &tag, &value, &len) == 0 && tag == LDAP_TAG_OCTET_STRING) {
            ;
        }
        if (reader_tlv(&inner, &tag, &value, &len) == 0 && tag == LDAP_TAG_OCTET_STRING) {
            copy_ber_string(result->diagnostic, sizeof(result->diagnostic), value, len);
        }
    } else if (tag == LDAP_TAG_SEARCH_ENTRY) {
        ber_reader_t inner;
        inner.data = value;
        inner.len = len;
        inner.pos = 0;
        if (reader_tlv(&inner, &tag, &value, &len) != 0 || tag != LDAP_TAG_OCTET_STRING) {
            return -1;
        }
        copy_ber_string(result->object_name, sizeof(result->object_name), value, len);
        parse_search_entry_attributes(&inner, result);
        result->result_code = LDAP_SUCCESS;
    }

    return 0;
}

static int send_all(int fd, const unsigned char *data, size_t len)
{
    size_t sent = 0;

    while (sent < len) {
        ssize_t rc = send(fd, data + sent, len - sent, MSG_NOSIGNAL);
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

static int send_packet(int fd, const ber_buf_t *protocol_op, int message_id)
{
    ber_buf_t message = {{0}, 0};
    ber_buf_t outer = {{0}, 0};

    if (append_integer(&message, LDAP_TAG_INTEGER, message_id) != 0 ||
        append_bytes(&message, protocol_op->data, protocol_op->len) != 0 ||
        append_container(&outer, LDAP_TAG_SEQUENCE, &message) != 0) {
        return -1;
    }
    return send_all(fd, outer.data, outer.len);
}

static int build_bind_request(ber_buf_t *request, const char *bind_name, const char *password)
{
    ber_buf_t inner = {{0}, 0};

    request->len = 0;
    if (append_integer(&inner, LDAP_TAG_INTEGER, 3) != 0 ||
        append_string(&inner, LDAP_TAG_OCTET_STRING, bind_name) != 0 ||
        append_string(&inner, LDAP_TAG_SIMPLE_AUTH, password) != 0 ||
        append_container(request, LDAP_TAG_BIND_REQUEST, &inner) != 0) {
        return -1;
    }
    return 0;
}

static int send_unbind(int fd, int message_id)
{
    ber_buf_t op = {{0}, 0};
    if (append_tlv(&op, LDAP_TAG_UNBIND_REQUEST, "", 0) != 0) {
        return -1;
    }
    return send_packet(fd, &op, message_id);
}

static int ldap_simple_bind(int fd,
                            int message_id,
                            const char *bind_name,
                            const char *password,
                            int *ldap_code,
                            char *diagnostic,
                            size_t diagnostic_size)
{
    ber_buf_t request = {{0}, 0};
    unsigned char packet[LDAP_MAX_PACKET];
    size_t packet_len;
    ldap_result_t result;

    if (build_bind_request(&request, bind_name, password) != 0 ||
        send_packet(fd, &request, message_id) != 0) {
        return -1;
    }

    for (;;) {
        int rc = read_ldap_packet(fd, packet, sizeof(packet), &packet_len);
        if (rc <= 0) {
            return -1;
        }
        if (parse_ldap_message(packet, packet_len, &result) != 0) {
            return -1;
        }
        if (result.message_id != message_id) {
            continue;
        }
        if (result.protocol_tag != LDAP_TAG_BIND_RESPONSE) {
            return -1;
        }
        *ldap_code = result.result_code;
        if (diagnostic_size > 0) {
            snprintf(diagnostic, diagnostic_size, "%s", result.diagnostic);
        }
        return result.result_code == LDAP_SUCCESS ? 0 : 1;
    }
}

static int connect_timeout(const char *host, int port, int timeout_sec, char *error, size_t error_size)
{
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct addrinfo *cursor;
    char portbuf[16];
    int fd = -1;
    int gai;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(portbuf, sizeof(portbuf), "%d", port);

    gai = getaddrinfo(host, portbuf, &hints, &result);
    if (gai != 0) {
        char buffer[256];
        snprintf(buffer, sizeof(buffer), "LDAP 地址解析失败: %s", gai_strerror(gai));
        set_error(error, error_size, buffer);
        return -1;
    }

    for (cursor = result; cursor != NULL; cursor = cursor->ai_next) {
        int flags;
        int so_error = 0;
        socklen_t so_error_len = sizeof(so_error);
        fd_set write_set;
        struct timeval timeout;

        fd = socket(cursor->ai_family, cursor->ai_socktype, cursor->ai_protocol);
        if (fd < 0) {
            continue;
        }
        flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) {
            (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }

        if (connect(fd, cursor->ai_addr, cursor->ai_addrlen) == 0) {
            if (flags >= 0) {
                (void)fcntl(fd, F_SETFL, flags);
            }
            break;
        }
        if (errno != EINPROGRESS) {
            close(fd);
            fd = -1;
            continue;
        }

        FD_ZERO(&write_set);
        FD_SET(fd, &write_set);
        timeout.tv_sec = timeout_sec;
        timeout.tv_usec = 0;
        if (select(fd + 1, NULL, &write_set, NULL, &timeout) > 0 &&
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_error_len) == 0 &&
            so_error == 0) {
            if (flags >= 0) {
                (void)fcntl(fd, F_SETFL, flags);
            }
            break;
        }
        close(fd);
        fd = -1;
    }

    freeaddrinfo(result);
    if (fd < 0) {
        set_error(error, error_size, "无法连接 LDAP 服务器");
        return -1;
    }

    {
        struct timeval timeout;
        timeout.tv_sec = timeout_sec;
        timeout.tv_usec = 0;
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    }

    return fd;
}

static void derive_domain_from_base_dn(const char *base_dn, char *domain, size_t domain_size)
{
    const char *cursor = base_dn;
    size_t len = 0;

    if (domain_size == 0) {
        return;
    }
    domain[0] = '\0';
    while (*cursor != '\0') {
        while (*cursor == ' ' || *cursor == ',') {
            cursor++;
        }
        if ((cursor[0] == 'D' || cursor[0] == 'd') &&
            (cursor[1] == 'C' || cursor[1] == 'c') && cursor[2] == '=') {
            const char *value = cursor + 3;
            size_t part_len = 0;
            while (value[part_len] != '\0' && value[part_len] != ',') {
                part_len++;
            }
            if (part_len > 0) {
                if (len > 0 && len + 1 < domain_size) {
                    domain[len++] = '.';
                }
                while (part_len > 0 && value[part_len - 1] == ' ') {
                    part_len--;
                }
                if (len + part_len >= domain_size) {
                    part_len = domain_size - len - 1;
                }
                memcpy(domain + len, value, part_len);
                len += part_len;
                domain[len] = '\0';
            }
            cursor = value + part_len;
        }
        while (*cursor != '\0' && *cursor != ',') {
            cursor++;
        }
    }
}

static int build_direct_bind_name(const tenet_config_t *config,
                                  const char *username,
                                  char *bind_name,
                                  size_t bind_name_size)
{
    char domain[256];

    if (strchr(username, '@') != NULL || strchr(username, '=') != NULL || strchr(username, '\\') != NULL) {
        snprintf(bind_name, bind_name_size, "%s", username);
        return 0;
    }

    derive_domain_from_base_dn(config->ldap_base_dn, domain, sizeof(domain));
    if (domain[0] != '\0') {
        snprintf(bind_name, bind_name_size, "%s@%s", username, domain);
    } else {
        snprintf(bind_name, bind_name_size, "uid=%.128s,%.360s", username, config->ldap_base_dn);
    }
    return 0;
}

static int append_equality_filter(ber_buf_t *buf, const char *attribute, const char *value)
{
    ber_buf_t inner = {{0}, 0};

    if (append_string(&inner, LDAP_TAG_OCTET_STRING, attribute) != 0 ||
        append_string(&inner, LDAP_TAG_OCTET_STRING, value) != 0 ||
        append_container(buf, LDAP_FILTER_EQUALITY, &inner) != 0) {
        return -1;
    }
    return 0;
}

static int build_search_filter(ber_buf_t *filter, const char *username)
{
    ber_buf_t alternatives = {{0}, 0};

    filter->len = 0;
    if (append_equality_filter(&alternatives, "sAMAccountName", username) != 0 ||
        append_equality_filter(&alternatives, "uid", username) != 0 ||
        append_equality_filter(&alternatives, "cn", username) != 0 ||
        append_equality_filter(&alternatives, "userPrincipalName", username) != 0 ||
        append_equality_filter(&alternatives, "mail", username) != 0 ||
        append_container(filter, LDAP_FILTER_OR, &alternatives) != 0) {
        return -1;
    }
    return 0;
}

static int build_search_request(ber_buf_t *request,
                                const tenet_config_t *config,
                                const char *username)
{
    ber_buf_t inner = {{0}, 0};
    ber_buf_t filter = {{0}, 0};
    ber_buf_t attributes = {{0}, 0};

    request->len = 0;
    if (append_string(&attributes, LDAP_TAG_OCTET_STRING, "displayName") != 0 ||
        append_string(&attributes, LDAP_TAG_OCTET_STRING, "cn") != 0 ||
        append_string(&attributes, LDAP_TAG_OCTET_STRING, "name") != 0 ||
        build_search_filter(&filter, username) != 0 ||
        append_string(&inner, LDAP_TAG_OCTET_STRING, config->ldap_base_dn) != 0 ||
        append_integer(&inner, LDAP_TAG_ENUM, 2) != 0 ||
        append_integer(&inner, LDAP_TAG_ENUM, 0) != 0 ||
        append_integer(&inner, LDAP_TAG_INTEGER, 1) != 0 ||
        append_integer(&inner, LDAP_TAG_INTEGER, config->ldap_timeout_sec) != 0 ||
        append_boolean(&inner, 0) != 0 ||
        append_bytes(&inner, filter.data, filter.len) != 0 ||
        append_container(&inner, LDAP_TAG_SEQUENCE, &attributes) != 0 ||
        append_container(request, LDAP_TAG_SEARCH_REQUEST, &inner) != 0) {
        return -1;
    }
    return 0;
}

static int ldap_search_user_dn(int fd,
                               int message_id,
                               const tenet_config_t *config,
                               const char *username,
                               char *user_dn,
                               size_t user_dn_size,
                               char *display_name,
                               size_t display_name_size,
                               char *error,
                               size_t error_size)
{
    ber_buf_t request = {{0}, 0};
    unsigned char packet[LDAP_MAX_PACKET];
    size_t packet_len;
    int done_code = -1;

    if (build_search_request(&request, config, username) != 0 ||
        send_packet(fd, &request, message_id) != 0) {
        set_error(error, error_size, "发送 LDAP 搜索请求失败");
        return -1;
    }

    user_dn[0] = '\0';
    if (display_name_size > 0) {
        display_name[0] = '\0';
    }
    for (;;) {
        ldap_result_t result;
        int rc = read_ldap_packet(fd, packet, sizeof(packet), &packet_len);
        if (rc <= 0) {
            set_error(error, error_size, "读取 LDAP 搜索响应失败");
            return -1;
        }
        if (parse_ldap_message(packet, packet_len, &result) != 0 || result.message_id != message_id) {
            continue;
        }
        if (result.protocol_tag == LDAP_TAG_SEARCH_ENTRY && user_dn[0] == '\0') {
            snprintf(user_dn, user_dn_size, "%s", result.object_name);
            if (display_name_size > 0 && result.display_name[0] != '\0') {
                snprintf(display_name, display_name_size, "%s", result.display_name);
            }
        } else if (result.protocol_tag == LDAP_TAG_SEARCH_DONE) {
            done_code = result.result_code;
            if (done_code != LDAP_SUCCESS) {
                char buffer[512];
                snprintf(buffer, sizeof(buffer), "%s%s%s",
                         ldap_code_text(done_code),
                         result.diagnostic[0] != '\0' ? ": " : "",
                         result.diagnostic);
                set_error(error, error_size, buffer);
                return -1;
            }
            break;
        }
    }

    if (user_dn[0] == '\0') {
        set_error(error, error_size, "没有在 BaseDN 中找到该用户");
        return 1;
    }
    return 0;
}

static int authenticate_direct(const tenet_config_t *config,
                               const char *username,
                               const char *password,
                               char *display_name,
                               size_t display_name_size,
                               char *error,
                               size_t error_size)
{
    int fd;
    int code = -1;
    char diagnostic[256];
    char bind_name[TENET_MAX_DN];
    int rc;

    build_direct_bind_name(config, username, bind_name, sizeof(bind_name));
    if (display_name_size > 0) {
        snprintf(display_name, display_name_size, "%s", username);
    }
    fd = connect_timeout(config->ldap_host, config->ldap_port, config->ldap_timeout_sec,
                         error, error_size);
    if (fd < 0) {
        return -1;
    }

    diagnostic[0] = '\0';
    rc = ldap_simple_bind(fd, 1, bind_name, password, &code,
                          diagnostic, sizeof(diagnostic));

    if (rc == 0) {
        char found_dn[TENET_MAX_DN];

        if (display_name_size > 0) {
            display_name[0] = '\0';
        }
        (void)ldap_search_user_dn(fd, 2, config, username,
                                  found_dn, sizeof(found_dn),
                                  display_name, display_name_size,
                                  error, error_size);
        if (display_name_size > 0 && display_name[0] == '\0') {
            snprintf(display_name, display_name_size, "%s", username);
        }
        (void)send_unbind(fd, 3);
        close(fd);
        return 0;
    }
    (void)send_unbind(fd, 2);
    close(fd);
    if (rc > 0) {
        char buffer[1024];
        snprintf(buffer, sizeof(buffer), "%s%s%s (bind=%s)",
                 ldap_code_text(code), diagnostic[0] != '\0' ? ": " : "",
                 diagnostic, bind_name);
        set_error(error, error_size, buffer);
        return -1;
    }

    set_error(error, error_size, "LDAP 绑定请求失败");
    return -1;
}

static int authenticate_search(const tenet_config_t *config,
                               const char *username,
                               const char *password,
                               char *display_name,
                               size_t display_name_size,
                               char *error,
                               size_t error_size)
{
    int fd;
    int code = -1;
    char diagnostic[256];
    char user_dn[TENET_MAX_DN];
    int rc;

    fd = connect_timeout(config->ldap_host, config->ldap_port, config->ldap_timeout_sec,
                         error, error_size);
    if (fd < 0) {
        return -1;
    }

    diagnostic[0] = '\0';
    if (config->ldap_bind_dn[0] != '\0' || config->ldap_bind_password[0] != '\0') {
        rc = ldap_simple_bind(fd, 1, config->ldap_bind_dn, config->ldap_bind_password,
                              &code, diagnostic, sizeof(diagnostic));
    } else {
        rc = ldap_simple_bind(fd, 1, "", "", &code, diagnostic, sizeof(diagnostic));
    }
    if (rc != 0) {
        char buffer[512];
        snprintf(buffer, sizeof(buffer), "搜索绑定失败: %s%s%s",
                 ldap_code_text(code), diagnostic[0] != '\0' ? ": " : "", diagnostic);
        set_error(error, error_size, buffer);
        close(fd);
        return -1;
    }

    if (ldap_search_user_dn(fd, 2, config, username, user_dn, sizeof(user_dn),
                            display_name, display_name_size,
                            error, error_size) != 0) {
        (void)send_unbind(fd, 4);
        close(fd);
        return -1;
    }

    diagnostic[0] = '\0';
    rc = ldap_simple_bind(fd, 3, user_dn, password, &code,
                          diagnostic, sizeof(diagnostic));
    (void)send_unbind(fd, 4);
    close(fd);

    if (rc == 0) {
        return 0;
    }
    if (rc > 0) {
        char buffer[512];
        snprintf(buffer, sizeof(buffer), "%s%s%s", ldap_code_text(code),
                 diagnostic[0] != '\0' ? ": " : "", diagnostic);
        set_error(error, error_size, buffer);
        return -1;
    }

    set_error(error, error_size, "LDAP 用户绑定请求失败");
    return -1;
}

int tenet_ldap_lookup_user(const tenet_config_t *config,
                           const char *username,
                           char *display_name,
                           size_t display_name_size,
                           char *error,
                           size_t error_size)
{
    int fd;
    int code = -1;
    char diagnostic[256];
    char user_dn[TENET_MAX_DN];
    int rc;

    if (display_name_size > 0) {
        display_name[0] = '\0';
    }
    if (!config->ldap_enabled) {
        set_error(error, error_size, "LDAP 已关闭");
        return -1;
    }

    fd = connect_timeout(config->ldap_host, config->ldap_port, config->ldap_timeout_sec,
                         error, error_size);
    if (fd < 0) {
        return -1;
    }

    diagnostic[0] = '\0';
    if (config->ldap_bind_dn[0] != '\0' || config->ldap_bind_password[0] != '\0') {
        rc = ldap_simple_bind(fd, 1, config->ldap_bind_dn, config->ldap_bind_password,
                              &code, diagnostic, sizeof(diagnostic));
    } else {
        rc = ldap_simple_bind(fd, 1, "", "", &code, diagnostic, sizeof(diagnostic));
    }
    if (rc != 0) {
        char buffer[512];
        snprintf(buffer, sizeof(buffer), "LDAP 查询绑定失败: %s%s%s",
                 ldap_code_text(code), diagnostic[0] != '\0' ? ": " : "", diagnostic);
        set_error(error, error_size, buffer);
        close(fd);
        return -1;
    }

    rc = ldap_search_user_dn(fd, 2, config, username, user_dn, sizeof(user_dn),
                             display_name, display_name_size,
                             error, error_size);
    (void)send_unbind(fd, 3);
    close(fd);
    return rc;
}

int tenet_ldap_authenticate(const tenet_config_t *config,
                            const char *username,
                            const char *password,
                            char *display_name,
                            size_t display_name_size,
                            char *error,
                            size_t error_size)
{
    if (display_name_size > 0) {
        display_name[0] = '\0';
    }
    if (!config->ldap_enabled) {
        if (display_name_size > 0) {
            snprintf(display_name, display_name_size, "%s", username);
        }
        return 0;
    }
    if (password == NULL || password[0] == '\0') {
        set_error(error, error_size, "密码不能为空");
        return -1;
    }
    if (config->ldap_search) {
        return authenticate_search(config, username, password,
                                   display_name, display_name_size,
                                   error, error_size);
    }
    return authenticate_direct(config, username, password,
                               display_name, display_name_size,
                               error, error_size);
}
