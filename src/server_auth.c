#include "server_internal.h"

static void render_auth_header(server_state_t *state, client_t *client)
{
    int online_count;

    pthread_mutex_lock(&state->mutex);
    online_count = state->online_count;
    pthread_mutex_unlock(&state->mutex);
    render_header(client, online_count);
}

static int read_password(server_state_t *state,
                         client_t *client,
                         const char *prompt,
                         char *password,
                         size_t password_size)
{
    int telnet = state->config->transport == TENET_TRANSPORT_TELNET;

    (void)send_text(client, prompt);
    if (telnet) {
        request_server_echo(client);
    }
    if (read_line(client, password, password_size, 1) <= 0) {
        if (telnet) {
            request_server_echo(client);
        }
        return -1;
    }
    if (telnet) {
        request_server_echo(client);
    }
    trim_line(password);
    return 0;
}

static void set_authenticated_client(client_t *client,
                                     const char *username,
                                     const char *display_name)
{
    safe_copy(client->username, sizeof(client->username), username);
    safe_copy(client->display_name, sizeof(client->display_name),
              display_name != NULL && display_name[0] != '\0' ? display_name : username);
}

static const char *registration_gateway_username(void)
{
    const char *username = getenv("TENET_REGISTRATION_USER");

    return username != NULL && username[0] != '\0' ? username : "tenet";
}

static int client_is_registration_gateway(const client_t *client)
{
    return ascii_equal_ignore_case(client->username, registration_gateway_username());
}

static int username_in_list(const char *list, const char *username)
{
    const char *cursor = list;

    if (list == NULL || username == NULL || username[0] == '\0') {
        return 0;
    }
    while (*cursor != '\0') {
        char token[TENET_MAX_USERNAME];
        size_t len = 0;

        while (*cursor != '\0' && (isspace((unsigned char)*cursor) || *cursor == ',')) {
            cursor++;
        }
        while (*cursor != '\0' && !isspace((unsigned char)*cursor) && *cursor != ',') {
            if (len + 1 < sizeof(token)) {
                token[len++] = *cursor;
            }
            cursor++;
        }
        token[len] = '\0';
        if (token[0] != '\0' && ascii_equal_ignore_case(token, username)) {
            return 1;
        }
    }
    return 0;
}

static int client_is_internal_user(server_state_t *state, const client_t *client)
{
    return username_in_list(state->config->internal_users, client->username);
}

static int authenticate_registered_user(server_state_t *state,
                                        client_t *client,
                                        const tenet_local_user_record_t *record)
{
    char password[TENET_MAX_PASSWORD];

    password[0] = '\0';
    if (read_password(state, client, "密码: ", password, sizeof(password)) != 0) {
        return -1;
    }
    if (tenet_local_user_password_matches(record, password)) {
        set_authenticated_client(client, record->username, record->display_name);
        return 0;
    }
    (void)send_text(client, ANSI_RED "密码错误。\r\n" ANSI_RESET);
    return 1;
}

static int register_app_user(server_state_t *state,
                             client_t *client,
                             const char *username)
{
    char display_name[TENET_MAX_DISPLAY_NAME];
    char password[TENET_MAX_PASSWORD];
    char confirm[TENET_MAX_PASSWORD];
    char error[256];
    int attempts;

    render_auth_header(state, client);
    (void)send_fmt(client,
                   "欢迎来到 " ANSI_BOLD "tenet" ANSI_RESET "。\r\n\r\n"
                   ANSI_YELLOW "用户 %s 尚未注册，需要创建聊天账号。" ANSI_RESET "\r\n"
                   "这是 tenet 自己的账号注册，和 SSH/LDAP 登录相互独立。\r\n\r\n",
                   username);

    for (attempts = 0; attempts < 3; attempts++) {
        display_name[0] = '\0';
        password[0] = '\0';
        confirm[0] = '\0';
        error[0] = '\0';

        (void)send_text(client, "Display name: ");
        if (read_line(client, display_name, sizeof(display_name), 0) <= 0) {
            return -1;
        }
        trim_line(display_name);
        if (!valid_display_name(display_name)) {
            (void)send_text(client, ANSI_RED "Display name 不能为空，且不能包含控制字符。\r\n" ANSI_RESET);
            continue;
        }
        if (read_password(state, client, "密码: ", password, sizeof(password)) != 0) {
            return -1;
        }
        if (password[0] == '\0') {
            (void)send_text(client, ANSI_RED "密码不能为空。\r\n" ANSI_RESET);
            continue;
        }
        if (read_password(state, client, "确认密码: ", confirm, sizeof(confirm)) != 0) {
            return -1;
        }
        if (strcmp(password, confirm) != 0) {
            (void)send_text(client, ANSI_RED "两次输入的密码不一致。\r\n" ANSI_RESET);
            continue;
        }
        if (tenet_local_user_save(state->config, username, display_name, password,
                                  error, sizeof(error)) != 0) {
            (void)send_fmt(client, ANSI_RED "注册失败: %s\r\n" ANSI_RESET,
                           error[0] != '\0' ? error : "无法保存用户");
            continue;
        }
        set_authenticated_client(client, username, display_name);
        (void)send_text(client, ANSI_GREEN "注册成功，正在进入聊天室...\r\n" ANSI_RESET);
        return 0;
    }

    (void)send_text(client, "注册失败次数过多，连接关闭。\r\n");
    return -1;
}


