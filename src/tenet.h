#ifndef TENET_H
#define TENET_H

#include <stddef.h>

#define TENET_DEFAULT_PORT 2323
#define TENET_DEFAULT_BIND_ADDR "0.0.0.0"
#define TENET_DEFAULT_SOCKET_PATH "/tmp/tenet.sock"
#define TENET_DEFAULT_LOCAL_USER_DB "tenet-users.db"
#define TENET_DEFAULT_LDAP_HOST "ldap.example.org"
#define TENET_DEFAULT_LDAP_PORT 389
#define TENET_DEFAULT_BASE_DN "DC=example,DC=org"

#define TENET_MAX_USERNAME 64
#define TENET_MAX_DISPLAY_NAME 256
#define TENET_MAX_PASSWORD 512
#define TENET_MAX_LINE 2048
#define TENET_MAX_HOST 256
#define TENET_MAX_DN 512
#define TENET_MAX_PATH 512

typedef enum tenet_transport {
    TENET_TRANSPORT_SSH = 0,
    TENET_TRANSPORT_TELNET = 1
} tenet_transport_t;

typedef struct tenet_config {
    char bind_addr[TENET_MAX_HOST];
    int port;
    int max_clients;
    tenet_transport_t transport;
    char socket_path[108];

    int ldap_enabled;
    char ldap_host[TENET_MAX_HOST];
    int ldap_port;
    char ldap_base_dn[TENET_MAX_DN];
    int ldap_timeout_sec;
    int ldap_search;
    char ldap_bind_dn[TENET_MAX_DN];
    char ldap_bind_password[TENET_MAX_PASSWORD];
    char local_user_db_path[TENET_MAX_PATH];
    int sync_local_ssh_users;
} tenet_config_t;

void tenet_config_defaults(tenet_config_t *config);
int tenet_server_run(const tenet_config_t *config);
int tenet_session_run(const tenet_config_t *config);
int tenet_ldap_authenticate(const tenet_config_t *config,
                            const char *username,
                            const char *password,
                            char *display_name,
                            size_t display_name_size,
                            char *error,
                            size_t error_size);
int tenet_ldap_lookup_user(const tenet_config_t *config,
                           const char *username,
                           char *display_name,
                           size_t display_name_size,
                           char *error,
                           size_t error_size);

#endif
