#include "tenet.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void copy_string(char *dest, size_t size, const char *src)
{
    if (size == 0) {
        return;
    }
    if (src == NULL) {
        dest[0] = '\0';
        return;
    }
    snprintf(dest, size, "%s", src);
}

static int parse_int(const char *value, int min, int max, int *out)
{
    char *end = NULL;
    long parsed;

    errno = 0;
    parsed = strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed < min || parsed > max) {
        return -1;
    }
    *out = (int)parsed;
    return 0;
}

static int env_truthy(const char *value)
{
    return value != NULL &&
           (strcmp(value, "1") == 0 || strcmp(value, "yes") == 0 ||
            strcmp(value, "true") == 0 || strcmp(value, "on") == 0);
}

void tenet_config_defaults(tenet_config_t *config)
{
    memset(config, 0, sizeof(*config));
    copy_string(config->bind_addr, sizeof(config->bind_addr), TENET_DEFAULT_BIND_ADDR);
    config->port = TENET_DEFAULT_PORT;
    config->max_clients = 64;
    config->transport = TENET_TRANSPORT_SSH;
    copy_string(config->socket_path, sizeof(config->socket_path), TENET_DEFAULT_SOCKET_PATH);
    copy_string(config->local_user_db_path, sizeof(config->local_user_db_path), TENET_DEFAULT_LOCAL_USER_DB);
    config->internal_users[0] = '\0';
    config->sync_local_ssh_users = 0;

    config->ldap_enabled = 1;
    copy_string(config->ldap_host, sizeof(config->ldap_host), TENET_DEFAULT_LDAP_HOST);
    config->ldap_port = TENET_DEFAULT_LDAP_PORT;
    copy_string(config->ldap_base_dn, sizeof(config->ldap_base_dn), TENET_DEFAULT_BASE_DN);
    config->ldap_timeout_sec = 5;
    config->ldap_search = 0;
}

static void apply_env(tenet_config_t *config)
{
    const char *value;

    value = getenv("TENET_BIND");
    if (value != NULL && *value != '\0') {
        copy_string(config->bind_addr, sizeof(config->bind_addr), value);
    }
    value = getenv("TENET_PORT");
    if (value != NULL) {
        (void)parse_int(value, 1, 65535, &config->port);
    }
    value = getenv("TENET_MAX_CLIENTS");
    if (value != NULL) {
        (void)parse_int(value, 1, 10000, &config->max_clients);
    }
    value = getenv("TENET_SOCKET");
    if (value != NULL && *value != '\0') {
        copy_string(config->socket_path, sizeof(config->socket_path), value);
    }
    value = getenv("TENET_LOCAL_USER_DB");
    if (value != NULL && *value != '\0') {
        copy_string(config->local_user_db_path, sizeof(config->local_user_db_path), value);
    }
    value = getenv("TENET_INTERNAL_USERS");
    if (value != NULL && *value != '\0') {
        copy_string(config->internal_users, sizeof(config->internal_users), value);
    }
    if (env_truthy(getenv("TENET_SYNC_LOCAL_SSH_USERS"))) {
        config->sync_local_ssh_users = 1;
    }
    if (env_truthy(getenv("TENET_NO_LDAP"))) {
        config->ldap_enabled = 0;
    }
    value = getenv("TENET_LDAP_HOST");
    if (value != NULL && *value != '\0') {
        copy_string(config->ldap_host, sizeof(config->ldap_host), value);
    }
    value = getenv("TENET_LDAP_PORT");
    if (value != NULL) {
        (void)parse_int(value, 1, 65535, &config->ldap_port);
    }
    value = getenv("TENET_LDAP_BASE_DN");
    if (value != NULL && *value != '\0') {
        copy_string(config->ldap_base_dn, sizeof(config->ldap_base_dn), value);
    }
    value = getenv("TENET_LDAP_TIMEOUT");
    if (value != NULL) {
        (void)parse_int(value, 1, 120, &config->ldap_timeout_sec);
    }
    if (env_truthy(getenv("TENET_LDAP_SEARCH"))) {
        config->ldap_search = 1;
    }
    value = getenv("TENET_LDAP_BIND_DN");
    if (value != NULL) {
        copy_string(config->ldap_bind_dn, sizeof(config->ldap_bind_dn), value);
    }
    value = getenv("TENET_LDAP_BIND_PASSWORD");
    if (value != NULL) {
        copy_string(config->ldap_bind_password, sizeof(config->ldap_bind_password), value);
    }
}

static void usage(FILE *stream)
{
    fprintf(stream,
            "tenet - SSH 聊天室\n\n"
            "用法:\n"
            "  tenet [--ssh-backend] [选项]\n"
            "  tenet --ssh-session [选项]\n\n"
            "选项:\n"
            "  --ssh-backend            启动 OpenSSH ForceCommand 后端，默认模式\n"
            "  --ssh-session            作为 SSH 会话连接本机后端\n"
            "  --socket PATH            Unix socket 路径，默认 /tmp/tenet.sock\n"
            "  --telnet                 兼容调试模式：启动旧 Telnet 监听\n"
            "  --bind ADDR              Telnet 模式监听地址，默认 0.0.0.0\n"
            "  --port PORT              Telnet 模式端口，默认 2323\n"
            "  --max-clients N          最大在线人数，默认 64\n"
            "  --no-ldap                Telnet 调试模式关闭 LDAP\n"
            "  --ldap-host HOST         LDAP 地址，默认 ldap.example.org\n"
            "  --ldap-port PORT         LDAP 端口，默认 389\n"
            "  --ldap-base-dn DN        BaseDN，默认 DC=example,DC=org\n"
            "  --ldap-timeout SEC       LDAP 超时秒数，默认 5\n"
            "  --ldap-search            Telnet 模式先搜索用户 DN 再绑定验证\n"
            "  --ldap-bind-dn DN        搜索模式的 LDAP 绑定 DN\n"
            "  --ldap-bind-password PW  搜索模式的 LDAP 绑定密码\n"
            "  --local-user-db PATH     tenet 聊天账号库，默认 tenet-users.db\n"
            "  --internal-users LIST    允许从 Unix socket 直连的内部服务用户列表\n"
            "  --sync-local-ssh-users   注册时同步创建同名 SSH 本地用户\n"
            "  -h, --help               显示帮助\n\n"
            "环境变量同名可用，例如 TENET_SOCKET、TENET_PORT、TENET_LDAP_HOST。\n");
}

