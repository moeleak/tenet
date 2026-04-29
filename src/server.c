#include "server_internal.h"

volatile sig_atomic_t stop_requested;
server_state_t *global_state;

static void refresh_client_screen(server_state_t *state, client_t *client);
static void refresh_client_input(server_state_t *state, client_t *client);
static void compute_layout(const client_t *client, ui_layout_t *layout);
static const char *client_name(const client_t *client);
static client_t *find_client_by_username_locked(server_state_t *state, const char *username);
static int clamp_int(int value, int min_value, int max_value);
static int read_control_sequence(client_t *client);
static size_t utf8_prefix_for_width(const char *text, int max_width, int *used_width);
static int send_bytes_unlocked(client_t *client, const void *data, size_t len);
static void destroy_client(client_t *client);

static void handle_signal(int signum)
{
    (void)signum;
    stop_requested = 1;
    if (global_state != NULL && global_state->listen_fd >= 0) {
        close(global_state->listen_fd);
        global_state->listen_fd = -1;
    }
}

void safe_copy(char *dest, size_t size, const char *src)
{
    if (size == 0) {
        return;
    }
    snprintf(dest, size, "%s", src != NULL ? src : "");
}

void trim_line(char *text)
{
    size_t len;

    while (isspace((unsigned char)*text)) {
        memmove(text, text + 1, strlen(text));
    }
    len = strlen(text);
    while (len > 0 && isspace((unsigned char)text[len - 1])) {
        text[--len] = '\0';
    }
}

int ascii_equal_ignore_case(const char *left, const char *right)
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

static int ascii_starts_with_ignore_case(const char *text, const char *prefix)
{
    while (*prefix != '\0') {
        if (*text == '\0' ||
            tolower((unsigned char)*text) != tolower((unsigned char)*prefix)) {
            return 0;
        }
        text++;
        prefix++;
    }
    return 1;
}

static void now_string(char *buf, size_t size)
{
    time_t now = time(NULL);
    struct tm tm_now;

    localtime_r(&now, &tm_now);
    strftime(buf, size, "%H:%M", &tm_now);
}

static void format_header_title(char *buf, size_t size, int online_count)
{
    snprintf(buf, size, " TENET 比友聊天室 | 共有 %d 位比友在线 | /help 查看命令帮助 ",
             online_count);
}


static ssize_t send_all(int fd, const void *data, size_t len)
{
    const char *bytes = data;
    size_t sent = 0;

    while (sent < len) {
        ssize_t rc = send(fd, bytes + sent, len - sent, MSG_NOSIGNAL);
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
    return (ssize_t)sent;
}

static int send_text_unlocked(client_t *client, const char *text)
{
    return send_bytes_unlocked(client, text, strlen(text));
}

static int flush_write_buffer_unlocked(client_t *client)
{
    int rc = 0;

    if (client->write_buffer_len > 0) {
        rc = send_all(client->fd, client->write_buffer, client->write_buffer_len) < 0 ? -1 : 0;
        client->write_buffer_len = 0;
    }
    if (client->write_buffer_error) {
        client->write_buffer_error = 0;
        return -1;
    }
    return rc;
}

static void begin_buffered_write_unlocked(client_t *client)
{
    client->write_buffering = 1;
    client->write_buffer_error = 0;
    client->write_buffer_len = 0;
}

static int end_buffered_write_unlocked(client_t *client)
{
    int rc = flush_write_buffer_unlocked(client);

    client->write_buffering = 0;
    return rc;
}

static int send_bytes_unlocked(client_t *client, const void *data, size_t len)
{
    size_t needed;
    size_t new_cap;

    if (!client->write_buffering) {
        return send_all(client->fd, data, len) < 0 ? -1 : 0;
    }
    if (client->write_buffer_error) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }
    if (len > SIZE_MAX - client->write_buffer_len) {
        client->write_buffer_error = 1;
        return -1;
    }

    needed = client->write_buffer_len + len;
    if (needed > client->write_buffer_cap) {
        char *new_buffer;

        new_cap = client->write_buffer_cap > 0 ? client->write_buffer_cap : 8192;
        while (new_cap < needed) {
            if (new_cap > SIZE_MAX / 2) {
                new_cap = needed;
                break;
            }
            new_cap *= 2;
        }
        new_buffer = realloc(client->write_buffer, new_cap);
        if (new_buffer == NULL) {
            client->write_buffer_error = 1;
            return -1;
        }
        client->write_buffer = new_buffer;
        client->write_buffer_cap = new_cap;
    }
    memcpy(client->write_buffer + client->write_buffer_len, data, len);
    client->write_buffer_len += len;
    return 0;
}

int send_text(client_t *client, const char *text)
{
    int rc;

    pthread_mutex_lock(&client->write_mutex);
    rc = send_text_unlocked(client, text);
    pthread_mutex_unlock(&client->write_mutex);
    return rc;
}

static int send_fmt_unlocked(client_t *client, const char *fmt, ...)
{
    char buffer[4096];
    va_list args;
    int written;

    va_start(args, fmt);
    written = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    if (written < 0) {
        return -1;
    }
    if ((size_t)written >= sizeof(buffer)) {
        written = (int)sizeof(buffer) - 1;
    }
    return send_bytes_unlocked(client, buffer, (size_t)written);
}

int send_fmt(client_t *client, const char *fmt, ...)
{
    char buffer[2048];
    va_list args;
    int written;

    va_start(args, fmt);
    written = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    if (written < 0) {
        return -1;
    }
    if ((size_t)written >= sizeof(buffer)) {
        written = (int)sizeof(buffer) - 1;
    }
    pthread_mutex_lock(&client->write_mutex);
    written = send_all(client->fd, buffer, (size_t)written) < 0 ? -1 : 0;
    pthread_mutex_unlock(&client->write_mutex);
    return written;
}

static void send_spaces_unlocked(client_t *client, int count)
{
    static const char spaces[] = "                                                                                ";

    while (count > 0) {
        int chunk = count > (int)sizeof(spaces) - 1 ? (int)sizeof(spaces) - 1 : count;
        (void)send_bytes_unlocked(client, spaces, (size_t)chunk);
        count -= chunk;
    }
}

static void draw_header_bar_unlocked(client_t *client, int width, const char *title)
{
    int used_width = 0;
    size_t bytes;

    if (width <= 0) {
        return;
    }

    bytes = utf8_prefix_for_width(title != NULL ? title : "", width, &used_width);
    (void)send_text_unlocked(client, ANSI_BLUE_BG ANSI_WHITE ANSI_BOLD);
    if (bytes > 0) {
        (void)send_bytes_unlocked(client, title, bytes);
    }
    if (used_width < width) {
        send_spaces_unlocked(client, width - used_width);
    }
    (void)send_text_unlocked(client, ANSI_RESET);
}

static void send_telnet_setup(int fd)
{
    const unsigned char setup[] = {
        TELNET_IAC, TELNET_WILL, TELNET_SUPPRESS_GO_AHEAD,
        TELNET_IAC, TELNET_WILL, TELNET_ECHO,
        TELNET_IAC, TELNET_DONT, TELNET_LINEMODE
    };
    (void)send_all(fd, setup, sizeof(setup));
}

void request_server_echo(client_t *client)
{
    const unsigned char sequence[] = {TELNET_IAC, TELNET_WILL, TELNET_ECHO};
    (void)send_all(client->fd, sequence, sizeof(sequence));
}

static int recv_byte(client_t *client, unsigned char *out)
{
    for (;;) {
        ssize_t rc = recv(client->fd, out, 1, 0);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (rc == 0) {
            return 0;
        }
        return 1;
    }
}

static int utf8_decode_last(const char *text, size_t len, unsigned int *codepoint, size_t *start)
{
    size_t pos;
    const unsigned char *bytes = (const unsigned char *)text;

    if (len == 0) {
        return -1;
    }

    pos = len - 1;
    while (pos > 0 && (bytes[pos] & 0xc0u) == 0x80u) {
        pos--;
    }

    if ((bytes[pos] & 0x80u) == 0) {
        *codepoint = bytes[pos];
    } else if ((bytes[pos] & 0xe0u) == 0xc0u && len - pos >= 2) {
        *codepoint = ((unsigned int)(bytes[pos] & 0x1fu) << 6) |
                     (unsigned int)(bytes[pos + 1] & 0x3fu);
    } else if ((bytes[pos] & 0xf0u) == 0xe0u && len - pos >= 3) {
        *codepoint = ((unsigned int)(bytes[pos] & 0x0fu) << 12) |
                     ((unsigned int)(bytes[pos + 1] & 0x3fu) << 6) |
                     (unsigned int)(bytes[pos + 2] & 0x3fu);
    } else if ((bytes[pos] & 0xf8u) == 0xf0u && len - pos >= 4) {
        *codepoint = ((unsigned int)(bytes[pos] & 0x07u) << 18) |
                     ((unsigned int)(bytes[pos + 1] & 0x3fu) << 12) |
                     ((unsigned int)(bytes[pos + 2] & 0x3fu) << 6) |
                     (unsigned int)(bytes[pos + 3] & 0x3fu);
    } else {
        *codepoint = bytes[len - 1];
        pos = len - 1;
    }

    *start = pos;
    return 0;
}

static int unicode_is_wide(unsigned int codepoint)
{
    return (codepoint >= 0x1100 && codepoint <= 0x115f) ||
           codepoint == 0x2329 || codepoint == 0x232a ||
           (codepoint >= 0x2e80 && codepoint <= 0xa4cf) ||
           (codepoint >= 0xac00 && codepoint <= 0xd7a3) ||
           (codepoint >= 0xf900 && codepoint <= 0xfaff) ||
           (codepoint >= 0xfe10 && codepoint <= 0xfe19) ||
           (codepoint >= 0xfe30 && codepoint <= 0xfe6f) ||
           (codepoint >= 0xff00 && codepoint <= 0xff60) ||
           (codepoint >= 0xffe0 && codepoint <= 0xffe6) ||
           (codepoint >= 0x20000 && codepoint <= 0x3fffd);
}

static int unicode_is_combining(unsigned int codepoint)
{
    return (codepoint >= 0x0300 && codepoint <= 0x036f) ||
           (codepoint >= 0x1ab0 && codepoint <= 0x1aff) ||
           (codepoint >= 0x1dc0 && codepoint <= 0x1dff) ||
           (codepoint >= 0x20d0 && codepoint <= 0x20ff) ||
           (codepoint >= 0xfe20 && codepoint <= 0xfe2f);
}

static int unicode_display_width(unsigned int codepoint)
{
    if (unicode_is_combining(codepoint)) {
        return 0;
    }
    return unicode_is_wide(codepoint) ? 2 : 1;
}

static size_t ansi_sequence_len(const char *text, size_t len, size_t pos)
{
    if (pos >= len || text[pos] != '\033') {
        return 0;
    }
    pos++;
    if (pos < len && text[pos] == '[') {
        pos++;
        while (pos < len && !isalpha((unsigned char)text[pos])) {
            pos++;
        }
        if (pos < len) {
            pos++;
        }
        return pos;
    }
    return pos;
}

static size_t utf8_next_len(const char *text, size_t len, size_t pos, unsigned int *codepoint)
{
    const unsigned char *bytes = (const unsigned char *)text;

    if (pos >= len) {
        return 0;
    }
    if ((bytes[pos] & 0x80u) == 0) {
        *codepoint = bytes[pos];
        return 1;
    }
    if ((bytes[pos] & 0xe0u) == 0xc0u && pos + 1 < len &&
        (bytes[pos + 1] & 0xc0u) == 0x80u) {
        *codepoint = ((unsigned int)(bytes[pos] & 0x1fu) << 6) |
                     (unsigned int)(bytes[pos + 1] & 0x3fu);
        return 2;
    }
    if ((bytes[pos] & 0xf0u) == 0xe0u && pos + 2 < len &&
        (bytes[pos + 1] & 0xc0u) == 0x80u &&
        (bytes[pos + 2] & 0xc0u) == 0x80u) {
        *codepoint = ((unsigned int)(bytes[pos] & 0x0fu) << 12) |
                     ((unsigned int)(bytes[pos + 1] & 0x3fu) << 6) |
                     (unsigned int)(bytes[pos + 2] & 0x3fu);
        return 3;
    }
    if ((bytes[pos] & 0xf8u) == 0xf0u && pos + 3 < len &&
        (bytes[pos + 1] & 0xc0u) == 0x80u &&
        (bytes[pos + 2] & 0xc0u) == 0x80u &&
        (bytes[pos + 3] & 0xc0u) == 0x80u) {
        *codepoint = ((unsigned int)(bytes[pos] & 0x07u) << 18) |
                     ((unsigned int)(bytes[pos + 1] & 0x3fu) << 12) |
                     ((unsigned int)(bytes[pos + 2] & 0x3fu) << 6) |
                     (unsigned int)(bytes[pos + 3] & 0x3fu);
        return 4;
    }
    *codepoint = bytes[pos];
    return 1;
}

