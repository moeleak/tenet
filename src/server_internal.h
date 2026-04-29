#ifndef TENET_SERVER_INTERNAL_H
#define TENET_SERVER_INTERNAL_H

#include "tenet.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TENET_SESSION_HELLO_V1 "TENET/1"
#define TENET_SESSION_HELLO_V2 "TENET/2"
#define TENET_SESSION_HELLO_V3 "TENET/3"
#define TENET_BOT_HELLO_V1 "TENET/BOT/1"
#define TENET_CONTROL_BYTE 0x1d

#define TELNET_IAC 255
#define TELNET_DONT 254
#define TELNET_DO 253
#define TELNET_WONT 252
#define TELNET_WILL 251
#define TELNET_SB 250
#define TELNET_SE 240
#define TELNET_ECHO 1
#define TELNET_SUPPRESS_GO_AHEAD 3
#define TELNET_LINEMODE 34

#define ANSI_CLEAR "\033[2J"
#define ANSI_HOME "\033[H"
#define ANSI_RESET "\033[0m"
#define ANSI_BOLD "\033[1m"
#define ANSI_DIM "\033[2m"
#define ANSI_GREEN "\033[32m"
#define ANSI_CYAN "\033[36m"
#define ANSI_MAGENTA "\033[35m"
#define ANSI_YELLOW "\033[33m"
#define ANSI_RED "\033[31m"
#define ANSI_BLUE_BG "\033[44m"
#define ANSI_WHITE "\033[37m"
#define ANSI_ERASE_LINE "\033[2K"
#define ANSI_SYNC_UPDATE_BEGIN "\033[?2026h"
#define ANSI_SYNC_UPDATE_END "\033[?2026l"

#define TENET_DEFAULT_SCREEN_ROWS 24
#define TENET_DEFAULT_SCREEN_COLS 80
#define TENET_MIN_SCREEN_ROWS 12
#define TENET_MIN_SCREEN_COLS 50
#define TENET_MAX_SCREEN_ROWS 200
#define TENET_MAX_SCREEN_COLS 500
#define TENET_MIN_SIDEBAR_COLS 18
#define TENET_MAX_SIDEBAR_COLS 32
#define TENET_MAX_INPUT_ROWS 6
#define TENET_MAX_VISIBLE_ROWS 256
#define TENET_INPUT_HISTORY_CAP 32
#define TENET_MAX_PM_TABS 8
#define TENET_HISTORY_CAP 200
#define TENET_HISTORY_LINE 4096
#define TENET_SCROLL_WHEEL_ROWS 3
#define TENET_BADAPPLE_SYSTEM_PATH "/usr/local/share/tenet/badapple.delta"
#define TENET_BADAPPLE_LOCAL_PATH "assets/badapple.delta"
#define TENET_BADAPPLE_FRAME_USEC 66667
#define TENET_KEYBOARD_PROTOCOL_ENABLE "\033[>1u\033[>4;2m"
#define TENET_KEYBOARD_PROTOCOL_DISABLE "\033[<u\033[>4;0m"
#define TENET_MOUSE_PROTOCOL_ENABLE "\033[?1000h\033[?1006h"
#define TENET_MOUSE_PROTOCOL_DISABLE "\033[?1006l\033[?1000l"

typedef struct client client_t;

struct client {
    int fd;
    int active;
    int skip_cr_tail;
    int in_chat;
    int bot_protocol;
    char username[TENET_MAX_USERNAME];
    char display_name[TENET_MAX_DISPLAY_NAME];
    char input_line[TENET_MAX_LINE];
    size_t input_cursor;
    char input_history[TENET_INPUT_HISTORY_CAP][TENET_MAX_LINE];
    size_t input_history_count;
    int input_history_index;
    int input_history_browsing;
    char input_history_draft[TENET_MAX_LINE];
    char status_line[TENET_HISTORY_LINE];
    char active_peer_username[TENET_MAX_USERNAME];
    char private_peers[TENET_MAX_PM_TABS][TENET_MAX_USERNAME];
    int private_peer_unread[TENET_MAX_PM_TABS];
    size_t private_peer_count;
    int lobby_unread;
    int screen_rows;
    int screen_cols;
    int last_render_rows;
    int last_render_cols;
    int last_input_top_row;
    int last_input_bottom_row;
    int last_input_inner_rows;
    char *write_buffer;
    size_t write_buffer_len;
    size_t write_buffer_cap;
    int write_buffering;
    int write_buffer_error;
    size_t history_base;
    int history_scroll_rows;
    pthread_mutex_t write_mutex;
    struct sockaddr_storage addr;
    socklen_t addr_len;
    pthread_t thread;
    client_t *next;
};

typedef struct ui_layout {
    int rows;
    int cols;
    int left_cols;
    int sidebar_cols;
    int chat_inner_width;
    int sidebar_inner_width;
    int input_text_width;
    int input_inner_rows;
    int input_top_row;
    int input_bottom_row;
    int chat_top_row;
    int chat_first_row;
    int chat_bottom_row;
    int chat_rows;
    int sidebar_col;
} ui_layout_t;

typedef struct history_entry {
    char line[TENET_HISTORY_LINE];
    char target_username[TENET_MAX_USERNAME];
    char private_from_username[TENET_MAX_USERNAME];
    char private_to_username[TENET_MAX_USERNAME];
} history_entry_t;

typedef struct badapple_movie {
    FILE *file;
    unsigned int rows;
    unsigned int cols;
    unsigned int frame_count;
    unsigned int frame_index;
    long data_offset;
    unsigned char *pixels;
} badapple_movie_t;

typedef struct server_state {
    const tenet_config_t *config;
    int listen_fd;
    int online_count;
    pthread_mutex_t mutex;
    client_t *clients;
    history_entry_t history[TENET_HISTORY_CAP];
    size_t history_count;
    size_t history_next;
    size_t history_total;
} server_state_t;

typedef struct tenet_local_user_record {
    char username[TENET_MAX_USERNAME];
    char display_name[TENET_MAX_DISPLAY_NAME];
    uint64_t salt;
    uint64_t password_hash;
} tenet_local_user_record_t;

extern volatile sig_atomic_t stop_requested;
extern server_state_t *global_state;

void safe_copy(char *dest, size_t size, const char *src);
void trim_line(char *text);
int ascii_equal_ignore_case(const char *left, const char *right);
int send_text(client_t *client, const char *text);
int send_fmt(client_t *client, const char *fmt, ...);
void request_server_echo(client_t *client);
int read_line(client_t *client, char *buffer, size_t size, int secret);
int valid_username(const char *username);
int valid_display_name(const char *display_name);
void render_header(client_t *client, int online_count);
int authenticate_client(server_state_t *state, client_t *client);
int authenticate_telnet_client(server_state_t *state, client_t *client);
int authenticate_ssh_client(server_state_t *state, client_t *client);

int tenet_local_user_find(const tenet_config_t *config,
                          const char *username,
                          tenet_local_user_record_t *record,
                          char *error,
                          size_t error_size);
int tenet_local_user_password_matches(const tenet_local_user_record_t *record,
                                      const char *password);
int tenet_local_user_check_registration_allowed(const tenet_config_t *config,
                                                const char *username,
                                                char *error,
                                                size_t error_size);
int tenet_local_user_save(const tenet_config_t *config,
                          const char *username,
                          const char *display_name,
                          const char *password,
                          char *error,
                          size_t error_size);

#endif
