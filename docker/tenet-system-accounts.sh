#!/bin/sh
set -eu

TENET_PERSIST_SYSTEM_ACCOUNTS="${TENET_PERSIST_SYSTEM_ACCOUNTS:-1}"
TENET_SYSTEM_ACCOUNT_DIR="${TENET_SYSTEM_ACCOUNT_DIR:-/var/lib/tenet/system-accounts}"
TENET_SYSTEM_ACCOUNT_MIN_ID="${TENET_SYSTEM_ACCOUNT_MIN_ID:-1000}"
TENET_SYSTEM_ACCOUNT_SYNC_INTERVAL="${TENET_SYSTEM_ACCOUNT_SYNC_INTERVAL:-10}"

account_files="passwd group shadow gshadow"

is_enabled() {
    case "$(printf '%s' "$1" | tr '[:upper:]' '[:lower:]')" in
        1|yes|true|on) return 0 ;;
        *) return 1 ;;
    esac
}

ensure_account_dir() {
    if [ -z "$TENET_SYSTEM_ACCOUNT_DIR" ] || [ "$TENET_SYSTEM_ACCOUNT_DIR" = "/" ]; then
        echo "tenet-system-accounts: unsafe TENET_SYSTEM_ACCOUNT_DIR" >&2
        exit 1
    fi
    mkdir -p "$TENET_SYSTEM_ACCOUNT_DIR"
    chmod 0700 "$TENET_SYSTEM_ACCOUNT_DIR"
}

copy_account_file() {
    source_file="$1"
    target_file="$2"
    temp_file="${target_file}.tmp.$$"

    rm -f "$temp_file"
    cp -p "$source_file" "$temp_file"
    mv "$temp_file" "$target_file"
}

seed_saved_files() {
    for account_file in $account_files; do
        if [ -f "/etc/$account_file" ] && [ ! -f "$TENET_SYSTEM_ACCOUNT_DIR/$account_file" ]; then
            copy_account_file "/etc/$account_file" "$TENET_SYSTEM_ACCOUNT_DIR/$account_file"
        fi
    done
}

save_accounts() {
    ensure_account_dir
    for account_file in $account_files; do
        if [ -f "/etc/$account_file" ]; then
            copy_account_file "/etc/$account_file" "$TENET_SYSTEM_ACCOUNT_DIR/$account_file"
        fi
    done
}

merge_passwd() {
    current_file="/etc/passwd"
    saved_file="$TENET_SYSTEM_ACCOUNT_DIR/passwd"
    output_file="$1"
    users_file="$2"

    cp -p "$current_file" "$output_file"
    : > "$users_file"
    awk -F: -v min_id="$TENET_SYSTEM_ACCOUNT_MIN_ID" -v users_file="$users_file" '
        FNR == NR { seen[$1] = 1; next }
        NF >= 7 && $3 ~ /^[0-9]+$/ && $3 >= min_id && !($1 in seen) {
            print $1 >> users_file
            print
        }
    ' "$current_file" "$saved_file" >> "$output_file"
}

merge_group() {
    current_file="/etc/group"
    saved_file="$TENET_SYSTEM_ACCOUNT_DIR/group"
    output_file="$1"
    groups_file="$2"

    cp -p "$current_file" "$output_file"
    : > "$groups_file"
    awk -F: -v min_id="$TENET_SYSTEM_ACCOUNT_MIN_ID" -v groups_file="$groups_file" '
        FNR == NR { seen[$1] = 1; next }
        NF >= 4 && $3 ~ /^[0-9]+$/ && $3 >= min_id && !($1 in seen) {
            print $1 >> groups_file
            print
        }
    ' "$current_file" "$saved_file" >> "$output_file"
}

merge_named_file() {
    current_file="$1"
    saved_file="$2"
    names_file="$3"
    output_file="$4"

    cp -p "$current_file" "$output_file"
    awk -F: '
        FILENAME == ARGV[1] { wanted[$1] = 1; next }
        FILENAME == ARGV[2] { seen[$1] = 1; next }
        ($1 in wanted) && !($1 in seen) { print }
    ' "$names_file" "$current_file" "$saved_file" >> "$output_file"
}

restore_accounts() {
    ensure_account_dir
    seed_saved_files

    temp_dir="$(mktemp -d)"
    trap 'rm -rf "$temp_dir"' EXIT HUP INT TERM

    if [ -f /etc/passwd ] && [ -f "$TENET_SYSTEM_ACCOUNT_DIR/passwd" ]; then
        merge_passwd "$temp_dir/passwd" "$temp_dir/users"
        mv "$temp_dir/passwd" /etc/passwd
    else
        : > "$temp_dir/users"
    fi

    if [ -f /etc/shadow ] && [ -f "$TENET_SYSTEM_ACCOUNT_DIR/shadow" ]; then
        merge_named_file /etc/shadow "$TENET_SYSTEM_ACCOUNT_DIR/shadow" "$temp_dir/users" "$temp_dir/shadow"
        mv "$temp_dir/shadow" /etc/shadow
    fi

    if [ -f /etc/group ] && [ -f "$TENET_SYSTEM_ACCOUNT_DIR/group" ]; then
        merge_group "$temp_dir/group" "$temp_dir/groups"
        mv "$temp_dir/group" /etc/group
    else
        : > "$temp_dir/groups"
    fi

    if [ -f /etc/gshadow ] && [ -f "$TENET_SYSTEM_ACCOUNT_DIR/gshadow" ]; then
        merge_named_file /etc/gshadow "$TENET_SYSTEM_ACCOUNT_DIR/gshadow" "$temp_dir/groups" "$temp_dir/gshadow"
        mv "$temp_dir/gshadow" /etc/gshadow
    fi

    save_accounts
}

watch_accounts() {
    while :; do
        sleep "$TENET_SYSTEM_ACCOUNT_SYNC_INTERVAL"
        save_accounts || true
    done
}

if ! is_enabled "$TENET_PERSIST_SYSTEM_ACCOUNTS"; then
    exit 0
fi

case "${1:-save}" in
    restore)
        restore_accounts
        ;;
    save)
        save_accounts
        ;;
    watch)
        watch_accounts
        ;;
    *)
        echo "usage: tenet-system-accounts [restore|save|watch]" >&2
        exit 2
        ;;
esac