static int need_arg(int argc, char **argv, int index)
{
    if (index + 1 >= argc) {
        fprintf(stderr, "%s 需要参数\n", argv[index]);
        return -1;
    }
    return 0;
}

static int parse_args(int argc, char **argv, tenet_config_t *config, int *run_session)
{
    int index;

    for (index = 1; index < argc; index++) {
        if (strcmp(argv[index], "--ssh-backend") == 0) {
            config->transport = TENET_TRANSPORT_SSH;
            *run_session = 0;
        } else if (strcmp(argv[index], "--ssh-session") == 0) {
            config->transport = TENET_TRANSPORT_SSH;
            *run_session = 1;
        } else if (strcmp(argv[index], "--socket") == 0) {
            if (need_arg(argc, argv, index) != 0) return -1;
            copy_string(config->socket_path, sizeof(config->socket_path), argv[++index]);
        } else if (strcmp(argv[index], "--telnet") == 0) {
            config->transport = TENET_TRANSPORT_TELNET;
            *run_session = 0;
        } else if (strcmp(argv[index], "--bind") == 0) {
            if (need_arg(argc, argv, index) != 0) return -1;
            copy_string(config->bind_addr, sizeof(config->bind_addr), argv[++index]);
        } else if (strcmp(argv[index], "--port") == 0) {
            if (need_arg(argc, argv, index) != 0) return -1;
            if (parse_int(argv[++index], 1, 65535, &config->port) != 0) {
                fprintf(stderr, "端口无效: %s\n", argv[index]);
                return -1;
            }
        } else if (strcmp(argv[index], "--max-clients") == 0) {
            if (need_arg(argc, argv, index) != 0) return -1;
            if (parse_int(argv[++index], 1, 10000, &config->max_clients) != 0) {
                fprintf(stderr, "最大在线人数无效: %s\n", argv[index]);
                return -1;
            }
        } else if (strcmp(argv[index], "--no-ldap") == 0) {
            config->ldap_enabled = 0;
        } else if (strcmp(argv[index], "--ldap-host") == 0) {
            if (need_arg(argc, argv, index) != 0) return -1;
            copy_string(config->ldap_host, sizeof(config->ldap_host), argv[++index]);
        } else if (strcmp(argv[index], "--ldap-port") == 0) {
            if (need_arg(argc, argv, index) != 0) return -1;
            if (parse_int(argv[++index], 1, 65535, &config->ldap_port) != 0) {
                fprintf(stderr, "LDAP 端口无效: %s\n", argv[index]);
                return -1;
            }
        } else if (strcmp(argv[index], "--ldap-base-dn") == 0) {
            if (need_arg(argc, argv, index) != 0) return -1;
            copy_string(config->ldap_base_dn, sizeof(config->ldap_base_dn), argv[++index]);
        } else if (strcmp(argv[index], "--ldap-timeout") == 0) {
            if (need_arg(argc, argv, index) != 0) return -1;
            if (parse_int(argv[++index], 1, 120, &config->ldap_timeout_sec) != 0) {
                fprintf(stderr, "LDAP 超时无效: %s\n", argv[index]);
                return -1;
            }
        } else if (strcmp(argv[index], "--ldap-search") == 0) {
            config->ldap_search = 1;
        } else if (strcmp(argv[index], "--ldap-bind-dn") == 0) {
            if (need_arg(argc, argv, index) != 0) return -1;
            copy_string(config->ldap_bind_dn, sizeof(config->ldap_bind_dn), argv[++index]);
        } else if (strcmp(argv[index], "--ldap-bind-password") == 0) {
            if (need_arg(argc, argv, index) != 0) return -1;
            copy_string(config->ldap_bind_password, sizeof(config->ldap_bind_password), argv[++index]);
        } else if (strcmp(argv[index], "--local-user-db") == 0) {
            if (need_arg(argc, argv, index) != 0) return -1;
            copy_string(config->local_user_db_path, sizeof(config->local_user_db_path), argv[++index]);
        } else if (strcmp(argv[index], "--internal-users") == 0) {
            if (need_arg(argc, argv, index) != 0) return -1;
            copy_string(config->internal_users, sizeof(config->internal_users), argv[++index]);
        } else if (strcmp(argv[index], "--sync-local-ssh-users") == 0) {
            config->sync_local_ssh_users = 1;
        } else if (strcmp(argv[index], "-h") == 0 || strcmp(argv[index], "--help") == 0) {
            usage(stdout);
            exit(0);
        } else {
            fprintf(stderr, "未知选项: %s\n", argv[index]);
            usage(stderr);
            return -1;
        }
    }

    return 0;
}

int main(int argc, char **argv)
{
    tenet_config_t config;
    int run_session = 0;

    tenet_config_defaults(&config);
    apply_env(&config);
    if (parse_args(argc, argv, &config, &run_session) != 0) {
        return 2;
    }

    if (run_session) {
        return tenet_session_run(&config) == 0 ? 0 : 1;
    }
    return tenet_server_run(&config) == 0 ? 0 : 1;
}