static int display_width(const char *text)
{
    size_t len = strlen(text);
    size_t pos = 0;
    int width = 0;

    while (pos < len) {
        unsigned int codepoint;
        size_t next_len = utf8_next_len(text, len, pos, &codepoint);
        if (next_len == 0) {
            break;
        }
        if (codepoint == '\033') {
            size_t next_pos = ansi_sequence_len(text, len, pos);
            if (next_pos > pos) {
                pos = next_pos;
                continue;
            }
            pos += next_len;
            continue;
        }
        width += unicode_display_width(codepoint);
        pos += next_len;
    }
    return width;
}

static size_t utf8_previous_start(const char *text, size_t cursor)
{
    const unsigned char *bytes = (const unsigned char *)text;

    if (cursor == 0) {
        return 0;
    }
    cursor--;
    while (cursor > 0 && (bytes[cursor] & 0xc0u) == 0x80u) {
        cursor--;
    }
    return cursor;
}

static size_t utf8_next_start(const char *text, size_t cursor)
{
    size_t len = strlen(text);
    unsigned int codepoint;
    size_t next_len;

    if (cursor >= len) {
        return len;
    }
    next_len = utf8_next_len(text, len, cursor, &codepoint);
    if (next_len == 0) {
        return len;
    }
    return cursor + next_len;
}

static size_t clamp_cursor_to_boundary(const char *text, size_t cursor)
{
    size_t len = strlen(text);
    const unsigned char *bytes = (const unsigned char *)text;

    if (cursor > len) {
        cursor = len;
    }
    while (cursor > 0 && cursor < len && (bytes[cursor] & 0xc0u) == 0x80u) {
        cursor--;
    }
    return cursor;
}

static int utf8_buffer_complete(const char *text, size_t len)
{
    size_t pos = 0;

    while (pos < len) {
        unsigned int codepoint;
        size_t next_len = utf8_next_len(text, len, pos, &codepoint);
        const unsigned char byte = (const unsigned char)text[pos];
        (void)codepoint;
        if (next_len == 0) {
            return 0;
        }
        if ((byte & 0x80u) != 0) {
            if ((byte & 0xe0u) == 0xc0u && pos + 1 >= len) {
                return 0;
            }
            if ((byte & 0xf0u) == 0xe0u && pos + 2 >= len) {
                return 0;
            }
            if ((byte & 0xf8u) == 0xf0u && pos + 3 >= len) {
                return 0;
            }
        }
        pos += next_len;
    }
    return 1;
}

static size_t utf8_prefix_for_width(const char *text, int max_width, int *used_width)
{
    size_t len = strlen(text);
    size_t pos = 0;
    int width = 0;

    while (pos < len) {
        unsigned int codepoint;
        size_t next_len = utf8_next_len(text, len, pos, &codepoint);
        int char_width;
        if (next_len == 0) {
            break;
        }
        if (codepoint == '\033') {
            size_t next_pos = ansi_sequence_len(text, len, pos);
            if (next_pos > pos) {
                pos = next_pos;
                continue;
            }
        }
        char_width = unicode_display_width(codepoint);
        if (width + char_width > max_width) {
            break;
        }
        width += char_width;
        pos += next_len;
    }
    if (used_width != NULL) {
        *used_width = width;
    }
    return pos;
}

static void erase_previous_character(client_t *client, char *buffer, size_t *len)
{
    unsigned int codepoint;
    size_t start;
    int width;

    if (*len == 0) {
        return;
    }
    if (utf8_decode_last(buffer, *len, &codepoint, &start) != 0) {
        return;
    }
    width = unicode_display_width(codepoint);
    *len = start;
    buffer[*len] = '\0';
    while (width-- > 0) {
        (void)send_text(client, "\b \b");
    }
}

static int skip_telnet_command(client_t *client, unsigned char command)
{
    unsigned char byte;

    if (command == TELNET_IAC) {
        return 1;
    }
    if (command == TELNET_DO || command == TELNET_DONT ||
        command == TELNET_WILL || command == TELNET_WONT) {
        return recv_byte(client, &byte) > 0 ? 1 : -1;
    }
    if (command == TELNET_SB) {
        int previous_iac = 0;
        for (;;) {
            int rc = recv_byte(client, &byte);
            if (rc <= 0) {
                return -1;
            }
            if (previous_iac && byte == TELNET_SE) {
                return 1;
            }
            previous_iac = byte == TELNET_IAC;
        }
    }
    return 1;
}

static void input_set_buffer(client_t *client,
                             char *buffer,
                             size_t *len,
                             const char *text)
{
    safe_copy(buffer, TENET_MAX_LINE, text);
    *len = strlen(buffer);
    safe_copy(client->input_line, sizeof(client->input_line), buffer);
    client->input_cursor = *len;
}

static void input_history_add(client_t *client, const char *line)
{
    if (line == NULL || *line == '\0') {
        return;
    }
    if (client->input_history_count > 0 &&
        strcmp(client->input_history[client->input_history_count - 1], line) == 0) {
        return;
    }
    if (client->input_history_count == TENET_INPUT_HISTORY_CAP) {
        memmove(client->input_history, client->input_history + 1,
                sizeof(client->input_history[0]) * (TENET_INPUT_HISTORY_CAP - 1));
        client->input_history_count--;
    }
    safe_copy(client->input_history[client->input_history_count], sizeof(client->input_history[0]), line);
    client->input_history_count++;
}

static void input_history_previous(client_t *client, char *buffer, size_t *len)
{
    if (client->input_history_count == 0) {
        return;
    }
    if (!client->input_history_browsing) {
        safe_copy(client->input_history_draft, sizeof(client->input_history_draft), buffer);
        client->input_history_index = (int)client->input_history_count;
        client->input_history_browsing = 1;
    }
    if (client->input_history_index > 0) {
        client->input_history_index--;
    }
    input_set_buffer(client, buffer, len, client->input_history[client->input_history_index]);
}

static void input_history_next(client_t *client, char *buffer, size_t *len)
{
    if (!client->input_history_browsing) {
        return;
    }
    if (client->input_history_index + 1 < (int)client->input_history_count) {
        client->input_history_index++;
        input_set_buffer(client, buffer, len, client->input_history[client->input_history_index]);
        return;
    }
    client->input_history_browsing = 0;
    client->input_history_index = -1;
    input_set_buffer(client, buffer, len, client->input_history_draft);
    client->input_history_draft[0] = '\0';
}

static void input_cancel_history_browse(client_t *client)
{
    client->input_history_browsing = 0;
    client->input_history_index = -1;
    client->input_history_draft[0] = '\0';
}

static void input_delete_before_cursor(client_t *client, char *buffer, size_t *len)
{
    size_t start;

    if (client->input_cursor == 0) {
        return;
    }
    start = utf8_previous_start(buffer, client->input_cursor);
    memmove(buffer + start, buffer + client->input_cursor, *len - client->input_cursor + 1);
    *len -= client->input_cursor - start;
    client->input_cursor = start;
    safe_copy(client->input_line, sizeof(client->input_line), buffer);
    input_cancel_history_browse(client);
}

static void input_delete_at_cursor(client_t *client, char *buffer, size_t *len)
{
    size_t next;

    if (client->input_cursor >= *len) {
        return;
    }
    next = utf8_next_start(buffer, client->input_cursor);
    memmove(buffer + client->input_cursor, buffer + next, *len - next + 1);
    *len -= next - client->input_cursor;
    safe_copy(client->input_line, sizeof(client->input_line), buffer);
    input_cancel_history_browse(client);
}

static void input_insert_byte(client_t *client, char *buffer, size_t *len, size_t size, unsigned char byte)
{
    if (*len + 1 >= size) {
        return;
    }
    client->input_cursor = clamp_cursor_to_boundary(buffer, client->input_cursor);
    memmove(buffer + client->input_cursor + 1,
            buffer + client->input_cursor,
            *len - client->input_cursor + 1);
    buffer[client->input_cursor++] = (char)byte;
    (*len)++;
    safe_copy(client->input_line, sizeof(client->input_line), buffer);
    input_cancel_history_browse(client);
}

static void input_move_left(client_t *client)
{
    client->input_cursor = utf8_previous_start(client->input_line, client->input_cursor);
}

static void input_move_right(client_t *client)
{
    client->input_cursor = utf8_next_start(client->input_line, client->input_cursor);
}

static void input_autocomplete(client_t *client, char *buffer, size_t *len, size_t size)
{
    const char *prefix_start = "/pm ";
    size_t prefix_len = strlen(prefix_start);
    char partial[TENET_MAX_USERNAME];
    client_t *cursor;
    const client_t *match = NULL;
    size_t match_count = 0;
    size_t partial_len;

    if (!client->in_chat || strncmp(buffer, prefix_start, prefix_len) != 0 ||
        client->input_cursor < prefix_len) {
        return;
    }
    if (strchr(buffer + prefix_len, ' ') != NULL) {
        return;
    }
    partial_len = *len - prefix_len;
    if (partial_len >= sizeof(partial)) {
        partial_len = sizeof(partial) - 1;
    }
    memcpy(partial, buffer + prefix_len, partial_len);
    partial[partial_len] = '\0';

    pthread_mutex_lock(&global_state->mutex);
    for (cursor = global_state->clients; cursor != NULL; cursor = cursor->next) {
        if (!cursor->active || !cursor->in_chat || cursor == client) {
            continue;
        }
        if (ascii_starts_with_ignore_case(cursor->username, partial) ||
            ascii_starts_with_ignore_case(client_name(cursor), partial)) {
            match = cursor;
            match_count++;
        }
    }
    if (match_count == 1 && match != NULL) {
        char completed[TENET_MAX_LINE];

        snprintf(completed, sizeof(completed), "/pm %s", match->username);
        if (strlen(completed) < size) {
            input_set_buffer(client, buffer, len, completed);
        }
    }
    pthread_mutex_unlock(&global_state->mutex);
}

static void scroll_history(client_t *client, int rows)
{
    if (rows > 0 && client->history_scroll_rows > INT_MAX - rows) {
        client->history_scroll_rows = INT_MAX;
    } else {
        client->history_scroll_rows += rows;
    }
    if (client->history_scroll_rows < 0) {
        client->history_scroll_rows = 0;
    }
}

static int private_peer_index(const client_t *client, const char *username)
{
    size_t i;

    for (i = 0; i < client->private_peer_count; i++) {
        if (ascii_equal_ignore_case(client->private_peers[i], username)) {
            return (int)i;
        }
    }
    return -1;
}

static int add_private_peer(client_t *client, const char *username)
{
    if (username == NULL || *username == '\0' ||
        ascii_equal_ignore_case(username, client->username)) {
        return -1;
    }
    if (private_peer_index(client, username) >= 0) {
        return 0;
    }
    if (client->private_peer_count >= TENET_MAX_PM_TABS) {
        return -1;
    }
    safe_copy(client->private_peers[client->private_peer_count],
              sizeof(client->private_peers[0]), username);
    client->private_peer_unread[client->private_peer_count] = 0;
    client->private_peer_count++;
    return 0;
}

static void switch_to_lobby(client_t *client)
{
    client->active_peer_username[0] = '\0';
    client->lobby_unread = 0;
    client->history_scroll_rows = 0;
}

static int switch_to_private_peer(client_t *client, const char *username)
{
    int index;

    if (private_peer_index(client, username) < 0 && add_private_peer(client, username) != 0) {
        return -1;
    }
    index = private_peer_index(client, username);
    if (index >= 0) {
        client->private_peer_unread[index] = 0;
    }
    safe_copy(client->active_peer_username, sizeof(client->active_peer_username), username);
    client->history_scroll_rows = 0;
    client->status_line[0] = '\0';
    return 0;
}

