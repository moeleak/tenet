#!/bin/sh
set -eu

TENET_USER="${TENET_USER:-tenet}"
TENET_PASSWORD="${TENET_PASSWORD:-tenet}"
TENET_AUTH_MODE="${TENET_AUTH_MODE:-local}"
TENET_SSH_PORT="${TENET_SSH_PORT:-2222}"
TENET_SOCKET="${TENET_SOCKET:-/run/tenet/tenet.sock}"
TENET_LOCAL_USER_DB="${TENET_LOCAL_USER_DB:-/var/lib/tenet/users.db}"
TENET_SYNC_LOCAL_SSH_USERS="${TENET_SYNC_LOCAL_SSH_USERS:-1}"
export TENET_LOCAL_USER_DB TENET_SYNC_LOCAL_SSH_USERS
TENET_MAX_CLIENTS="${TENET_MAX_CLIENTS:-64}"
TENET_AUTHORIZED_KEYS="${TENET_AUTHORIZED_KEYS:-}"
TENET_AUTHORIZED_KEYS_FILE="${TENET_AUTHORIZED_KEYS_FILE:-/etc/tenet/authorized_keys}"
TENET_AUTHORIZED_KEYS_DIR="${TENET_AUTHORIZED_KEYS_DIR:-/var/lib/tenet/ssh/authorized_keys.d}"
TENET_BACKEND_ARGS="${TENET_BACKEND_ARGS:-}"
TENET_DISABLE_PASSWORD_AUTH="${TENET_DISABLE_PASSWORD_AUTH:-0}"
TENET_ENABLE_SSH_COPY_ID="${TENET_ENABLE_SSH_COPY_ID:-1}"
TENET_ENABLE_LOCAL_GATEWAY="${TENET_ENABLE_LOCAL_GATEWAY:-1}"
TENET_REGISTRATION_USER="${TENET_REGISTRATION_USER:-tenet}"
TENET_LOCAL_GATEWAY_USERS="${TENET_LOCAL_GATEWAY_USERS:-$TENET_REGISTRATION_USER}"
TENET_LDAP_HOST="${TENET_LDAP_HOST:-ldap.example.org}"
TENET_LDAP_PORT="${TENET_LDAP_PORT:-389}"
TENET_LDAP_URI="${TENET_LDAP_URI:-ldap://${TENET_LDAP_HOST}:${TENET_LDAP_PORT}}"
TENET_LDAP_BASE_DN="${TENET_LDAP_BASE_DN:-DC=example,DC=org}"
TENET_LDAP_DOMAIN="${TENET_LDAP_DOMAIN:-tenet}"
TENET_LDAP_SCHEMA="${TENET_LDAP_SCHEMA:-ad}"
TENET_LDAP_USER_NAME_ATTR="${TENET_LDAP_USER_NAME_ATTR:-sAMAccountName}"
TENET_LDAP_USER_FULLNAME_ATTR="${TENET_LDAP_USER_FULLNAME_ATTR:-displayName}"
TENET_LDAP_USER_OBJECT_CLASS="${TENET_LDAP_USER_OBJECT_CLASS:-user}"
TENET_LDAP_GROUP_OBJECT_CLASS="${TENET_LDAP_GROUP_OBJECT_CLASS:-group}"
TENET_LDAP_START_TLS="${TENET_LDAP_START_TLS:-0}"
TENET_LDAP_TLS_REQCERT="${TENET_LDAP_TLS_REQCERT:-allow}"
TENET_LDAP_ID_MAPPING="${TENET_LDAP_ID_MAPPING:-1}"
TENET_LDAP_REFERRALS="${TENET_LDAP_REFERRALS:-0}"
TENET_LDAP_NETWORK_TIMEOUT="${TENET_LDAP_NETWORK_TIMEOUT:-5}"
TENET_LDAP_OPT_TIMEOUT="${TENET_LDAP_OPT_TIMEOUT:-5}"
TENET_LDAP_SEARCH_TIMEOUT="${TENET_LDAP_SEARCH_TIMEOUT:-10}"
TENET_LDAP_CACHE_CREDENTIALS="${TENET_LDAP_CACHE_CREDENTIALS:-0}"
TENET_LDAP_ENUMERATE="${TENET_LDAP_ENUMERATE:-0}"
TENET_LDAP_BIND_DN="${TENET_LDAP_BIND_DN:-}"
TENET_LDAP_BIND_PASSWORD="${TENET_LDAP_BIND_PASSWORD:-}"
TENET_LDAP_ACCESS_FILTER="${TENET_LDAP_ACCESS_FILTER:-}"
TENET_LDAP_TEST_USER="${TENET_LDAP_TEST_USER:-}"
TENET_ALLOW_USERS="${TENET_ALLOW_USERS:-}"
TENET_ALLOW_GROUPS="${TENET_ALLOW_GROUPS:-}"

