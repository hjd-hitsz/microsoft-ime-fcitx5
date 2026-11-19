verify_digest() {
    if ! printf '%s  %s\n' "$2" "$1" | sha256sum --check --status; then
        printf 'SHA-256 verification failed for %s\n' "$1" >&2
        return 1
    fi
}

download_verified() (
    file=$1
    url=$2
    digest=$3
    if [ -f "$file" ]; then
        if verify_digest "$file" "$digest"; then exit; fi
    fi
    part=$(mktemp "$file.XXXXXX")
    trap 'rm -f -- "$part"' EXIT
    printf 'Downloading %s\n' "$url" >&2
    curl --fail --silent --show-error --location --proto '=https' --proto-redir '=https' \
        --connect-timeout 15 --max-time 600 --retry 2 --output "$part" "$url"
    verify_digest "$part" "$digest"
    mv -f -- "$part" "$file"
)