static int remove_active_private_peer(client_t *client)
{
    int index;

    if (client->active_peer_username[0] == '\0') {
        return -1;
    }
    index = private_peer_index(client, client->active_peer_username);
    if (index >= 0) {
        size_t remaining = client->private_peer_count - (size_t)index - 1;

        if (remaining > 0) {
            memmove(client->private_peers[index], client->private_peers[index + 1],
                    remaining * sizeof(client->private_peers[0]));
            memmove(client->private_peer_unread + index,
                    client->private_peer_unread + index + 1,
                    remaining * sizeof(client->private_peer_unread[0]));
        }
        client->private_peer_count--;
    }
    switch_to_lobby(client);
    return 0;
}

static int tab_segment_width(const char *label, int active, int unread)
{
    return display_width(label) + (unread ? 1 : 0) + (active ? 4 : 2);
}

static void append_chat_tab(char *buf, size_t size, const char *label, int active, int unread)
{
    size_t used = strlen(buf);

    if (used >= size) {
        return;
    }
    if (unread) {
        snprintf(buf + used, size - used,
                 active ? " [" ANSI_YELLOW ANSI_BOLD "*%s" ANSI_RESET "] " :
                          " " ANSI_YELLOW ANSI_BOLD "*%s" ANSI_RESET " ",
                 label);
    } else {
        snprintf(buf + used, size - used, active ? " [%s] " : " %s ", label);
    }
}

static void format_chat_title(const client_t *client, char *buf, size_t size)
{
    size_t i;

    if (size == 0) {
        return;
    }
    buf[0] = '\0';
    append_chat_tab(buf, size, "大厅",
                    client->active_peer_username[0] == '\0',
                    client->lobby_unread);
    for (i = 0; i < client->private_peer_count; i++) {
        client_t *peer = global_state != NULL ?
                         find_client_by_username_locked(global_state, client->private_peers[i]) : NULL;
        const char *label = peer != NULL ? client_name(peer) : client->private_peers[i];

        append_chat_tab(buf, size, label,
                        ascii_equal_ignore_case(client->active_peer_username,
                                                client->private_peers[i]),
                        client->private_peer_unread[i]);
    }
}

static int switch_chat_tab_at_col(server_state_t *state, client_t *client, int col)
{
    int cursor = 3;
    int width;
    size_t i;

    width = tab_segment_width("大厅", client->active_peer_username[0] == '\0',
                              client->lobby_unread);
    if (col >= cursor && col < cursor + width) {
        switch_to_lobby(client);
        return 1;
    }
    cursor += width;

    for (i = 0; i < client->private_peer_count; i++) {
        const char *peer = client->private_peers[i];
        client_t *peer_client = find_client_by_username_locked(state, peer);
        const char *label = peer_client != NULL ? client_name(peer_client) : peer;

        width = tab_segment_width(label,
                                  ascii_equal_ignore_case(client->active_peer_username, peer),
                                  client->private_peer_unread[i]);
        if (col >= cursor && col < cursor + width) {
            (void)switch_to_private_peer(client, peer);
            return 1;
        }
        cursor += width;
    }
    return 0;
}

static client_t *sidebar_client_at_row_locked(server_state_t *state,
                                              const client_t *viewer,
                                              int row)
{
    client_t *cursor;
    int current_row = 5;

    (void)viewer;
    for (cursor = state->clients; cursor != NULL; cursor = cursor->next) {
        if (!cursor->in_chat) {
            continue;
        }
        if (current_row == row) {
            return cursor;
        }
        current_row++;
    }
    return NULL;
}

static void handle_mouse_event(client_t *client, int button, int col, int row, int pressed)
{
    if (!client->in_chat || global_state == NULL) {
        return;
    }
    if ((button & 64) == 0) {
        ui_layout_t layout;

        if (!pressed || (button & 3) != 0) {
            return;
        }
        compute_layout(client, &layout);
        if (row == layout.chat_top_row && col < layout.left_cols) {
            pthread_mutex_lock(&global_state->mutex);
            (void)switch_chat_tab_at_col(global_state, client, col);
            pthread_mutex_unlock(&global_state->mutex);
        } else if (layout.sidebar_cols > 0 && col >= layout.sidebar_col &&
                   col < layout.sidebar_col + layout.sidebar_cols) {
            pthread_mutex_lock(&global_state->mutex);
            client_t *peer = sidebar_client_at_row_locked(global_state, client, row);
            if (peer != NULL && peer != client) {
                (void)switch_to_private_peer(client, peer->username);
                snprintf(client->status_line, sizeof(client->status_line),
                         "正在和 %s 私聊。输入 /close 关闭此私聊。", client_name(peer));
            }
            pthread_mutex_unlock(&global_state->mutex);
        }
        return;
    }
    pthread_mutex_lock(&global_state->mutex);
    if ((button & 3) == 0) {
        scroll_history(client, TENET_SCROLL_WHEEL_ROWS);
    } else if ((button & 3) == 1) {
        scroll_history(client, -TENET_SCROLL_WHEEL_ROWS);
    }
    pthread_mutex_unlock(&global_state->mutex);
}

static int csi_is_final_byte(unsigned char byte)
{
    return byte >= 0x40 && byte <= 0x7e;
}

static size_t parse_csi_params(const char *body, size_t body_len, int *params, size_t max_params)
{
    size_t count = 0;
    size_t pos = 0;

    while (pos < body_len && count < max_params) {
        int value = 0;
        int seen_digit = 0;

        while (pos < body_len && !isdigit((unsigned char)body[pos])) {
            pos++;
        }
        while (pos < body_len && isdigit((unsigned char)body[pos])) {
            value = value * 10 + body[pos] - '0';
            seen_digit = 1;
            pos++;
        }
        if (seen_digit) {
            params[count++] = value;
        }
    }
    return count;
}

static int csi_modifier_has_shift(int modifier)
{
    return modifier > 1 && ((modifier - 1) & 1) != 0;
}

static int csi_is_shift_enter(char final, const int *params, size_t count)
{
    if ((final == 'u' || final == 'U') &&
        count >= 2 && params[0] == 13 && csi_modifier_has_shift(params[1])) {
        return 1;
    }

    if (final == '~') {
        if (count >= 3 && params[0] == 27 && csi_modifier_has_shift(params[1]) && params[2] == 13) {
            return 1;
        }
        if (count >= 2 && params[0] == 13 && csi_modifier_has_shift(params[1])) {
            return 1;
        }
    }
    return 0;
}

static void handle_csi_sequence(client_t *client,
                                char *buffer,
                                size_t *len,
                                size_t size,
                                char final,
                                const int *params,
                                size_t param_count)
{
    if (csi_is_shift_enter(final, params, param_count)) {
        input_insert_byte(client, buffer, len, size, '\n');
        return;
    }

    if ((final == 'M' || final == 'm') && param_count >= 3) {
        handle_mouse_event(client, params[0], params[1], params[2], final == 'M');
        return;
    }

    if (final == 'A') {
        input_history_previous(client, buffer, len);
    } else if (final == 'B') {
        input_history_next(client, buffer, len);
    } else if (final == 'C') {
        input_move_right(client);
    } else if (final == 'D') {
        input_move_left(client);
    } else if (final == 'H') {
        client->input_cursor = 0;
    } else if (final == 'F') {
        client->input_cursor = *len;
    } else if (final == '~' && param_count > 0) {
        if (params[0] == 3) {
            input_delete_at_cursor(client, buffer, len);
        } else if (params[0] == 1 || params[0] == 7) {
            client->input_cursor = 0;
        } else if (params[0] == 4 || params[0] == 8) {
            client->input_cursor = *len;
        }
    }
}

static int read_escape_sequence(client_t *client,
                                char *buffer,
                                size_t *len,
                                size_t size,
                                int secret)
{
    unsigned char byte;
    int rc;
    (void)secret;

    rc = recv_byte(client, &byte);
    if (rc <= 0) {
        return rc;
    }

    if (byte == 'O') {
        rc = recv_byte(client, &byte);
        if (rc <= 0) {
            return rc;
        }
        if (byte == 'A') {
            input_history_previous(client, buffer, len);
        } else if (byte == 'B') {
            input_history_next(client, buffer, len);
        } else if (byte == 'C') {
            input_move_right(client);
        } else if (byte == 'D') {
            input_move_left(client);
        } else if (byte == 'H') {
            client->input_cursor = 0;
        } else if (byte == 'F') {
            client->input_cursor = *len;
        }
        safe_copy(client->input_line, sizeof(client->input_line), buffer);
        return 1;
    }

    if (byte == '\r' || byte == '\n') {
        input_insert_byte(client, buffer, len, size, '\n');
        safe_copy(client->input_line, sizeof(client->input_line), buffer);
        return 1;
    }
    if (byte != '[') {
        return 1;
    }


    {
        char sequence[32];
        size_t used = 0;
        char final = '\0';
        int params[8];
        size_t param_count;

        rc = recv_byte(client, &byte);
        if (rc <= 0) {
            return rc;
        }
        if (byte == 'M') {
            unsigned char mouse_button;
            unsigned char mouse_x;
            unsigned char mouse_y;

            rc = recv_byte(client, &mouse_button);
            if (rc <= 0) {
                return rc;
            }
            rc = recv_byte(client, &mouse_x);
            if (rc <= 0) {
                return rc;
            }
            rc = recv_byte(client, &mouse_y);
            if (rc <= 0) {
                return rc;
            }
            (void)mouse_x;
            (void)mouse_y;
            handle_mouse_event(client, (int)mouse_button - 32,
                               (int)mouse_x - 32,
                               (int)mouse_y - 32,
                               1);
            safe_copy(client->input_line, sizeof(client->input_line), buffer);
            return 1;
        }
        sequence[used++] = (char)byte;
        if (csi_is_final_byte(byte)) {
            final = (char)byte;
        }

        while (final == '\0' && used + 1 < sizeof(sequence)) {
            rc = recv_byte(client, &byte);
            if (rc <= 0) {
                return rc;
            }
            sequence[used++] = (char)byte;
            if (csi_is_final_byte(byte)) {
                final = (char)byte;
                break;
            }
        }
        if (final == '\0') {
            safe_copy(client->input_line, sizeof(client->input_line), buffer);
            return 1;
        }
        sequence[used] = '\0';
        param_count = parse_csi_params(sequence, used - 1, params, sizeof(params) / sizeof(params[0]));
        handle_csi_sequence(client, buffer, len, size, final, params, param_count);
    }

    safe_copy(client->input_line, sizeof(client->input_line), buffer);
    return 1;
}

