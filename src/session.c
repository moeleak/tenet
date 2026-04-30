#include "tenet.h"

#include <ctype.h>
#include <errno.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/termios.h>
#include <sys/un.h>
#include <unistd.h>

#define TENET_SESSION_HELLO "TENET/3\n"
#define TENET_CONTROL_BYTE 0x1d
#define TENET_DEFAULT_SCREEN_ROWS 24
#define TENET_DEFAULT_SCREEN_COLS 80
#define TENET_RELAY_BUFFER_SIZE 65536

typedef struct terminal_state {
    int enabled;
    struct termios old_termios;
} terminal_state_t;

static volatile sig_atomic_t resize_pending;

static int connect_backend(const char *path)
{
    struct sockaddr_un addr;
    int fd;

    if (strlen(path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "tenet: socket 路径过长: %s\n", path);
        return -1;
    }

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("tenet: socket");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("tenet: connect backend");
        close(fd);
        return -1;
    }
    return fd;
}

static int write_all(int fd, const void *data, size_t len)
{
    const char *bytes = data;
    size_t sent = 0;

    while (sent < len) {
        ssize_t rc = write(fd, bytes + sent, len - sent);
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

static void copy_string(char *dest, size_t size, const char *src)
{
    if (size == 0) {
        return;
    }
    snprintf(dest, size, "%s", src != NULL ? src : "");
}

static void trim_ascii_spaces(char *text)
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

static void copy_gecos_name(char *dest, size_t size, const char *src)
{
    size_t used = 0;

    if (size == 0) {
        return;
    }
    dest[0] = '\0';
    if (src == NULL) {
        return;
    }

    while (*src != '\0' && *src != ',' && used + 1 < size) {
        unsigned char ch = (unsigned char)*src++;
        if (ch < 0x20 || ch == 0x7f) {
            if (used > 0 && dest[used - 1] != ' ') {
                dest[used++] = ' ';
            }
            continue;
        }
        dest[used++] = (char)ch;
    }
    dest[used] = '\0';
    trim_ascii_spaces(dest);
}

static void lookup_display_name(const char *user, char *display_name, size_t size)
{
    struct passwd *pw;

    if (size == 0) {
        return;
    }
    display_name[0] = '\0';

    pw = getpwuid(getuid());
    if (pw != NULL) {
        copy_gecos_name(display_name, size, pw->pw_gecos);
    }

    if (display_name[0] == '\0' && user != NULL && *user != '\0') {
        pw = getpwnam(user);
        if (pw != NULL) {
            copy_gecos_name(display_name, size, pw->pw_gecos);
        }
    }

    if (display_name[0] == '\0') {
        copy_string(display_name, size, user != NULL && *user != '\0' ? user : "ssh-user");
    }
}

static void get_terminal_size(int *rows, int *cols)
{
    struct winsize size;

    *rows = TENET_DEFAULT_SCREEN_ROWS;
    *cols = TENET_DEFAULT_SCREEN_COLS;
    memset(&size, 0, sizeof(size));
    if (isatty(STDIN_FILENO) && ioctl(STDIN_FILENO, TIOCGWINSZ, &size) == 0) {
        if (size.ws_row > 0) {
            *rows = size.ws_row;
        }
        if (size.ws_col > 0) {
            *cols = size.ws_col;
        }
    }
}

static void send_window_size(int backend_fd)
{
    int rows;
    int cols;
    char line[64];

    get_terminal_size(&rows, &cols);
    snprintf(line, sizeof(line), "%cW%d;%d\n", TENET_CONTROL_BYTE, rows, cols);
    (void)write_all(backend_fd, line, strlen(line));
}

static void handle_resize_signal(int signum)
{
    (void)signum;
    resize_pending = 1;
}

static void setup_terminal(terminal_state_t *state)
{
    struct termios raw;

    memset(state, 0, sizeof(*state));
    if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &state->old_termios) != 0) {
        return;
    }
    raw = state->old_termios;
    raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_iflag &= (tcflag_t)~(IXON | ICRNL | INLCR);
    raw.c_oflag &= (tcflag_t)~(OPOST);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0) {
        state->enabled = 1;
    }
}

