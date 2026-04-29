#!/bin/sh
set -eu

TENET_SOCKET="${TENET_SOCKET:-/run/tenet/tenet.sock}"
TENET_ENABLE_SSH_COPY_ID="${TENET_ENABLE_SSH_COPY_ID:-1}"

while [ "$#" -gt 0 ]; do
    case "$1" in
        --socket)
            TENET_SOCKET="$2"
            shift 2
            ;;
        --copy-id)
            TENET_ENABLE_SSH_COPY_ID="$2"
            shift 2
            ;;
        *)
            shift
            ;;
    esac
done

is_enabled() {
    case "$(printf '%s' "$1" | tr '[:upper:]' '[:lower:]')" in
        1|yes|true|on) return 0 ;;
        *) return 1 ;;
    esac
}

is_ssh_copy_id_command() {
    case "${SSH_ORIGINAL_COMMAND:-}" in
        *authorized_keys*) return 0 ;;
        *) return 1 ;;
    esac
}

is_noop_probe_command() {
    case "${SSH_ORIGINAL_COMMAND:-}" in
        exit|"exit 0"|true|"/bin/true"|:) return 0 ;;
        *) return 1 ;;
    esac
}

is_public_key_line() {
    case "$1" in
        ssh-rsa\ *|rsa-sha2-256\ *|rsa-sha2-512\ *) return 0 ;;
        ssh-ed25519\ *|ssh-ed448\ *) return 0 ;;
        ecdsa-sha2-nistp256\ *|ecdsa-sha2-nistp384\ *|ecdsa-sha2-nistp521\ *) return 0 ;;
        sk-ssh-ed25519@openssh.com\ *|sk-ecdsa-sha2-nistp256@openssh.com\ *) return 0 ;;
        *) return 1 ;;
    esac
}

install_public_keys() {
    user="$(id -un)"
    home="$(getent passwd "$user" | cut -d: -f6)"
    added=0
    seen=0

    if [ -z "$home" ] || [ ! -d "$home" ]; then
        echo "tenet: cannot find home directory for $user" >&2
        exit 1
    fi

    umask 077
    mkdir -p "$home/.ssh"
    touch "$home/.ssh/authorized_keys"
    chmod 0700 "$home/.ssh"
    chmod 0600 "$home/.ssh/authorized_keys"

    while IFS= read -r key_line || [ -n "$key_line" ]; do
        key_line="$(printf '%s' "$key_line" | tr -d '\r')"
        [ -n "$key_line" ] || continue
        seen=$((seen + 1))

        if ! is_public_key_line "$key_line"; then
            echo "tenet: ignored unsupported public key line" >&2
            continue
        fi
        if grep -qxF "$key_line" "$home/.ssh/authorized_keys" 2>/dev/null; then
            continue
        fi
        printf '%s\n' "$key_line" >> "$home/.ssh/authorized_keys"
        added=$((added + 1))
    done

    if [ "$seen" -eq 0 ]; then
        echo "tenet: no public keys received" >&2
        exit 1
    fi

    echo "tenet: added $added new public key(s) for $user"
}

if is_noop_probe_command; then
    exit 0
fi

if is_ssh_copy_id_command; then
    if ! is_enabled "$TENET_ENABLE_SSH_COPY_ID"; then
        echo "tenet: ssh-copy-id support is disabled" >&2
        exit 1
    fi
    install_public_keys
    exit 0
fi

if [ -n "${SSH_ORIGINAL_COMMAND:-}" ]; then
    echo "tenet: remote commands are disabled; connect without a command to enter chat" >&2
    exit 1
fi

exec /usr/local/bin/tenet --ssh-session --socket "$TENET_SOCKET"