int read_line(client_t *client, char *buffer, size_t size, int secret)
{
    size_t len = 0;

    if (size == 0) {
        return -1;
    }
    buffer[0] = '\0';
    if (client->in_chat) {
        client->input_line[0] = '\0';
        client->input_cursor = 0;
        input_cancel_history_browse(client);
        refresh_client_screen(global_state, client);
    }

    for (;;) {
        unsigned char byte;
        int rc = recv_byte(client, &byte);
        if (rc <= 0) {
            return rc;
        }

        if (byte == TELNET_IAC) {
            rc = recv_byte(client, &byte);
            if (rc <= 0) {
                return rc;
            }
            if (skip_telnet_command(client, byte) < 0) {
                return -1;
            }
            continue;
        }

        if (byte == TENET_CONTROL_BYTE) {
            if (read_control_sequence(client) <= 0) {
                return 0;
            }
            continue;
        }

        if (client->skip_cr_tail) {
            client->skip_cr_tail = 0;
            if (byte == '\n' || byte == '\0') {
                continue;
            }
        }

        if (byte == '\r') {
            buffer[len] = '\0';
            client->skip_cr_tail = 1;
            if (client->in_chat) {
                input_history_add(client, buffer);
                client->history_scroll_rows = 0;
                client->input_line[0] = '\0';
                client->input_cursor = 0;
                input_cancel_history_browse(client);
                refresh_client_screen(global_state, client);
            } else {
                (void)send_text(client, "\r\n");
            }
            return 1;
        }
        if (byte == '\n') {
            buffer[len] = '\0';
            if (client->in_chat) {
                input_history_add(client, buffer);
                client->history_scroll_rows = 0;
                client->input_line[0] = '\0';
                client->input_cursor = 0;
                input_cancel_history_browse(client);
                refresh_client_screen(global_state, client);
            } else {
                (void)send_text(client, "\r\n");
            }
            return 1;
        }
        if (byte == 3 || byte == 4) {
            return 0;
        }
        if (client->in_chat && byte == '\t') {
            input_autocomplete(client, buffer, &len, size);
            refresh_client_input(global_state, client);
            continue;
        }
        if (client->in_chat && byte == 1) {
            client->input_cursor = 0;
            refresh_client_input(global_state, client);
            continue;
        }
        if (client->in_chat && byte == 5) {
            client->input_cursor = len;
            refresh_client_input(global_state, client);
            continue;
        }
        if (client->in_chat && byte == 2) {
            input_move_left(client);
            refresh_client_input(global_state, client);
            continue;
        }
        if (client->in_chat && byte == 6) {
            input_move_right(client);
            refresh_client_input(global_state, client);
            continue;
        }
        if (client->in_chat && byte == 16) {
            input_history_previous(client, buffer, &len);
            refresh_client_input(global_state, client);
            continue;
        }
        if (client->in_chat && byte == 14) {
            input_history_next(client, buffer, &len);
            refresh_client_input(global_state, client);
            continue;
        }
        if (client->in_chat && byte == 11) {
            buffer[client->input_cursor] = '\0';
            len = client->input_cursor;
            safe_copy(client->input_line, sizeof(client->input_line), buffer);
            input_cancel_history_browse(client);
            refresh_client_input(global_state, client);
            continue;
        }
        if (client->in_chat && byte == 21) {
            memmove(buffer, buffer + client->input_cursor, len - client->input_cursor + 1);
            len -= client->input_cursor;
            client->input_cursor = 0;
            safe_copy(client->input_line, sizeof(client->input_line), buffer);
            input_cancel_history_browse(client);
            refresh_client_input(global_state, client);
            continue;
        }
        if (client->in_chat && byte == 27) {
            if (read_escape_sequence(client, buffer, &len, size, secret) <= 0) {
                return 0;
            }
            refresh_client_screen(global_state, client);
            continue;
        }
        if (byte == 8 || byte == 127) {
            if (client->in_chat) {
                input_delete_before_cursor(client, buffer, &len);
                refresh_client_input(global_state, client);
            } else {
                erase_previous_character(client, buffer, &len);
            }
            continue;
        }
        if (byte < 32 || byte == 255) {
            continue;
        }
        if (len + 1 < size) {
            if (secret) {
                buffer[len++] = (char)byte;
                buffer[len] = '\0';
                (void)send_text(client, "*");
            } else if (client->in_chat) {
                input_insert_byte(client, buffer, &len, size, byte);
                if (utf8_buffer_complete(buffer, len)) {
                    safe_copy(client->input_line, sizeof(client->input_line), buffer);
                    refresh_client_input(global_state, client);
                }
            } else {
                buffer[len++] = (char)byte;
                buffer[len] = '\0';
                (void)send_bytes_unlocked(client, &byte, 1);
            }
        }
    }
}

static int read_plain_line_fd(int fd, char *buffer, size_t size)
{
    size_t len = 0;

    if (size == 0) {
        return -1;
    }
    buffer[0] = '\0';
    while (len + 1 < size) {
        unsigned char byte;
        ssize_t got = recv(fd, &byte, 1, 0);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (got == 0) {
            return 0;
        }
        if (byte == '\r') {
            continue;
        }
        if (byte == '\n') {
            buffer[len] = '\0';
            return 1;
        }
        buffer[len++] = (char)byte;
    }
    buffer[len] = '\0';
    return 1;
}

static int parse_window_size(const char *text, int *rows, int *cols)
{
    int parsed_rows;
    int parsed_cols;

    if (sscanf(text, "%d %d", &parsed_rows, &parsed_cols) != 2 &&
        sscanf(text, "%d;%d", &parsed_rows, &parsed_cols) != 2) {
        return -1;
    }
    if (parsed_rows <= 0 || parsed_cols <= 0) {
        return -1;
    }
    *rows = clamp_int(parsed_rows, TENET_MIN_SCREEN_ROWS, TENET_MAX_SCREEN_ROWS);
    *cols = clamp_int(parsed_cols, TENET_MIN_SCREEN_COLS, TENET_MAX_SCREEN_COLS);
    return 0;
}

static int read_control_sequence(client_t *client)
{
    char line[64];
    size_t len = 0;

    while (len + 1 < sizeof(line)) {
        unsigned char byte;
        int rc = recv_byte(client, &byte);
        if (rc <= 0) {
            return rc;
        }
        if (byte == '\r') {
            continue;
        }
        if (byte == '\n') {
            break;
        }
        line[len++] = (char)byte;
    }
    line[len] = '\0';

    if (line[0] == 'W') {
        int rows;
        int cols;
        if (parse_window_size(line + 1, &rows, &cols) == 0) {
            client->screen_rows = rows;
            client->screen_cols = cols;
            if (client->in_chat) {
                refresh_client_screen(global_state, client);
            }
        }
    }
    return 1;
}

int valid_username(const char *username)
{
    size_t len = strlen(username);
    size_t i;

    if (len < 1 || len >= TENET_MAX_USERNAME) {
        return 0;
    }
    for (i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)username[i];
        if (!(isalnum(ch) || ch == '.' || ch == '_' || ch == '-' || ch == '@')) {
            return 0;
        }
    }
    return 1;
}

int valid_display_name(const char *display_name)
{
    size_t len = strlen(display_name);
    size_t i;

    if (len < 1 || len >= TENET_MAX_DISPLAY_NAME) {
        return 0;
    }
    for (i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)display_name[i];
        if (ch < 0x20 || ch == 0x7f) {
            return 0;
        }
    }
    return 1;
}

static int add_client_if_username_free(server_state_t *state, client_t *client)
{
    client_t *cursor;

    pthread_mutex_lock(&state->mutex);
    for (cursor = state->clients; cursor != NULL; cursor = cursor->next) {
        if (ascii_equal_ignore_case(cursor->username, client->username)) {
            pthread_mutex_unlock(&state->mutex);
            return -1;
        }
    }
    client->active = 1;
    client->next = state->clients;
    state->clients = client;
    if (client->in_chat) {
        state->online_count++;
    }
    pthread_mutex_unlock(&state->mutex);
    return 0;
}

static void mark_client_in_chat(server_state_t *state, client_t *client)
{
    pthread_mutex_lock(&state->mutex);
    if (!client->in_chat) {
        client->in_chat = 1;
        state->online_count++;
    }
    pthread_mutex_unlock(&state->mutex);
}

static void remove_client(server_state_t *state, client_t *client)
{
    client_t **cursor;
    int was_in_chat;

    pthread_mutex_lock(&state->mutex);
    cursor = &state->clients;
    while (*cursor != NULL) {
        if (*cursor == client) {
            *cursor = client->next;
            break;
        }
        cursor = &(*cursor)->next;
    }
    was_in_chat = client->in_chat;
    client->active = 0;
    client->in_chat = 0;
    client->next = NULL;
    if (was_in_chat && state->online_count > 0) {
        state->online_count--;
    }
    pthread_mutex_unlock(&state->mutex);
}

static void destroy_client(client_t *client)
{
    if (client == NULL) {
        return;
    }
    free(client->write_buffer);
    pthread_mutex_destroy(&client->write_mutex);
    free(client);
}

static const char *client_name(const client_t *client)
{
    return client->display_name[0] != '\0' ? client->display_name : client->username;
}

static int history_entry_is_private(const history_entry_t *entry)
{
    return entry->private_from_username[0] != '\0' || entry->private_to_username[0] != '\0';
}

static int history_entry_matches_active_private(const history_entry_t *entry, const client_t *client)
{
    if (client->active_peer_username[0] == '\0') {
        return 0;
    }
    return (ascii_equal_ignore_case(entry->private_from_username, client->username) &&
            ascii_equal_ignore_case(entry->private_to_username, client->active_peer_username)) ||
           (ascii_equal_ignore_case(entry->private_from_username, client->active_peer_username) &&
            ascii_equal_ignore_case(entry->private_to_username, client->username));
}

static int history_entry_visible_to_client(const history_entry_t *entry, const client_t *client)
{
    if (entry->target_username[0] != '\0' &&
        !ascii_equal_ignore_case(entry->target_username, client->username)) {
        return 0;
    }
    if (history_entry_is_private(entry)) {
        return history_entry_matches_active_private(entry, client);
    }
    if (client->active_peer_username[0] != '\0') {
        return 0;
    }
    return 1;
}

static void draw_truncated_unlocked(client_t *client, const char *text, int width)
{
    int used_width = 0;
    size_t bytes = utf8_prefix_for_width(text, width, &used_width);

    if (width <= 0) {
        return;
    }
    (void)send_bytes_unlocked(client, text, bytes);
    (void)send_text_unlocked(client, ANSI_RESET);
    if (used_width < width) {
        send_spaces_unlocked(client, width - used_width);
    }
}

static const char *skip_utf8_width(const char *text, int skip_width)
{
    size_t len = strlen(text);
    size_t pos = 0;
    int width = 0;

    while (pos < len && width < skip_width) {
        unsigned int codepoint;
        size_t next_len = utf8_next_len(text, len, pos, &codepoint);
        if (next_len == 0) {
            break;
        }
        if (codepoint == '\033') {
            size_t next_pos = ansi_sequence_len(text, len, pos);
            if (next_pos > pos) {
                pos = next_pos;
                continue;
            }
        }
        width += unicode_display_width(codepoint);
        pos += next_len;
    }
    return text + pos;
}

static size_t wrapped_row_count_for_width(const char *text, int width)
{
    int line_width;

    if (width <= 0) {
        return 1;
    }
    line_width = display_width(text);
    if (line_width <= 0) {
        return 1;
    }
    return (size_t)((line_width + width - 1) / width);
}

static size_t collect_wrapped_history_locked(server_state_t *state,
                                             client_t *client,
                                             const char **lines,
                                             int *offsets,
                                             size_t max_lines,
                                             int width)
{
    size_t available_start = state->history_total > state->history_count ?
                             state->history_total - state->history_count : 0;
    size_t start_total = client->history_base > available_start ? client->history_base : available_start;
    size_t total_rows = 0;
    size_t first_row = 0;
    size_t current_row = 0;
    size_t count = 0;
    size_t total;

    if (max_lines == 0 || width <= 0) {
        return 0;
    }

    for (total = start_total; total < state->history_total; total++) {
        size_t logical = total - available_start;
        size_t ring_index = (state->history_next + TENET_HISTORY_CAP - state->history_count + logical) % TENET_HISTORY_CAP;
        const history_entry_t *entry = &state->history[ring_index];

        if (!history_entry_visible_to_client(entry, client)) {
            continue;
        }
        total_rows += wrapped_row_count_for_width(entry->line, width);
    }

    if (total_rows <= max_lines) {
        client->history_scroll_rows = 0;
    } else {
        size_t max_scroll = total_rows - max_lines;

        if (max_scroll > (size_t)INT_MAX) {
            max_scroll = (size_t)INT_MAX;
        }
        if (client->history_scroll_rows < 0) {
            client->history_scroll_rows = 0;
        }
        if ((size_t)client->history_scroll_rows > max_scroll) {
            client->history_scroll_rows = (int)max_scroll;
        }
        first_row = total_rows - max_lines - (size_t)client->history_scroll_rows;
    }

    for (total = start_total; total < state->history_total; total++) {
        size_t logical = total - available_start;
        size_t ring_index = (state->history_next + TENET_HISTORY_CAP - state->history_count + logical) % TENET_HISTORY_CAP;
        const history_entry_t *entry = &state->history[ring_index];
        const char *line = entry->line;
        int line_width = display_width(line);
        int offset = 0;

        if (!history_entry_visible_to_client(entry, client)) {
            continue;
        }

        if (line_width == 0) {
            if (current_row >= first_row && count < max_lines) {
                lines[count] = line;
                offsets[count] = 0;
                count++;
            }
            current_row++;
            if (count == max_lines) {
                break;
            }
            continue;
        }

        while (offset < line_width) {
            if (current_row >= first_row && count < max_lines) {
                lines[count] = line;
                offsets[count] = offset;
                count++;
            }
            current_row++;
            if (count == max_lines) {
                break;
            }
            offset += width;
        }
        if (count == max_lines) {
            break;
        }
    }
    return count;
}