static void restore_terminal(terminal_state_t *state)
{
    if (state->enabled) {
        (void)tcsetattr(STDIN_FILENO, TCSANOW, &state->old_termios);
    }
}

static void send_hello(int backend_fd)
{
    const char *user = getenv("USER");
    struct passwd *pw;
    char line[TENET_MAX_DISPLAY_NAME + 32];
    char display_name[TENET_MAX_DISPLAY_NAME];
    int rows;
    int cols;

    if (user == NULL || *user == '\0') {
        user = getenv("LOGNAME");
    }
    if (user == NULL || *user == '\0') {
        pw = getpwuid(getuid());
        if (pw != NULL && pw->pw_name != NULL && pw->pw_name[0] != '\0') {
            user = pw->pw_name;
        }
    }
    if (user == NULL || *user == '\0') {
        user = "ssh-user";
    }
    lookup_display_name(user, display_name, sizeof(display_name));
    get_terminal_size(&rows, &cols);

    (void)write_all(backend_fd, TENET_SESSION_HELLO, strlen(TENET_SESSION_HELLO));
    snprintf(line, sizeof(line), "%s\n", user);
    (void)write_all(backend_fd, line, strlen(line));
    snprintf(line, sizeof(line), "%s\n", display_name);
    (void)write_all(backend_fd, line, strlen(line));
    snprintf(line, sizeof(line), "%d %d\n", rows, cols);
    (void)write_all(backend_fd, line, strlen(line));
}

static int copy_loop(int backend_fd)
{
    int stdin_open = 1;

    for (;;) {
        fd_set read_set;
        int max_fd = backend_fd;

        if (resize_pending) {
            resize_pending = 0;
            send_window_size(backend_fd);
        }

        FD_ZERO(&read_set);
        FD_SET(backend_fd, &read_set);
        if (stdin_open) {
            FD_SET(STDIN_FILENO, &read_set);
            if (STDIN_FILENO > max_fd) {
                max_fd = STDIN_FILENO;
            }
        }

        if (select(max_fd + 1, &read_set, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("tenet: select");
            return -1;
        }

        if (FD_ISSET(backend_fd, &read_set)) {
            char buffer[TENET_RELAY_BUFFER_SIZE];
            ssize_t got = read(backend_fd, buffer, sizeof(buffer));
            if (got < 0) {
                if (errno == EINTR) {
                    continue;
                }
                perror("tenet: read backend");
                return -1;
            }
            if (got == 0) {
                return 0;
            }
            if (write_all(STDOUT_FILENO, buffer, (size_t)got) != 0) {
                return -1;
            }
        }

        if (stdin_open && FD_ISSET(STDIN_FILENO, &read_set)) {
            char buffer[4096];
            ssize_t got = read(STDIN_FILENO, buffer, sizeof(buffer));
            if (got < 0) {
                if (errno == EINTR) {
                    continue;
                }
                perror("tenet: read stdin");
                return -1;
            }
            if (got == 0) {
                stdin_open = 0;
                (void)shutdown(backend_fd, SHUT_WR);
                continue;
            }
            if (write_all(backend_fd, buffer, (size_t)got) != 0) {
                return -1;
            }
        }
    }
}

int tenet_session_run(const tenet_config_t *config)
{
    terminal_state_t terminal;
    struct sigaction old_winch;
    struct sigaction winch_action;
    int fd = connect_backend(config->socket_path);
    int rc;

    if (fd < 0) {
        return -1;
    }
    setup_terminal(&terminal);
    memset(&winch_action, 0, sizeof(winch_action));
    winch_action.sa_handler = handle_resize_signal;
    sigemptyset(&winch_action.sa_mask);
    (void)sigaction(SIGWINCH, &winch_action, &old_winch);
    send_hello(fd);
    rc = copy_loop(fd);
    (void)sigaction(SIGWINCH, &old_winch, NULL);
    restore_terminal(&terminal);
    close(fd);
    return rc;
}
