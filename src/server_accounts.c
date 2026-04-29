#include "server_internal.h"

#define TENET_PASSWORD_FNV_OFFSET 1469598103934665603ULL
#define TENET_PASSWORD_FNV_PRIME 1099511628211ULL

static void set_account_error(char *error, size_t error_size, const char *message)
{
    if (error_size == 0) {
        return;
    }
    snprintf(error, error_size, "%s", message != NULL ? message : "未知错误");
}

static const char *local_user_db_path(const tenet_config_t *config)
{
    if (config->local_user_db_path[0] != '\0') {
        return config->local_user_db_path;
    }
    return TENET_DEFAULT_LOCAL_USER_DB;
}

static uint64_t hash_bytes(uint64_t hash, const void *data, size_t len)
{
    const unsigned char *bytes = data;
    size_t index;

    for (index = 0; index < len; index++) {
        hash ^= (uint64_t)bytes[index];
        hash *= TENET_PASSWORD_FNV_PRIME;
    }
    return hash;
}

static uint64_t password_hash_for(const char *username, const char *password, uint64_t salt)
{
    uint64_t hash = TENET_PASSWORD_FNV_OFFSET;

    hash = hash_bytes(hash, &salt, sizeof(salt));
    hash = hash_bytes(hash, username, strlen(username));
    hash = hash_bytes(hash, "\xff", 1);
    hash = hash_bytes(hash, password, strlen(password));
    return hash;
}

static uint64_t generate_salt(void)
{
    uint64_t salt = 0;
    int fd = open("/dev/urandom", O_RDONLY);

    if (fd >= 0) {
        unsigned char *cursor = (unsigned char *)&salt;
        size_t remaining = sizeof(salt);

        while (remaining > 0) {
            ssize_t got = read(fd, cursor, remaining);
            if (got < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            if (got == 0) {
                break;
            }
            cursor += got;
            remaining -= (size_t)got;
        }
        close(fd);
    }
    if (salt == 0) {
        salt = (uint64_t)time(NULL) ^ ((uint64_t)getpid() << 32) ^ (uint64_t)(uintptr_t)&salt;
    }
    return salt;
}

static int parse_hex_u64(const char *text, uint64_t *value)
{
    char *end = NULL;
    unsigned long long parsed;

    errno = 0;
    parsed = strtoull(text, &end, 16);
    if (errno != 0 || end == text || *end != '\0') {
        return -1;
    }
    *value = (uint64_t)parsed;
    return 0;
}

static int parse_user_record(char *line, tenet_local_user_record_t *record)
{
    char *username;
    char *display_name;
    char *salt_text;
    char *hash_text;
    char *cursor;

    cursor = strchr(line, '\n');
    if (cursor != NULL) {
        *cursor = '\0';
    }
    cursor = strchr(line, '\r');
    if (cursor != NULL) {
        *cursor = '\0';
    }

    username = line;
    display_name = strchr(username, '\t');
    if (display_name == NULL) {
        return -1;
    }
    *display_name++ = '\0';
    salt_text = strchr(display_name, '\t');
    if (salt_text == NULL) {
        return -1;
    }
    *salt_text++ = '\0';
    hash_text = strchr(salt_text, '\t');
    if (hash_text == NULL) {
        return -1;
    }
    *hash_text++ = '\0';
    if (strchr(hash_text, '\t') != NULL || !valid_username(username) ||
        !valid_display_name(display_name) || parse_hex_u64(salt_text, &record->salt) != 0 ||
        parse_hex_u64(hash_text, &record->password_hash) != 0) {
        return -1;
    }

    safe_copy(record->username, sizeof(record->username), username);
    safe_copy(record->display_name, sizeof(record->display_name), display_name);
    return 0;
}

static int valid_local_ssh_username(const char *username)
{
    size_t len;
    size_t index;

    if (username == NULL) {
        return 0;
    }
    len = strlen(username);
    if (len < 1 || len >= TENET_MAX_USERNAME) {
        return 0;
    }
    if (!(username[0] == '_' || (username[0] >= 'a' && username[0] <= 'z'))) {
        return 0;
    }
    for (index = 1; index < len; index++) {
        unsigned char ch = (unsigned char)username[index];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
              ch == '_' || ch == '-')) {
            return 0;
        }
    }
    return 1;
}