static int clamp_int(int value, int min_value, int max_value)
{
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

static int text_position_is_newline(const char *text, size_t len, size_t pos)
{
    unsigned int codepoint;

    return pos < len && utf8_next_len(text, len, pos, &codepoint) > 0 && codepoint == '\n';
}

static int visual_row_count_for_text(const char *text, int width)
{
    size_t len = strlen(text);
    size_t pos = 0;
    int rows = 1;
    int col = 0;

    if (width <= 0) {
        return 1;
    }
    while (pos < len) {
        unsigned int codepoint;
        size_t next_len = utf8_next_len(text, len, pos, &codepoint);
        int char_width;

        if (next_len == 0) {
            break;
        }
        if (codepoint == '\n') {
            rows++;
            col = 0;
            pos += next_len;
            continue;
        }
        char_width = unicode_display_width(codepoint);
        if (col > 0 && col + char_width > width) {
            rows++;
            col = 0;
        }
        col += char_width;
        if (col >= width && pos + next_len < len &&
            !text_position_is_newline(text, len, pos + next_len)) {
            rows++;
            col = 0;
        }
        pos += next_len;
    }
    return rows;
}

static void visual_position_for_cursor(const char *text,
                                       size_t cursor,
                                       int width,
                                       int *row,
                                       int *col)
{
    size_t len = strlen(text);
    size_t pos = 0;

    *row = 0;
    *col = 0;
    if (width <= 0) {
        return;
    }
    if (cursor > len) {
        cursor = len;
    }
    while (pos < cursor) {
        unsigned int codepoint;
        size_t next_len = utf8_next_len(text, len, pos, &codepoint);
        int char_width;

        if (next_len == 0 || pos + next_len > cursor) {
            break;
        }
        if (codepoint == '\n') {
            (*row)++;
            *col = 0;
            pos += next_len;
            continue;
        }
        char_width = unicode_display_width(codepoint);
        if (*col > 0 && *col + char_width > width) {
            (*row)++;
            *col = 0;
        }
        *col += char_width;
        if (*col >= width && pos + next_len < cursor &&
            !text_position_is_newline(text, len, pos + next_len)) {
            (*row)++;
            *col = 0;
        }
        pos += next_len;
    }
}

static void copy_visual_row(const char *text,
                            int width,
                            int target_row,
                            char *out,
                            size_t out_size)
{
    size_t len = strlen(text);
    size_t pos = 0;
    size_t used = 0;
    int row = 0;
    int col = 0;

    if (out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (width <= 0) {
        return;
    }
    while (pos < len) {
        unsigned int codepoint;
        size_t next_len = utf8_next_len(text, len, pos, &codepoint);
        int char_width;

        if (next_len == 0) {
            break;
        }
        if (codepoint == '\n') {
            if (row == target_row) {
                break;
            }
            row++;
            col = 0;
            pos += next_len;
            continue;
        }
        char_width = unicode_display_width(codepoint);
        if (col > 0 && col + char_width > width) {
            if (row == target_row) {
                break;
            }
            row++;
            col = 0;
        }
        if (row == target_row) {
            if (used + next_len >= out_size) {
                break;
            }
            memcpy(out + used, text + pos, next_len);
            used += next_len;
            out[used] = '\0';
        }
        col += char_width;
        pos += next_len;
        if (col >= width && pos < len && !text_position_is_newline(text, len, pos)) {
            if (row == target_row) {
                break;
            }
            row++;
            col = 0;
        }
        if (row > target_row) {
            break;
        }
    }
}

static void compute_layout(const client_t *client, ui_layout_t *layout)
{
    int input_rows_needed;

    layout->rows = clamp_int(client->screen_rows > 0 ? client->screen_rows : TENET_DEFAULT_SCREEN_ROWS,
                             TENET_MIN_SCREEN_ROWS, TENET_MAX_SCREEN_ROWS);
    layout->cols = clamp_int(client->screen_cols > 0 ? client->screen_cols : TENET_DEFAULT_SCREEN_COLS,
                             TENET_MIN_SCREEN_COLS, TENET_MAX_SCREEN_COLS);

    if (layout->cols >= 70) {
        layout->sidebar_cols = clamp_int(layout->cols / 4,
                                         TENET_MIN_SIDEBAR_COLS,
                                         TENET_MAX_SIDEBAR_COLS);
        if (layout->cols - layout->sidebar_cols < 38) {
            layout->sidebar_cols = layout->cols - 38;
        }
    } else {
        layout->sidebar_cols = 0;
    }
    if (layout->sidebar_cols < TENET_MIN_SIDEBAR_COLS) {
        layout->sidebar_cols = 0;
    }

    layout->left_cols = layout->cols - layout->sidebar_cols;
    layout->sidebar_col = layout->left_cols + 1;
    layout->chat_inner_width = layout->left_cols - 4;
    layout->sidebar_inner_width = layout->sidebar_cols > 0 ? layout->sidebar_cols - 4 : 0;
    layout->input_text_width = layout->left_cols - 6;
    if (layout->input_text_width < 1) {
        layout->input_text_width = 1;
    }

    input_rows_needed = visual_row_count_for_text(client->input_line, layout->input_text_width);
    layout->input_inner_rows = clamp_int(input_rows_needed, 1, TENET_MAX_INPUT_ROWS);
    if (layout->input_inner_rows > layout->rows - 8) {
        layout->input_inner_rows = layout->rows - 8;
    }
    if (layout->input_inner_rows < 1) {
        layout->input_inner_rows = 1;
    }

    layout->input_bottom_row = layout->rows;
    layout->input_top_row = layout->input_bottom_row - layout->input_inner_rows - 1;
    layout->chat_top_row = 2;
    layout->chat_first_row = 3;
    layout->chat_bottom_row = layout->input_top_row - 1;
    layout->chat_rows = layout->chat_bottom_row - layout->chat_first_row;
    if (layout->chat_rows < 1) {
        layout->chat_rows = 1;
    }
    if (layout->chat_rows > TENET_MAX_VISIBLE_ROWS) {
        layout->chat_rows = TENET_MAX_VISIBLE_ROWS;
    }
}

static void draw_border_unlocked(client_t *client, int row, int col, int width, const char *title)
{
    int title_width = title != NULL ? display_width(title) : 0;
    int title_limit;
    size_t title_bytes;
    int fill;

    if (width < 2) {
        return;
    }
    if (title == NULL || *title == '\0') {
        (void)send_fmt_unlocked(client, "\033[%d;%dH" ANSI_CYAN "+", row, col);
        for (fill = 0; fill < width - 2; fill++) {
            (void)send_text_unlocked(client, "-");
        }
        (void)send_text_unlocked(client, "+" ANSI_RESET);
        return;
    }

    title_limit = width - 4;
    if (title_limit < 0) {
        title_limit = 0;
    }
    title_bytes = utf8_prefix_for_width(title, title_limit, &title_width);
    fill = width - title_width - 4;
    if (fill < 0) {
        fill = 0;
    }
    (void)send_fmt_unlocked(client, "\033[%d;%dH" ANSI_CYAN "+-", row, col);
    (void)send_bytes_unlocked(client, title, title_bytes);
    (void)send_text_unlocked(client, ANSI_CYAN "-");
    while (fill-- > 0) {
        (void)send_text_unlocked(client, "-");
    }
    (void)send_text_unlocked(client, "+" ANSI_RESET);
}

static void draw_box_line_unlocked(client_t *client,
                                   int row,
                                   int col,
                                   int width,
                                   const char *text)
{
    int inner_width = width - 4;

    (void)send_fmt_unlocked(client, "\033[%d;%dH" ANSI_CYAN "|" ANSI_RESET " ", row, col);
    draw_truncated_unlocked(client, text != NULL ? text : "", inner_width);
    (void)send_text_unlocked(client, " " ANSI_CYAN "|" ANSI_RESET);
}

static void draw_input_unlocked(client_t *client, const ui_layout_t *layout)
{
    int total_rows = visual_row_count_for_text(client->input_line, layout->input_text_width);
    int cursor_logical_row = 0;
    int cursor_col_in_row = 0;
    int first_visible;
    int row;
    int cursor_row = layout->input_top_row + 1;
    int cursor_col = 5;

    visual_position_for_cursor(client->input_line, client->input_cursor,
                               layout->input_text_width,
                               &cursor_logical_row,
                               &cursor_col_in_row);
    first_visible = cursor_logical_row >= layout->input_inner_rows ?
                    cursor_logical_row - layout->input_inner_rows + 1 : 0;
    if (first_visible + layout->input_inner_rows > total_rows) {
        first_visible = total_rows > layout->input_inner_rows ? total_rows - layout->input_inner_rows : 0;
    }

    for (row = 0; row < layout->input_inner_rows; row++) {
        int logical_row = first_visible + row;
        char visual_line[TENET_MAX_LINE];

        copy_visual_row(client->input_line, layout->input_text_width,
                        logical_row, visual_line, sizeof(visual_line));
        (void)send_fmt_unlocked(client,
                                "\033[%d;1H" ANSI_CYAN "|" ANSI_RESET " %s ",
                                layout->input_top_row + 1 + row,
                                row == 0 ? ANSI_BOLD ">" ANSI_RESET : " ");
        draw_truncated_unlocked(client, visual_line, layout->input_text_width);
        (void)send_text_unlocked(client, " " ANSI_CYAN "|" ANSI_RESET);
    }

    if (total_rows > 0) {
        int visible_cursor_row = cursor_logical_row - first_visible;

        if (visible_cursor_row < 0) {
            visible_cursor_row = 0;
        }
        if (visible_cursor_row >= layout->input_inner_rows) {
            visible_cursor_row = layout->input_inner_rows - 1;
        }
        cursor_col_in_row = clamp_int(cursor_col_in_row, 0, layout->input_text_width);
        cursor_row = layout->input_top_row + 1 + visible_cursor_row;
        cursor_col = 5 + cursor_col_in_row;
    }
    (void)send_fmt_unlocked(client, "\033[%d;%dH", cursor_row, cursor_col);
}

static void draw_sidebar_unlocked(server_state_t *state,
                                  client_t *client,
                                  const ui_layout_t *layout)
{
    client_t *cursor;
    int row = 3;
    char line[TENET_MAX_DISPLAY_NAME + 16];

    if (layout->sidebar_cols <= 0) {
        return;
    }

    draw_border_unlocked(client, 2, layout->sidebar_col, layout->sidebar_cols, " 在线用户 ");
    snprintf(line, sizeof(line), "共 %d 人", state->online_count);
    draw_box_line_unlocked(client, row++, layout->sidebar_col, layout->sidebar_cols, line);
    if (row < layout->rows) {
        draw_box_line_unlocked(client, row++, layout->sidebar_col, layout->sidebar_cols, "");
    }

    for (cursor = state->clients; cursor != NULL && row < layout->rows; cursor = cursor->next) {
        if (!cursor->in_chat) {
            continue;
        }
        snprintf(line, sizeof(line), "%s %s", cursor == client ? ">" : " ", client_name(cursor));
        draw_box_line_unlocked(client, row++, layout->sidebar_col, layout->sidebar_cols, line);
    }
    while (row < layout->rows) {
        draw_box_line_unlocked(client, row++, layout->sidebar_col, layout->sidebar_cols, "");
    }
    draw_border_unlocked(client, layout->rows, layout->sidebar_col, layout->sidebar_cols, NULL);
}

static void refresh_client_screen_locked(server_state_t *state, client_t *client)
{
    ui_layout_t layout;
    char title[160];
    char chat_title[TENET_MAX_LINE];
    const char *wrapped_lines[TENET_MAX_VISIBLE_ROWS];
    int wrapped_offsets[TENET_MAX_VISIBLE_ROWS];
    size_t count;
    int row;

    compute_layout(client, &layout);
    pthread_mutex_lock(&client->write_mutex);
    begin_buffered_write_unlocked(client);
    format_header_title(title, sizeof(title), state->online_count);
    if (client->last_render_rows != layout.rows || client->last_render_cols != layout.cols) {
        (void)send_text_unlocked(client, ANSI_SYNC_UPDATE_BEGIN "\033[?25l" ANSI_CLEAR ANSI_HOME);
    } else {
        (void)send_text_unlocked(client, ANSI_SYNC_UPDATE_BEGIN "\033[?25l" ANSI_HOME);
    }
    draw_header_bar_unlocked(client, layout.cols, title);

    format_chat_title(client, chat_title, sizeof(chat_title));
    draw_border_unlocked(client, layout.chat_top_row, 1, layout.left_cols, chat_title);
    count = collect_wrapped_history_locked(state, client, wrapped_lines, wrapped_offsets,
                                           (size_t)layout.chat_rows, layout.chat_inner_width);

    for (row = 0; row < layout.chat_rows; row++) {
        if ((size_t)row < count) {
            draw_box_line_unlocked(client, layout.chat_first_row + row, 1, layout.left_cols,
                                   skip_utf8_width(wrapped_lines[row], wrapped_offsets[row]));
        } else {
            draw_box_line_unlocked(client, layout.chat_first_row + row, 1, layout.left_cols, "");
        }
    }
    draw_border_unlocked(client, layout.chat_bottom_row, 1, layout.left_cols, NULL);

    draw_border_unlocked(client, layout.input_top_row, 1, layout.left_cols, " 输入消息 ");
    draw_border_unlocked(client, layout.input_bottom_row, 1, layout.left_cols, NULL);
    draw_sidebar_unlocked(state, client, &layout);
    draw_input_unlocked(client, &layout);
    client->last_render_rows = layout.rows;
    client->last_render_cols = layout.cols;
    client->last_input_top_row = layout.input_top_row;
    client->last_input_bottom_row = layout.input_bottom_row;
    client->last_input_inner_rows = layout.input_inner_rows;
    (void)send_text_unlocked(client, "\033[?25h" ANSI_SYNC_UPDATE_END);
    (void)end_buffered_write_unlocked(client);
    pthread_mutex_unlock(&client->write_mutex);
}

static void refresh_client_screen(server_state_t *state, client_t *client)
{
    pthread_mutex_lock(&state->mutex);
    refresh_client_screen_locked(state, client);
    pthread_mutex_unlock(&state->mutex);
}

static void refresh_client_input(server_state_t *state, client_t *client)
{
    ui_layout_t layout;
    int need_full_refresh;

    pthread_mutex_lock(&state->mutex);
    compute_layout(client, &layout);
    need_full_refresh = client->last_render_rows != layout.rows ||
                        client->last_render_cols != layout.cols ||
                        client->last_input_top_row != layout.input_top_row ||
                        client->last_input_bottom_row != layout.input_bottom_row ||
                        client->last_input_inner_rows != layout.input_inner_rows;
    if (need_full_refresh) {
        refresh_client_screen_locked(state, client);
    } else {
        pthread_mutex_lock(&client->write_mutex);
        begin_buffered_write_unlocked(client);
        (void)send_text_unlocked(client, ANSI_SYNC_UPDATE_BEGIN "\033[?25l");
        draw_input_unlocked(client, &layout);
        (void)send_text_unlocked(client, "\033[?25h" ANSI_SYNC_UPDATE_END);
        (void)end_buffered_write_unlocked(client);
        pthread_mutex_unlock(&client->write_mutex);
    }
    pthread_mutex_unlock(&state->mutex);
}

static void history_add_locked(server_state_t *state, const char *line, const char *target_username)
{
    safe_copy(state->history[state->history_next].line, TENET_HISTORY_LINE, line);
    safe_copy(state->history[state->history_next].target_username,
              sizeof(state->history[state->history_next].target_username),
              target_username != NULL ? target_username : "");
    state->history[state->history_next].private_from_username[0] = '\0';
    state->history[state->history_next].private_to_username[0] = '\0';
    state->history_next = (state->history_next + 1) % TENET_HISTORY_CAP;
    state->history_total++;
    if (state->history_count < TENET_HISTORY_CAP) {
        state->history_count++;
    }
}

static void history_add_private_locked(server_state_t *state,
                                       const char *line,
                                       const char *from_username,
                                       const char *to_username)
{
    safe_copy(state->history[state->history_next].line, TENET_HISTORY_LINE, line);
    state->history[state->history_next].target_username[0] = '\0';
    safe_copy(state->history[state->history_next].private_from_username,
              sizeof(state->history[state->history_next].private_from_username),
              from_username != NULL ? from_username : "");
    safe_copy(state->history[state->history_next].private_to_username,
              sizeof(state->history[state->history_next].private_to_username),
              to_username != NULL ? to_username : "");
    state->history_next = (state->history_next + 1) % TENET_HISTORY_CAP;
    state->history_total++;
    if (state->history_count < TENET_HISTORY_CAP) {
        state->history_count++;
    }
}

static void history_add_private_targeted_locked(server_state_t *state,
                                                const char *line,
                                                const char *target_username,
                                                const char *from_username,
                                                const char *to_username)
{
    safe_copy(state->history[state->history_next].line, TENET_HISTORY_LINE, line);
    safe_copy(state->history[state->history_next].target_username,
              sizeof(state->history[state->history_next].target_username),
              target_username != NULL ? target_username : "");
    safe_copy(state->history[state->history_next].private_from_username,
              sizeof(state->history[state->history_next].private_from_username),
              from_username != NULL ? from_username : "");
    safe_copy(state->history[state->history_next].private_to_username,
              sizeof(state->history[state->history_next].private_to_username),
              to_username != NULL ? to_username : "");
    state->history_next = (state->history_next + 1) % TENET_HISTORY_CAP;
    state->history_total++;
    if (state->history_count < TENET_HISTORY_CAP) {
        state->history_count++;
    }
}

static void append_system_message_for_client_locked(server_state_t *state, client_t *client, const char *message)
{
    char line[TENET_HISTORY_LINE];

    snprintf(line, sizeof(line), ANSI_DIM "SYSTEM:" ANSI_RESET " %s", message);
    if (client->active_peer_username[0] != '\0') {
        history_add_private_targeted_locked(state, line, client->username,
                                            client->username,
                                            client->active_peer_username);
    } else {
        history_add_locked(state, line, client->username);
    }
}

static void append_system_message_for_client(server_state_t *state, client_t *client, const char *message)
{
    pthread_mutex_lock(&state->mutex);
    append_system_message_for_client_locked(state, client, message);
    refresh_client_screen_locked(state, client);
    pthread_mutex_unlock(&state->mutex);
}

static void refresh_all_clients_locked(server_state_t *state)
{
    client_t *cursor;

    for (cursor = state->clients; cursor != NULL; cursor = cursor->next) {
        if (cursor->active && cursor->in_chat) {
            refresh_client_screen_locked(state, cursor);
        }
    }
}

static void mark_lobby_unread_locked(server_state_t *state)
{
    client_t *cursor;

    for (cursor = state->clients; cursor != NULL; cursor = cursor->next) {
        if (cursor->active && cursor->in_chat && cursor->active_peer_username[0] != '\0') {
            cursor->lobby_unread = 1;
        }
    }
}

static void mark_private_unread(client_t *client, const char *peer_username)
{
    int index = private_peer_index(client, peer_username);

    if (index < 0) {
        return;
    }
    if (ascii_equal_ignore_case(client->active_peer_username, peer_username)) {
        client->private_peer_unread[index] = 0;
    } else {
        client->private_peer_unread[index] = 1;
    }
}

void render_header(client_t *client, int online_count)
{
    int cols = clamp_int(client->screen_cols > 0 ? client->screen_cols : TENET_DEFAULT_SCREEN_COLS,
                         TENET_MIN_SCREEN_COLS, TENET_MAX_SCREEN_COLS);
    char title[160];

    format_header_title(title, sizeof(title), online_count);
    pthread_mutex_lock(&client->write_mutex);
    begin_buffered_write_unlocked(client);
    (void)send_text_unlocked(client, ANSI_CLEAR ANSI_HOME);
    draw_header_bar_unlocked(client, cols, title);
    (void)send_text_unlocked(client, "\r\n");
    (void)end_buffered_write_unlocked(client);
    pthread_mutex_unlock(&client->write_mutex);
}


static int read_badapple_u16(FILE *file, unsigned int *value)
{
    unsigned char bytes[2];

    if (fread(bytes, 1, sizeof(bytes), file) != sizeof(bytes)) {
        return -1;
    }
    *value = (unsigned int)bytes[0] | ((unsigned int)bytes[1] << 8);
    return 0;
}

static FILE *open_badapple_asset(void)
{
    FILE *file = fopen(TENET_BADAPPLE_SYSTEM_PATH, "rb");

    if (file == NULL) {
        file = fopen(TENET_BADAPPLE_LOCAL_PATH, "rb");
    }
    return file;
}

static int badapple_open(badapple_movie_t *movie)
{
    char magic[4];
    unsigned int rows;
    unsigned int cols;
    unsigned int frame_count;

    memset(movie, 0, sizeof(*movie));
    movie->file = open_badapple_asset();
    if (movie->file == NULL) {
        return -1;
    }
    if (fread(magic, 1, sizeof(magic), movie->file) != sizeof(magic) ||
        memcmp(magic, "TBA2", sizeof(magic)) != 0 ||
        read_badapple_u16(movie->file, &rows) != 0 ||
        read_badapple_u16(movie->file, &cols) != 0 ||
        read_badapple_u16(movie->file, &frame_count) != 0 ||
        rows == 0 || cols == 0 || frame_count == 0 ||
        rows > 200 || cols > 300) {
        fclose(movie->file);
        memset(movie, 0, sizeof(*movie));
        return -1;
    }

    movie->rows = rows;
    movie->cols = cols;
    movie->frame_count = frame_count;
    movie->data_offset = ftell(movie->file);
    movie->pixels = calloc((size_t)rows * (size_t)cols, sizeof(movie->pixels[0]));
    if (movie->pixels == NULL || movie->data_offset < 0) {
        free(movie->pixels);
        fclose(movie->file);
        memset(movie, 0, sizeof(*movie));
        return -1;
    }
    return 0;
}

static void badapple_close(badapple_movie_t *movie)
{
    if (movie->file != NULL) {
        fclose(movie->file);
    }
    free(movie->pixels);
    memset(movie, 0, sizeof(*movie));
}

static int badapple_rewind(badapple_movie_t *movie)
{
    if (movie->file == NULL || fseek(movie->file, movie->data_offset, SEEK_SET) != 0) {
        return -1;
    }
    memset(movie->pixels, 0, (size_t)movie->rows * (size_t)movie->cols);
    movie->frame_index = 0;
    return 0;
}

static int badapple_next_frame(badapple_movie_t *movie)
{
    unsigned int segment_count;
    unsigned int segment;
    size_t cursor = 0;
    size_t pixel_count = (size_t)movie->rows * (size_t)movie->cols;

    if (movie->frame_index >= movie->frame_count) {
        return 1;
    }
    if (read_badapple_u16(movie->file, &segment_count) != 0) {
        return -1;
    }
    for (segment = 0; segment < segment_count; segment++) {
        unsigned int skip;
        unsigned int length;
        size_t index;

        if (read_badapple_u16(movie->file, &skip) != 0 ||
            read_badapple_u16(movie->file, &length) != 0) {
            return -1;
        }
        cursor += skip;
        if (cursor + length > pixel_count) {
            return -1;
        }
        for (index = 0; index < length; index++) {
            movie->pixels[cursor + index] = movie->pixels[cursor + index] ? 0 : 1;
        }
        cursor += length;
    }
    movie->frame_index++;
    return 0;
}

static size_t badapple_visible_pixel_count(const badapple_movie_t *movie)
{
    size_t pixel_count = (size_t)movie->rows * (size_t)movie->cols;
    size_t visible = 0;
    size_t index;

    for (index = 0; index < pixel_count; index++) {
        if (movie->pixels[index]) {
            visible++;
        }
    }
    return visible;
}

static int badapple_skip_leading_blank_frames(badapple_movie_t *movie)
{
    unsigned int attempts;

    for (attempts = 0; attempts < movie->frame_count; attempts++) {
        int rc;

        if (badapple_visible_pixel_count(movie) > 0) {
            return 0;
        }
        rc = badapple_next_frame(movie);
        if (rc != 0) {
            return -1;
        }
    }
    return badapple_visible_pixel_count(movie) > 0 ? 0 : -1;
}

static void draw_centered_unlocked(client_t *client, int row, int width, const char *text)
{
    int text_width = display_width(text);
    int col = (width - text_width) / 2 + 1;

    if (col < 1) {
        col = 1;
    }
    (void)send_fmt_unlocked(client, "\033[%d;%dH", row, col);
    draw_truncated_unlocked(client, text, width - col + 1);
}

static void draw_badapple_pixels_unlocked(client_t *client,
                                          const badapple_movie_t *movie,
                                          int top_row,
                                          int area_rows,
                                          int area_cols)
{
    int draw_rows;
    int draw_cols;
    int start_row;
    int start_col;
    int y;

    if (movie == NULL || movie->pixels == NULL || area_rows <= 0 || area_cols <= 0) {
        return;
    }

    draw_rows = area_rows < (int)movie->rows ? area_rows : (int)movie->rows;
    draw_cols = area_cols < (int)movie->cols ? area_cols : (int)movie->cols;
    if (draw_rows < 1 || draw_cols < 1) {
        return;
    }
    start_row = top_row + (area_rows - draw_rows) / 2;
    start_col = 1 + (area_cols - draw_cols) / 2;

    (void)send_text_unlocked(client, ANSI_WHITE ANSI_BOLD);
    for (y = 0; y < draw_rows; y++) {
        char line[TENET_MAX_SCREEN_COLS + 1];
        int source_y = y * (int)movie->rows / draw_rows;
        int x;

        for (x = 0; x < draw_cols; x++) {
            int source_x = x * (int)movie->cols / draw_cols;
            line[x] = movie->pixels[(size_t)source_y * movie->cols + (size_t)source_x] ? '#' : ' ';
        }
        line[draw_cols] = '\0';
        (void)send_fmt_unlocked(client, "\033[%d;%dH", start_row + y, start_col);
        (void)send_bytes_unlocked(client, line, (size_t)draw_cols);
    }
    (void)send_text_unlocked(client, ANSI_RESET);
}

static void render_entry_screen_unlocked(client_t *client, const badapple_movie_t *movie, int online_count)
{
    int rows = clamp_int(client->screen_rows > 0 ? client->screen_rows : TENET_DEFAULT_SCREEN_ROWS,
                         TENET_MIN_SCREEN_ROWS, TENET_MAX_SCREEN_ROWS);
    int cols = clamp_int(client->screen_cols > 0 ? client->screen_cols : TENET_DEFAULT_SCREEN_COLS,
                         TENET_MIN_SCREEN_COLS, TENET_MAX_SCREEN_COLS);
    int info_top = rows - 3;
    int animation_rows = info_top - 3;
    char title[128];
    char name_line[TENET_MAX_DISPLAY_NAME + 64];
    char id_line[TENET_MAX_USERNAME + 64];

    if (animation_rows < 1) {
        animation_rows = 1;
    }
    format_header_title(title, sizeof(title), online_count);
    snprintf(name_line, sizeof(name_line), ANSI_BOLD "姓名:" ANSI_RESET " %s", client_name(client));
    snprintf(id_line, sizeof(id_line), ANSI_BOLD "ID:" ANSI_RESET " %s", client->username);

    begin_buffered_write_unlocked(client);
    if (client->last_render_rows != rows || client->last_render_cols != cols) {
        (void)send_text_unlocked(client, ANSI_SYNC_UPDATE_BEGIN "\033[?25l" ANSI_CLEAR ANSI_HOME);
    } else {
        (void)send_text_unlocked(client, ANSI_SYNC_UPDATE_BEGIN "\033[?25l" ANSI_HOME);
    }
    draw_header_bar_unlocked(client, cols, title);
    if (movie != NULL && movie->pixels != NULL) {
        draw_badapple_pixels_unlocked(client, movie, 2, animation_rows, cols);
    } else {
        draw_centered_unlocked(client, 2 + animation_rows / 2, cols,
                               ANSI_DIM "Bad Apple 动画资源未找到" ANSI_RESET);
    }

    draw_centered_unlocked(client, info_top, cols, name_line);
    draw_centered_unlocked(client, info_top + 1, cols, id_line);
    draw_centered_unlocked(client, info_top + 2, cols,
                           ANSI_GREEN ANSI_BOLD "回车进入比友聊天室" ANSI_RESET);
    client->last_render_rows = rows;
    client->last_render_cols = cols;
    (void)send_text_unlocked(client, "\033[?25h" ANSI_SYNC_UPDATE_END);
    (void)end_buffered_write_unlocked(client);
}

static int handle_entry_screen_input(client_t *client, unsigned char byte)
{
    int rc;

    if (byte == TELNET_IAC) {
        rc = recv_byte(client, &byte);
        if (rc <= 0) {
            return -1;
        }
        return skip_telnet_command(client, byte) < 0 ? -1 : 1;
    }
    if (byte == TENET_CONTROL_BYTE) {
        return read_control_sequence(client) <= 0 ? -1 : 1;
    }
    if (client->skip_cr_tail) {
        client->skip_cr_tail = 0;
        if (byte == '\n' || byte == '\0') {
            return 1;
        }
    }
    if (byte == '\r') {
        client->skip_cr_tail = 1;
        return 0;
    }
    if (byte == '\n') {
        return 0;
    }
    if (byte == 3 || byte == 4) {
        return -1;
    }
    return 1;
}

static void enable_keyboard_protocol(client_t *client)
{
    (void)send_text(client, TENET_KEYBOARD_PROTOCOL_ENABLE TENET_MOUSE_PROTOCOL_ENABLE);
}

static void disable_keyboard_protocol(client_t *client)
{
    (void)send_text(client, TENET_MOUSE_PROTOCOL_DISABLE TENET_KEYBOARD_PROTOCOL_DISABLE);
}

static int show_entry_screen(client_t *client)
{
    badapple_movie_t movie;
    int has_movie = badapple_open(&movie) == 0;
    int frame_ready = 0;
    int result = 0;

    if (has_movie) {
        if (badapple_skip_leading_blank_frames(&movie) == 0) {
            frame_ready = 1;
        } else {
            badapple_close(&movie);
            has_movie = 0;
        }
    }
    enable_keyboard_protocol(client);

    for (;;) {
        fd_set read_set;
        struct timeval timeout;
        int ready;

        if (has_movie && !frame_ready) {
            int frame_rc = badapple_next_frame(&movie);

            if (frame_rc == 0) {
                frame_ready = 1;
            } else if (frame_rc > 0) {
                if (badapple_rewind(&movie) != 0 ||
                    badapple_skip_leading_blank_frames(&movie) != 0) {
                    badapple_close(&movie);
                    has_movie = 0;
                } else {
                    frame_ready = 1;
                }
            } else {
                badapple_close(&movie);
                has_movie = 0;
            }
        }

        int online_count = 0;

        if (global_state != NULL) {
            pthread_mutex_lock(&global_state->mutex);
            online_count = global_state->online_count;
            pthread_mutex_unlock(&global_state->mutex);
        }
        pthread_mutex_lock(&client->write_mutex);
        render_entry_screen_unlocked(client, has_movie ? &movie : NULL, online_count);
        pthread_mutex_unlock(&client->write_mutex);
        frame_ready = 0;


        FD_ZERO(&read_set);
        FD_SET(client->fd, &read_set);
        timeout.tv_sec = 0;
        timeout.tv_usec = TENET_BADAPPLE_FRAME_USEC;
        ready = select(client->fd + 1, &read_set, NULL, NULL, &timeout);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            result = -1;
            break;
        }
        if (ready == 0) {
            continue;
        }
        if (FD_ISSET(client->fd, &read_set)) {
            unsigned char byte;
            int input_result;

            if (recv_byte(client, &byte) <= 0) {
                result = -1;
                break;
            }
            input_result = handle_entry_screen_input(client, byte);
            if (input_result == 0) {
                break;
            }
            if (input_result < 0) {
                result = -1;
                break;
            }
        }
    }

    if (movie.file != NULL) {
        badapple_close(&movie);
    }
    if (result < 0) {
        disable_keyboard_protocol(client);
    }
    (void)send_text(client, "\033[?25h");
    return result;
}

static void send_help(server_state_t *state, client_t *client)
{
    append_system_message_for_client(state, client,
                                     "命令: /help 帮助 · /who 在线用户 · /pm 用户名 私聊 · /close 关闭私聊 · /me 动作 · /quit 退出；可点击在线用户私聊");
}

static void add_multiline_message_locked(server_state_t *state,
                                         const char *timebuf,
                                         const char *name,
                                         const char *message,
                                         int action)
{
    const char *start = message;
    int first = 1;

    do {
        const char *end = strchr(start, '\n');
        size_t len = end != NULL ? (size_t)(end - start) : strlen(start);
        char part[TENET_MAX_LINE];
        char line[TENET_HISTORY_LINE];

        if (len >= sizeof(part)) {
            len = sizeof(part) - 1;
        }
        memcpy(part, start, len);
        part[len] = '\0';

        if (action) {
            if (first) {
                snprintf(line, sizeof(line), ANSI_DIM "[%s]" ANSI_RESET " " ANSI_MAGENTA "* %s %s" ANSI_RESET,
                         timebuf, name, part);
            } else {
                snprintf(line, sizeof(line), ANSI_DIM "      │" ANSI_RESET " " ANSI_MAGENTA "%s" ANSI_RESET,
                         part);
            }
        } else {
            if (first) {
                snprintf(line, sizeof(line), ANSI_DIM "[%s]" ANSI_RESET " " ANSI_CYAN "%s" ANSI_RESET ": %s",
                         timebuf, name, part);
            } else {
                snprintf(line, sizeof(line), ANSI_DIM "      │" ANSI_RESET " %s", part);
            }
        }
        history_add_locked(state, line, NULL);
        first = 0;

        if (end == NULL) {
            break;
        }
        start = end + 1;
    } while (*start != '\0' || start[-1] == '\n');
}

static void add_private_multiline_message_locked(server_state_t *state,
                                                 const char *timebuf,
                                                 const client_t *sender,
                                                 const client_t *recipient,
                                                 const char *message)
{
    const char *start = message;
    int first = 1;

    do {
        const char *end = strchr(start, '\n');
        size_t len = end != NULL ? (size_t)(end - start) : strlen(start);
        char part[TENET_MAX_LINE];
        char line[TENET_HISTORY_LINE];

        if (len >= sizeof(part)) {
            len = sizeof(part) - 1;
        }
        memcpy(part, start, len);
        part[len] = '\0';

        if (first) {
            snprintf(line, sizeof(line), ANSI_DIM "[%s]" ANSI_RESET " " ANSI_MAGENTA "%s" ANSI_RESET ": %s",
                     timebuf, client_name(sender), part);
        } else {
            snprintf(line, sizeof(line), ANSI_DIM "      │" ANSI_RESET " %s", part);
        }
        history_add_private_locked(state, line, sender->username, recipient->username);
        first = 0;

        if (end == NULL) {
            break;
        }
        start = end + 1;
    } while (*start != '\0' || start[-1] == '\n');
}

static client_t *find_client_by_username_locked(server_state_t *state, const char *username)
{
    client_t *cursor;

    for (cursor = state->clients; cursor != NULL; cursor = cursor->next) {
        if (cursor->active && cursor->in_chat &&
            ascii_equal_ignore_case(cursor->username, username)) {
            return cursor;
        }
    }
    return NULL;
}

static void broadcast_message(server_state_t *state,
                              client_t *sender,
                              const char *kind,
                              const char *message,
                              int include_sender)
{
    char timebuf[16];
    char line[TENET_HISTORY_LINE];

    now_string(timebuf, sizeof(timebuf));
    pthread_mutex_lock(&state->mutex);
    (void)include_sender;
    if (sender != NULL && strcmp(kind, "chat") == 0) {
        add_multiline_message_locked(state, timebuf, client_name(sender), message, 0);
    } else if (sender != NULL && strcmp(kind, "me") == 0) {
        add_multiline_message_locked(state, timebuf, client_name(sender), message, 1);
    } else {
        snprintf(line, sizeof(line), ANSI_DIM "[%s]" ANSI_RESET " " ANSI_YELLOW "SYSTEM:" ANSI_RESET " %s",
                 timebuf, message);
        history_add_locked(state, line, NULL);
    }
    mark_lobby_unread_locked(state);
    refresh_all_clients_locked(state);
    pthread_mutex_unlock(&state->mutex);
}

static void send_private_message(server_state_t *state, client_t *sender, const char *message)
{
    char timebuf[16];
    client_t *recipient;

    if (sender->active_peer_username[0] == '\0') {
        append_system_message_for_client(state, sender, ANSI_RED "请先使用 /pm 用户名 打开私聊。" ANSI_RESET);
        return;
    }

    now_string(timebuf, sizeof(timebuf));
    pthread_mutex_lock(&state->mutex);
    recipient = find_client_by_username_locked(state, sender->active_peer_username);
    if (recipient == NULL) {
        append_system_message_for_client_locked(state, sender, ANSI_RED "对方不在线，无法发送私聊。" ANSI_RESET);
        refresh_client_screen_locked(state, sender);
        pthread_mutex_unlock(&state->mutex);
        return;
    }

    (void)add_private_peer(sender, recipient->username);
    (void)add_private_peer(recipient, sender->username);
    add_private_multiline_message_locked(state, timebuf, sender, recipient, message);
    sender->history_scroll_rows = 0;
    mark_private_unread(sender, recipient->username);
    mark_private_unread(recipient, sender->username);
    if (history_entry_matches_active_private(&state->history[(state->history_next + TENET_HISTORY_CAP - 1) % TENET_HISTORY_CAP], recipient)) {
        recipient->history_scroll_rows = 0;
    }
    refresh_client_screen_locked(state, sender);
    if (recipient != sender) {
        refresh_client_screen_locked(state, recipient);
    }
    pthread_mutex_unlock(&state->mutex);
}

static void send_who(server_state_t *state, client_t *client)
{
    client_t *cursor;
    char line[TENET_HISTORY_LINE];
    size_t used;
    int first = 1;
    int online_count = 0;

    pthread_mutex_lock(&state->mutex);
    for (cursor = state->clients; cursor != NULL; cursor = cursor->next) {
        if (cursor->in_chat) {
            online_count++;
        }
    }
    snprintf(line, sizeof(line), "在线用户 (%d): ", online_count);
    used = strlen(line);
    for (cursor = state->clients; cursor != NULL; cursor = cursor->next) {
        int written;

        if (!cursor->in_chat) {
            continue;
        }
        written = snprintf(line + used, sizeof(line) - used, "%s%s",
                           first ? "" : ", ",
                           client_name(cursor));
        if (written < 0 || (size_t)written >= sizeof(line) - used) {
            break;
        }
        used += (size_t)written;
        first = 0;
    }
    append_system_message_for_client_locked(state, client, line);
    refresh_client_screen_locked(state, client);
    pthread_mutex_unlock(&state->mutex);
}

static void handle_command(server_state_t *state, client_t *client, char *line, int *running)
{
    if (strcmp(line, "/help") == 0) {
        send_help(state, client);
    } else if (strcmp(line, "/who") == 0) {
        send_who(state, client);
    } else if (strncmp(line, "/pm ", 4) == 0) {
        char *username = line + 4;
        client_t *recipient;

        trim_line(username);
        if (!valid_username(username)) {
            append_system_message_for_client(state, client, ANSI_RED "用法: /pm 用户名" ANSI_RESET);
            return;
        }
        if (ascii_equal_ignore_case(username, client->username)) {
            append_system_message_for_client(state, client, ANSI_RED "不能和自己私聊。" ANSI_RESET);
            return;
        }
        pthread_mutex_lock(&state->mutex);
        recipient = find_client_by_username_locked(state, username);
        if (recipient == NULL) {
            append_system_message_for_client_locked(state, client, ANSI_RED "用户不在线。" ANSI_RESET);
            refresh_client_screen_locked(state, client);
            pthread_mutex_unlock(&state->mutex);
            return;
        }
        if (switch_to_private_peer(client, recipient->username) != 0) {
            append_system_message_for_client_locked(state, client, ANSI_RED "私聊标签已达上限，请先 /close 一个私聊。" ANSI_RESET);
        } else {
            snprintf(client->status_line, sizeof(client->status_line), "正在和 %s 私聊。输入 /close 关闭此私聊。", client_name(recipient));
        }
        refresh_client_screen_locked(state, client);
        pthread_mutex_unlock(&state->mutex);
    } else if (strcmp(line, "/close") == 0) {
        if (client->active_peer_username[0] == '\0') {
            append_system_message_for_client(state, client, ANSI_RED "大厅不能关闭。" ANSI_RESET);
            return;
        }
        pthread_mutex_lock(&state->mutex);
        (void)remove_active_private_peer(client);
        safe_copy(client->status_line, sizeof(client->status_line), "已关闭私聊，回到大厅。");
        refresh_client_screen_locked(state, client);
        pthread_mutex_unlock(&state->mutex);
    } else if (strcmp(line, "/quit") == 0 || strcmp(line, "/exit") == 0) {
        *running = 0;
    } else if (strncmp(line, "/me ", 4) == 0) {
        char *message = line + 4;
        trim_line(message);
        if (*message != '\0') {
            if (client->active_peer_username[0] != '\0') {
                append_system_message_for_client(state, client, ANSI_RED "私聊中暂不支持 /me。" ANSI_RESET);
            } else {
                broadcast_message(state, client, "me", message, 1);
            }
        }
    } else {
        append_system_message_for_client(state, client, ANSI_RED "未知命令，输入 /help 查看帮助。" ANSI_RESET);
    }
}

static void *client_thread(void *arg)
{
    client_t *client = arg;
    server_state_t *state = global_state;
    char line[TENET_MAX_LINE];
    char notice[TENET_MAX_DISPLAY_NAME + 64];
    int running = 1;

    pthread_detach(pthread_self());

    if (state->config->transport == TENET_TRANSPORT_TELNET) {
        send_telnet_setup(client->fd);
        if (authenticate_telnet_client(state, client) != 0) {
            close(client->fd);
            destroy_client(client);
            return NULL;
        }
        request_server_echo(client);
    } else {
        char hello[32];
        char ssh_user[TENET_MAX_USERNAME];
        char display_name[TENET_MAX_DISPLAY_NAME];
        int protocol_version;

        if (read_plain_line_fd(client->fd, hello, sizeof(hello)) <= 0 ||
            (strcmp(hello, TENET_SESSION_HELLO_V1) != 0 &&
             strcmp(hello, TENET_SESSION_HELLO_V2) != 0 &&
             strcmp(hello, TENET_SESSION_HELLO_V3) != 0) ||
            read_plain_line_fd(client->fd, ssh_user, sizeof(ssh_user)) <= 0 ||
            !valid_username(ssh_user)) {
            close(client->fd);
            destroy_client(client);
            return NULL;
        }
        if (strcmp(hello, TENET_SESSION_HELLO_V3) == 0) {
            protocol_version = 3;
        } else if (strcmp(hello, TENET_SESSION_HELLO_V2) == 0) {
            protocol_version = 2;
        } else {
            protocol_version = 1;
        }
        safe_copy(client->username, sizeof(client->username),
                  ssh_user[0] != '\0' ? ssh_user : "ssh-user");

        display_name[0] = '\0';
        if (protocol_version >= 2) {
            if (read_plain_line_fd(client->fd, display_name, sizeof(display_name)) <= 0) {
                close(client->fd);
                destroy_client(client);
                return NULL;
            }
            trim_line(display_name);
        }
        safe_copy(client->display_name, sizeof(client->display_name),
                  valid_display_name(display_name) ? display_name : client->username);
        if (protocol_version >= 3) {
            char window_size[64];
            int rows;
            int cols;
            if (read_plain_line_fd(client->fd, window_size, sizeof(window_size)) <= 0) {
                close(client->fd);
                destroy_client(client);
                return NULL;
            }
            if (parse_window_size(window_size, &rows, &cols) == 0) {
                client->screen_rows = rows;
                client->screen_cols = cols;
            }
        }
        if (authenticate_ssh_client(state, client) != 0) {
            close(client->fd);
            destroy_client(client);
            return NULL;
        }
    }

    if (add_client_if_username_free(state, client) != 0) {
        (void)send_text(client, ANSI_RED "该用户已在线，不能重复加入。\r\n" ANSI_RESET);
        close(client->fd);
        destroy_client(client);
        return NULL;
    }
    if (show_entry_screen(client) != 0) {
        remove_client(state, client);
        close(client->fd);
        destroy_client(client);
        return NULL;
    }
    mark_client_in_chat(state, client);
    snprintf(client->status_line, sizeof(client->status_line),
             "登录成功。欢迎 %s！输入 /help 查看命令。", client_name(client));
    refresh_client_screen(state, client);

    snprintf(notice, sizeof(notice), "%s 加入了聊天室。", client_name(client));
    broadcast_message(state, NULL, "system", notice, 0);

    while (running && !stop_requested) {
        if (read_line(client, line, sizeof(line), 0) <= 0) {
            break;
        }
        trim_line(line);
        if (line[0] == '\0') {
            continue;
        }
        if (line[0] == '/') {
            handle_command(state, client, line, &running);
        } else if (client->active_peer_username[0] != '\0') {
            send_private_message(state, client, line);
        } else {
            broadcast_message(state, client, "chat", line, 1);
        }
    }

    snprintf(notice, sizeof(notice), "%s 离开了聊天室。", client_name(client));
    disable_keyboard_protocol(client);
    remove_client(state, client);
    broadcast_message(state, NULL, "system", notice, 1);
    close(client->fd);
    destroy_client(client);
    return NULL;
}

static int create_tcp_listener(const tenet_config_t *config)
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
    hints.ai_flags = AI_PASSIVE;
    snprintf(portbuf, sizeof(portbuf), "%d", config->port);

    gai = getaddrinfo(config->bind_addr, portbuf, &hints, &result);
    if (gai != 0) {
        fprintf(stderr, "监听地址解析失败: %s\n", gai_strerror(gai));
        return -1;
    }

    for (cursor = result; cursor != NULL; cursor = cursor->ai_next) {
        int opt = 1;
        fd = socket(cursor->ai_family, cursor->ai_socktype, cursor->ai_protocol);
        if (fd < 0) {
            continue;
        }
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        if (bind(fd, cursor->ai_addr, cursor->ai_addrlen) == 0 &&
            listen(fd, config->max_clients) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }

    freeaddrinfo(result);
    return fd;
}

static int create_unix_listener(const tenet_config_t *config)
{
    struct sockaddr_un addr;
    int fd;

    if (strlen(config->socket_path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "Unix socket 路径过长: %s\n", config->socket_path);
        return -1;
    }

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", config->socket_path);
    (void)unlink(config->socket_path);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(fd, config->max_clients) != 0) {
        close(fd);
        return -1;
    }
    (void)chmod(config->socket_path, 0666);
    return fd;
}