if [ -n "$TENET_LDAP_ACCESS_FILTER" ]; then
    sssd_access_provider="ldap"
else
    sssd_access_provider="permit"
fi

auth_mode="$(printf '%s' "$TENET_AUTH_MODE" | tr '[:upper:]' '[:lower:]')"
case "$auth_mode" in
    local|ldap)
        ;;
    *)
        echo "TENET_AUTH_MODE must be 'local' or 'ldap'" >&2
        exit 1
        ;;
esac

bool_value() {
    case "$(printf '%s' "$1" | tr '[:upper:]' '[:lower:]')" in
        1|yes|true|on) printf 'true' ;;
        *) printf 'false' ;;
    esac
}

normalize_words() {
    printf '%s' "$*" | tr '\n\t' '  ' | tr -s ' ' | sed 's/^ *//;s/ *$//'
}

configure_nss_sss() {
    for database in passwd group shadow; do
        if grep -q "^${database}:" /etc/nsswitch.conf; then
            if ! grep "^${database}:" /etc/nsswitch.conf | grep -qw sss; then
                sed -i "s/^${database}:.*/& sss/" /etc/nsswitch.conf
            fi
        else
            printf '%s: files sss\n' "$database" >> /etc/nsswitch.conf
        fi
    done
}

write_sssd_config() {
    mkdir -p /etc/sssd /var/lib/sss/db /var/lib/sss/pipes /var/log/sssd /run/sssd
    cat > /etc/sssd/sssd.conf <<EOF_SSSD
[sssd]
config_file_version = 2
services = nss, pam
domains = ${TENET_LDAP_DOMAIN}

[nss]
fallback_homedir = /home/%u
default_shell = /bin/sh

[pam]

[domain/${TENET_LDAP_DOMAIN}]
id_provider = ldap
auth_provider = ldap
chpass_provider = ldap
access_provider = ${sssd_access_provider}
sudo_provider = none
autofs_provider = none
ldap_uri = ${TENET_LDAP_URI}
ldap_search_base = ${TENET_LDAP_BASE_DN}
ldap_schema = ${TENET_LDAP_SCHEMA}
ldap_user_name = ${TENET_LDAP_USER_NAME_ATTR}
ldap_user_gecos = ${TENET_LDAP_USER_FULLNAME_ATTR}
ldap_user_object_class = ${TENET_LDAP_USER_OBJECT_CLASS}
ldap_group_object_class = ${TENET_LDAP_GROUP_OBJECT_CLASS}
ldap_id_mapping = $(bool_value "$TENET_LDAP_ID_MAPPING")
ldap_referrals = $(bool_value "$TENET_LDAP_REFERRALS")
ldap_network_timeout = ${TENET_LDAP_NETWORK_TIMEOUT}
ldap_opt_timeout = ${TENET_LDAP_OPT_TIMEOUT}
ldap_search_timeout = ${TENET_LDAP_SEARCH_TIMEOUT}
ldap_id_use_start_tls = $(bool_value "$TENET_LDAP_START_TLS")
ldap_tls_reqcert = ${TENET_LDAP_TLS_REQCERT}
cache_credentials = $(bool_value "$TENET_LDAP_CACHE_CREDENTIALS")
enumerate = $(bool_value "$TENET_LDAP_ENUMERATE")
use_fully_qualified_names = false
case_sensitive = false
fallback_homedir = /home/%u
default_shell = /bin/sh
override_shell = /bin/sh
EOF_SSSD

    if [ -n "$TENET_LDAP_BIND_DN" ]; then
        {
            printf 'ldap_default_bind_dn = %s\n' "$TENET_LDAP_BIND_DN"
            printf 'ldap_default_authtok_type = password\n'
            printf 'ldap_default_authtok = %s\n' "$TENET_LDAP_BIND_PASSWORD"
        } >> /etc/sssd/sssd.conf
    fi

    if [ -n "$TENET_LDAP_ACCESS_FILTER" ]; then
        printf 'ldap_access_filter = %s\n' "$TENET_LDAP_ACCESS_FILTER" >> /etc/sssd/sssd.conf
    fi

    chmod 0600 /etc/sssd/sssd.conf
}