static int local_passwd_user_exists(const char *username)
{
    FILE *file = fopen("/etc/passwd", "r");
    char line[1024];

    if (file == NULL) {
        return 0;
    }
    while (fgets(line, sizeof(line), file) != NULL) {
        char *colon = strchr(line, ':');

        if (colon == NULL) {
            continue;
        }
        *colon = '\0';
        if (strcmp(line, username) == 0) {
            fclose(file);
            return 1;
        }
    }
    fclose(file);
    return 0;
}

static int run_child_command(char *const argv[], const char *input)
{
    int pipe_fds[2] = {-1, -1};
    pid_t pid;
    int status;

    if (input != NULL && pipe(pipe_fds) != 0) {
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        if (pipe_fds[0] >= 0) {
            close(pipe_fds[0]);
            close(pipe_fds[1]);
        }
        return -1;
    }
    if (pid == 0) {
        if (input != NULL) {
            close(pipe_fds[1]);
            if (dup2(pipe_fds[0], STDIN_FILENO) < 0) {
                _exit(127);
            }
            close(pipe_fds[0]);
        }
        execvp(argv[0], argv);
        _exit(127);
    }

    if (input != NULL) {
        const char *cursor = input;
        size_t remaining = strlen(input);

        close(pipe_fds[0]);
        while (remaining > 0) {
            ssize_t written = write(pipe_fds[1], cursor, remaining);
            if (written < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            if (written == 0) {
                break;
            }
            cursor += written;
            remaining -= (size_t)written;
        }
        close(pipe_fds[1]);
    }

    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return -1;
        }
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

static int ensure_local_ssh_user(const char *username, const char *password,
                                 char *error, size_t error_size)
{
    char chpasswd_line[TENET_MAX_USERNAME + TENET_MAX_PASSWORD + 4];
    struct passwd *passwd_entry;

    if (!valid_local_ssh_username(username)) {
        set_account_error(error, error_size,
                          "注册用户名需要作为 SSH 本地账号使用：只能用小写字母、数字、下划线、短横线，且以小写字母或下划线开头");
        return -1;
    }

    if (local_passwd_user_exists(username)) {
        set_account_error(error, error_size, "系统中已存在同名本地用户，不能覆盖为 tenet 独立账号");
        return -1;
    }
    passwd_entry = getpwnam(username);
    if (passwd_entry != NULL) {
        set_account_error(error, error_size, "系统或 LDAP 中已存在同名用户，不能创建独立 SSH 账号");
        return -1;
    }
    {
        char *const adduser_argv[] = {
            "adduser", "--disabled-password", "--gecos", "tenet chat user",
            "--shell", "/bin/sh", (char *)username, NULL
        };
        if (run_child_command(adduser_argv, NULL) != 0) {
            set_account_error(error, error_size, "创建 SSH 本地用户失败");
            return -1;
        }
    }

    snprintf(chpasswd_line, sizeof(chpasswd_line), "%s:%s\n", username, password);
    {
        char *const chpasswd_argv[] = {"chpasswd", NULL};
        if (run_child_command(chpasswd_argv, chpasswd_line) != 0) {
            set_account_error(error, error_size, "设置 SSH 本地用户密码失败");
            return -1;
        }
    }
    return 0;
}


int tenet_local_user_find(const tenet_config_t *config,
                          const char *username,
                          tenet_local_user_record_t *record,
                          char *error,
                          size_t error_size)
{
    const char *path = local_user_db_path(config);
    FILE *file = fopen(path, "r");
    char line[TENET_MAX_USERNAME + TENET_MAX_DISPLAY_NAME + 80];

    if (file == NULL) {
        if (errno == ENOENT) {
            return 1;
        }
        set_account_error(error, error_size, "无法读取本地用户库");
        return -1;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        tenet_local_user_record_t parsed;

        memset(&parsed, 0, sizeof(parsed));
        if (parse_user_record(line, &parsed) != 0) {
            continue;
        }
        if (ascii_equal_ignore_case(parsed.username, username)) {
            if (record != NULL) {
                *record = parsed;
            }
            fclose(file);
            return 0;
        }
    }

    fclose(file);
    return 1;
}

int tenet_local_user_password_matches(const tenet_local_user_record_t *record,
                                      const char *password)
{
    return record != NULL && password != NULL &&
           password_hash_for(record->username, password, record->salt) == record->password_hash;
}

static int lookup_ldap_user_for_registration(const tenet_config_t *config,
                                             const char *username,
                                             char *display_name,
                                             size_t display_name_size,
                                             char *error,
                                             size_t error_size)
{
    char lowercase_username[TENET_MAX_USERNAME];
    size_t index;
    int changed = 0;
    int rc;

    rc = tenet_ldap_lookup_user(config, username,
                                display_name, display_name_size,
                                error, error_size);
    if (rc != 1) {
        return rc;
    }

    for (index = 0; username[index] != '\0' && index < sizeof(lowercase_username) - 1; index++) {
        unsigned char ch = (unsigned char)username[index];
        lowercase_username[index] = (char)tolower(ch);
        if (lowercase_username[index] != username[index]) {
            changed = 1;
        }
    }
    lowercase_username[index] = '\0';
    if (!changed || username[index] != '\0') {
        return 1;
    }

    return tenet_ldap_lookup_user(config, lowercase_username,
                                  display_name, display_name_size,
                                  error, error_size);
}

int tenet_local_user_check_registration_allowed(const tenet_config_t *config,
                                                const char *username,
                                                char *error,
                                                size_t error_size)
{
    char display_name[TENET_MAX_DISPLAY_NAME];
    char ldap_error[256];
    int rc;

    if (!config->ldap_enabled) {
        return 0;
    }

    display_name[0] = '\0';
    ldap_error[0] = '\0';
    rc = lookup_ldap_user_for_registration(config, username,
                                           display_name, sizeof(display_name),
                                           ldap_error, sizeof(ldap_error));
    if (rc == 0) {
        set_account_error(error, error_size,
                          "LDAP 中已存在同名账号，不能注册同 ID 的 tenet 账号");
        return -1;
    }
    if (rc < 0) {
        char message[512];

        snprintf(message, sizeof(message), "无法确认 LDAP 中是否存在同名账号，暂不能注册%s%s",
                 ldap_error[0] != '\0' ? ": " : "",
                 ldap_error[0] != '\0' ? ldap_error : "");
        set_account_error(error, error_size, message);
        return -1;
    }
    return 0;
}


int tenet_local_user_save(const tenet_config_t *config,
                          const char *username,
                          const char *display_name,
                          const char *password,
                          char *error,
                          size_t error_size)
{
    const char *path = local_user_db_path(config);
    tenet_local_user_record_t existing;
    uint64_t salt;
    uint64_t hash;
    int fd;
    FILE *file;
    int found;

    if (!valid_username(username) || !valid_display_name(display_name) ||
        password == NULL || password[0] == '\0') {
        set_account_error(error, error_size, "注册信息无效");
        return -1;
    }

    found = tenet_local_user_find(config, username, &existing, error, error_size);
    if (found == 0) {
        set_account_error(error, error_size, "该用户已注册");
        return -1;
    }
    if (found < 0) {
        return -1;
    }

    if (tenet_local_user_check_registration_allowed(config, username, error, error_size) != 0) {
        return -1;
    }

    if (config->sync_local_ssh_users &&
        ensure_local_ssh_user(username, password, error, error_size) != 0) {
        return -1;
    }

    salt = generate_salt();
    hash = password_hash_for(username, password, salt);
    fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) {
        set_account_error(error, error_size, "无法写入本地用户库");
        return -1;
    }
    file = fdopen(fd, "a");
    if (file == NULL) {
        close(fd);
        set_account_error(error, error_size, "无法打开本地用户库");
        return -1;
    }
    if (fprintf(file, "%s\t%s\t%016llx\t%016llx\n", username, display_name,
                (unsigned long long)salt, (unsigned long long)hash) < 0 ||
        fclose(file) != 0) {
        set_account_error(error, error_size, "保存本地用户失败");
        return -1;
    }
    return 0;
}