int tenet_server_run(const tenet_config_t *config)
{
    server_state_t state;

    memset(&state, 0, sizeof(state));
    state.config = config;
    state.listen_fd = config->transport == TENET_TRANSPORT_TELNET ?
                      create_tcp_listener(config) : create_unix_listener(config);
    if (state.listen_fd < 0) {
        perror("无法监听");
        return -1;
    }
    if (pthread_mutex_init(&state.mutex, NULL) != 0) {
        perror("pthread_mutex_init");
        close(state.listen_fd);
        return -1;
    }

    global_state = &state;
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    if (config->transport == TENET_TRANSPORT_TELNET) {
        printf("tenet telnet listening on %s:%d\n", config->bind_addr, config->port);
    } else {
        printf("tenet ssh backend listening on %s\n", config->socket_path);
        printf("Use OpenSSH ForceCommand: tenet --ssh-session --socket %s\n", config->socket_path);
    }
    printf("LDAP %s (%s:%d, BaseDN=%s)\n",
           config->ldap_enabled ? "enabled" : "disabled",
           config->ldap_host, config->ldap_port, config->ldap_base_dn);

    while (!stop_requested) {
        client_t *client = calloc(1, sizeof(*client));
        if (client == NULL) {
            perror("calloc");
            break;
        }
        client->addr_len = sizeof(client->addr);
        if (pthread_mutex_init(&client->write_mutex, NULL) != 0) {
            perror("pthread_mutex_init");
            free(client);
            continue;
        }
        client->fd = accept(state.listen_fd, (struct sockaddr *)&client->addr, &client->addr_len);
        if (client->fd < 0) {
            destroy_client(client);
            if (errno == EINTR) {
                continue;
            }
            if (stop_requested) {
                break;
            }
            perror("accept");
            continue;
        }

        pthread_mutex_lock(&state.mutex);
        if (state.online_count >= config->max_clients) {
            pthread_mutex_unlock(&state.mutex);
            (void)send_all(client->fd, "聊天室已满，请稍后再试。\r\n", strlen("聊天室已满，请稍后再试。\r\n"));
            close(client->fd);
            destroy_client(client);
            continue;
        }
        pthread_mutex_unlock(&state.mutex);

        if (pthread_create(&client->thread, NULL, client_thread, client) != 0) {
            perror("pthread_create");
            close(client->fd);
            destroy_client(client);
        }
    }

    close(state.listen_fd);
    if (config->transport == TENET_TRANSPORT_SSH) {
        (void)unlink(config->socket_path);
    }
    pthread_mutex_destroy(&state.mutex);
    printf("tenet stopped\n");
    return 0;
}