write_ldap_pam_config() {
    cat > /etc/pam.d/sshd <<'EOF_PAM'
#%PAM-1.0
auth required pam_env.so
auth sufficient pam_unix.so
auth sufficient pam_sss.so forward_pass
auth required pam_deny.so

account sufficient pam_unix.so
account sufficient pam_sss.so
account required pam_deny.so

session required pam_limits.so
session required pam_env.so
session optional pam_mkhomedir.so skel=/etc/skel umask=077
session required pam_permit.so

password sufficient pam_unix.so yescrypt
password sufficient pam_sss.so use_authtok
password required pam_deny.so
EOF_PAM
}

start_sssd() {
    if [ -z "$TENET_LDAP_BIND_DN" ]; then
        echo "TENET LDAP warning: TENET_LDAP_BIND_DN is empty. This LDAP server may reject anonymous searches; set TENET_LDAP_BIND_DN and TENET_LDAP_BIND_PASSWORD if login hangs." >&2
    fi

    /usr/sbin/sssd -i &
    sssd_pid=$!

    index=0
    while [ "$index" -lt 20 ]; do
        if ! kill -0 "$sssd_pid" 2>/dev/null; then
            echo "sssd exited before sshd startup" >&2
            wait "$sssd_pid" 2>/dev/null || true
            exit 1
        fi
        if [ -S /var/lib/sss/pipes/nss ] || [ -S /var/lib/sss/pipes/private/pam ]; then
            break
        fi
        sleep 1
        index=$((index + 1))
    done

    if [ -n "$TENET_LDAP_TEST_USER" ]; then
        getent passwd "$TENET_LDAP_TEST_USER" >/dev/null
    fi
}

ensure_local_user() {
    user="$1"

    [ -n "$user" ] || return 0
    if awk -F: -v u="$user" '$1 == u { found = 1 } END { exit !found }' /etc/passwd; then
        :
    elif getent passwd "$user" >/dev/null 2>&1; then
        echo "TENET local gateway: $user exists in NSS; not creating local fallback user" >&2
        return 0
    else
        adduser --disabled-password --gecos "tenet chat user" --shell /bin/sh "$user" >/dev/null
    fi

    echo "$user:$TENET_PASSWORD" | chpasswd

    user_home="$(getent passwd "$user" | cut -d: -f6)"
    mkdir -p "$user_home/.ssh"
    chmod 0700 "$user_home/.ssh"

    if [ "$user" = "$TENET_USER" ]; then
        if [ -n "$TENET_AUTHORIZED_KEYS" ]; then
            printf '%s\n' "$TENET_AUTHORIZED_KEYS" > "$user_home/.ssh/authorized_keys"
        elif [ -f "$TENET_AUTHORIZED_KEYS_FILE" ]; then
            cp "$TENET_AUTHORIZED_KEYS_FILE" "$user_home/.ssh/authorized_keys"
        fi
    fi

    if [ -f "$user_home/.ssh/authorized_keys" ]; then
        chmod 0600 "$user_home/.ssh/authorized_keys"
    fi
    chown -R "$user:$user" "$user_home/.ssh"
}

ensure_local_gateway_users() {
    for user in $TENET_LOCAL_GATEWAY_USERS; do
        ensure_local_user "$user"
    done
}

mkdir -p /run/sshd /run/tenet /run/sssd /etc/tenet /var/lib/tenet/ssh

sssd_pid=""

if [ "$auth_mode" = "local" ]; then
    ensure_local_user "$TENET_USER"