int authenticate_telnet_client(server_state_t *state, client_t *client)
{
    char username[TENET_MAX_USERNAME];
    char display_name[TENET_MAX_DISPLAY_NAME];
    char password[TENET_MAX_PASSWORD];
    char error[256];
    int attempts;

    render_auth_header(state, client);
    (void)send_text(client,
                    "欢迎来到 " ANSI_BOLD "tenet" ANSI_RESET "。\r\n"
                    "使用 LDAP 账号登录。\r\n\r\n");

    for (attempts = 0; attempts < 3; attempts++) {
        username[0] = '\0';
        display_name[0] = '\0';
        password[0] = '\0';
        error[0] = '\0';

        (void)send_text(client, "用户名: ");
        if (read_line(client, username, sizeof(username), 0) <= 0) {
            return -1;
        }
        trim_line(username);
        if (!valid_username(username)) {
            (void)send_text(client, ANSI_RED "用户名只能包含字母、数字、点、下划线、短横线或 @。\r\n" ANSI_RESET);
            continue;
        }
        if (read_password(state, client, "密码: ", password, sizeof(password)) != 0) {
            return -1;
        }

        if (tenet_ldap_authenticate(state->config, username, password,
                                    display_name, sizeof(display_name),
                                    error, sizeof(error)) == 0) {
            set_authenticated_client(client, username,
                                     display_name[0] != '\0' ? display_name : username);
            return 0;
        }
        if (state->config->ldap_enabled) {
            (void)send_fmt(client, ANSI_RED "登录失败: %s\r\n" ANSI_RESET,
                           error[0] != '\0' ? error : "LDAP 认证失败");
        } else {
            (void)send_text(client, ANSI_RED "登录失败。\r\n" ANSI_RESET);
        }
    }

    (void)send_text(client, "尝试次数过多，连接关闭。\r\n");
    return -1;
}

int authenticate_client(server_state_t *state, client_t *client)
{
    char username[TENET_MAX_USERNAME];
    char error[256];
    int attempts;

    render_auth_header(state, client);
    (void)send_text(client,
                    "欢迎来到 " ANSI_BOLD "tenet" ANSI_RESET "。\r\n"
                    "请登录或注册 tenet 独立聊天账号。\r\n\r\n");

    for (attempts = 0; attempts < 3; attempts++) {
        tenet_local_user_record_t local_user;
        int local_rc;

        username[0] = '\0';
        error[0] = '\0';

        (void)send_text(client, "用户名: ");
        if (read_line(client, username, sizeof(username), 0) <= 0) {
            return -1;
        }
        trim_line(username);
        if (!valid_username(username)) {
            (void)send_text(client, ANSI_RED "用户名只能包含字母、数字、点、下划线、短横线或 @。\r\n" ANSI_RESET);
            continue;
        }

        memset(&local_user, 0, sizeof(local_user));
        local_rc = tenet_local_user_find(state->config, username, &local_user,
                                         error, sizeof(error));
        if (local_rc == 0) {
            int auth_rc = authenticate_registered_user(state, client, &local_user);
            if (auth_rc == 0) {
                return 0;
            }
            if (auth_rc < 0) {
                return -1;
            }
            continue;
        }
        if (local_rc < 0) {
            (void)send_fmt(client, ANSI_RED "用户库读取失败: %s\r\n" ANSI_RESET,
                           error[0] != '\0' ? error : "未知错误");
            continue;
        }

        error[0] = '\0';
        if (tenet_local_user_check_registration_allowed(state->config, username,
                                                        error, sizeof(error)) != 0) {
            (void)send_fmt(client, ANSI_RED "不能注册: %s\r\n" ANSI_RESET,
                           error[0] != '\0' ? error : "该用户名不能注册");
            continue;
        }

        {
            int register_rc = register_app_user(state, client, username);
            if (register_rc == 0) {
                return 0;
            }
            if (register_rc < 0) {
                return -1;
            }
        }
    }

    (void)send_text(client, "尝试次数过多，连接关闭。\r\n");
    return -1;
}

int authenticate_ssh_client(server_state_t *state, client_t *client)
{
    char display_name[TENET_MAX_DISPLAY_NAME];
    char error[256];
    tenet_local_user_record_t local_user;
    int lookup_rc;

    display_name[0] = '\0';
    error[0] = '\0';
    if (client_is_internal_user(state, client)) {
        return 0;
    }
    lookup_rc = tenet_ldap_lookup_user(state->config, client->username,
                                       display_name, sizeof(display_name),
                                       error, sizeof(error));
    if (lookup_rc == 0) {
        if (valid_display_name(display_name)) {
            safe_copy(client->display_name, sizeof(client->display_name), display_name);
        }
        return 0;
    }

    memset(&local_user, 0, sizeof(local_user));
    if (tenet_local_user_find(state->config, client->username, &local_user,
                              error, sizeof(error)) == 0) {
        set_authenticated_client(client, local_user.username, local_user.display_name);
        return 0;
    }

    if (lookup_rc == 1) {
        if (client_is_registration_gateway(client)) {
            return authenticate_client(state, client);
        }
        (void)send_fmt(client, ANSI_RED
                       "该 SSH 用户不在 LDAP 中，也不是已注册的 tenet 账号。\r\n"
                       "请先使用 ssh %s@host 进入注册入口。\r\n" ANSI_RESET,
                       registration_gateway_username());
        return -1;
    }

    return 0;
}