else
    configure_nss_sss
    write_sssd_config
    write_ldap_pam_config
    start_sssd
    if [ "$(bool_value "$TENET_ENABLE_LOCAL_GATEWAY")" = "true" ]; then
        ensure_local_gateway_users
    fi
fi
if [ ! -f /var/lib/tenet/ssh/ssh_host_ed25519_key ]; then
    ssh-keygen -t ed25519 -N '' -f /var/lib/tenet/ssh/ssh_host_ed25519_key >/dev/null
fi
if [ ! -f /var/lib/tenet/ssh/ssh_host_rsa_key ]; then
    ssh-keygen -t rsa -b 3072 -N '' -f /var/lib/tenet/ssh/ssh_host_rsa_key >/dev/null
fi

if [ "$TENET_DISABLE_PASSWORD_AUTH" = "1" ]; then
    password_auth="no"
else
    password_auth="yes"
fi

if [ "$auth_mode" = "ldap" ]; then
    mkdir -p "$TENET_AUTHORIZED_KEYS_DIR"
    use_pam="yes"
    keyboard_auth="$password_auth"
    allow_users_line=""
    allow_groups_line=""
    authorized_keys_line="AuthorizedKeysFile .ssh/authorized_keys ${TENET_AUTHORIZED_KEYS_DIR}/%u"
    if [ -n "$TENET_ALLOW_USERS" ]; then
        effective_allow_users="$TENET_ALLOW_USERS"
        if [ "$(bool_value "$TENET_ENABLE_LOCAL_GATEWAY")" = "true" ]; then
            effective_allow_users="$(normalize_words "$effective_allow_users $TENET_LOCAL_GATEWAY_USERS")"
        fi
        allow_users_line="AllowUsers ${effective_allow_users}"
    fi
    if [ -n "$TENET_ALLOW_GROUPS" ]; then
        allow_groups_line="AllowGroups ${TENET_ALLOW_GROUPS}"
    fi
else
    use_pam="no"
    keyboard_auth="no"
    allow_users_line="AllowUsers ${TENET_USER}"
    allow_groups_line=""
    authorized_keys_line=""
fi

cat > /etc/ssh/sshd_config <<EOF_SSHD
Port ${TENET_SSH_PORT}
ListenAddress 0.0.0.0
HostKey /var/lib/tenet/ssh/ssh_host_ed25519_key
HostKey /var/lib/tenet/ssh/ssh_host_rsa_key
PermitRootLogin no
PermitEmptyPasswords no
PasswordAuthentication ${password_auth}
PubkeyAuthentication yes
${authorized_keys_line}
KbdInteractiveAuthentication ${keyboard_auth}
ChallengeResponseAuthentication ${keyboard_auth}
UsePAM ${use_pam}
${allow_users_line}
${allow_groups_line}
PermitTTY yes
X11Forwarding no
AllowTcpForwarding no
AllowAgentForwarding no
PermitTunnel no
PermitUserEnvironment no
ClientAliveInterval 60
ClientAliveCountMax 3
AcceptEnv LANG LC_*
ForceCommand /usr/local/bin/tenet-ssh-command --socket ${TENET_SOCKET} --copy-id ${TENET_ENABLE_SSH_COPY_ID}
Subsystem sftp internal-sftp
EOF_SSHD

/usr/sbin/sshd -t -f /etc/ssh/sshd_config

/usr/local/bin/tenet --ssh-backend --socket "$TENET_SOCKET" --max-clients "$TENET_MAX_CLIENTS" $TENET_BACKEND_ARGS &
tenet_pid=$!

/usr/sbin/sshd -D -e -f /etc/ssh/sshd_config &
sshd_pid=$!

term_handler() {
    if [ -n "$sssd_pid" ]; then
        kill "$sshd_pid" "$tenet_pid" "$sssd_pid" 2>/dev/null || true
    else
        kill "$sshd_pid" "$tenet_pid" 2>/dev/null || true
    fi
    wait "$sshd_pid" 2>/dev/null || true
    wait "$tenet_pid" 2>/dev/null || true
    if [ -n "$sssd_pid" ]; then
        wait "$sssd_pid" 2>/dev/null || true
    fi
}
trap term_handler INT TERM

wait "$sshd_pid"
status=$?
term_handler
exit "$status"
