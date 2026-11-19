#!/bin/sh
set -eu

project=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cd "$project"
if [ "${1:-}" != --inside ]; then
    exec dbus-run-session -- xvfb-run -a sh "$0" --inside "$@"
fi
shift
if [ "$#" -lt 2 ]; then
    printf '%s\n' 'Usage: tests/fcitx-session.sh PINYIN EXPECTED_TEXT [SELECTION_KEYS...]' >&2
    exit 2
fi
input=$1
expected=$2
shift 2
if [ "$#" -eq 0 ]; then set -- space; fi
scratch=$(mktemp -d)
relay_pid=
fcitx_pid=
probe_pid=
trap 'for pid in "$probe_pid" "$fcitx_pid" "$relay_pid"; do
    if [ -n "$pid" ]; then kill "$pid" 2>/dev/null || true; fi
done
if [ -n "$relay_pid" ]; then wait "$relay_pid" || true; fi
if [ -n "${IME_TEST_KEEP_LOGS:-}" ]; then
    mkdir -p "$IME_TEST_KEEP_LOGS"
    cp "$scratch/"*.log "$scratch"/input.txt "$IME_TEST_KEEP_LOGS/"
fi
rm -rf -- "$scratch"' EXIT
export IME_CACHE_DIR=${IME_CACHE_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/microsoft-ime-fcitx5}
export XDG_CONFIG_HOME="$scratch/config" XDG_DATA_HOME="$scratch/data"
export XDG_RUNTIME_DIR="$scratch/runtime" XDG_CACHE_HOME="$scratch/cache"
export GTK_IM_MODULE=fcitx QT_IM_MODULE=fcitx XMODIFIERS=@im=fcitx GIO_USE_VFS=local NO_AT_BRIDGE=1
export MS_IME_SOCKET="${IME_TEST_SOCKET:-$XDG_RUNTIME_DIR/ime.sock}"
export IME_SETTINGS_PORT=${IME_SETTINGS_PORT:-17866}
method=microsoft-chinese-keyboard
mkdir -p "$XDG_RUNTIME_DIR" "$XDG_CONFIG_HOME/fcitx5/conf" "$XDG_DATA_HOME/fcitx5/addon" "$XDG_DATA_HOME/fcitx5/inputmethod"
chmod 700 "$XDG_RUNTIME_DIR"
start="$project/tools/start.sh"
if [ "${IME_TEST_INSTALLED:-0}" = 1 ]; then
    start=/usr/lib/microsoft-ime-fcitx5/tools/start.sh
else
    export FCITX_ADDON_DIRS="$project/build/fcitx:/usr/lib/$(gcc -print-multiarch)/fcitx5"
    cp packaging/fcitx/addon.conf "$XDG_DATA_HOME/fcitx5/addon/$method.conf"
    cp packaging/fcitx/inputmethod.conf "$XDG_DATA_HOME/fcitx5/inputmethod/$method.conf"
fi
if [ -n "${IME_TEST_OPTIONS:-}" ]; then
    printf '%b\n' "$IME_TEST_OPTIONS" >"$XDG_CONFIG_HOME/fcitx5/conf/$method.conf"
fi
cat >"$XDG_CONFIG_HOME/fcitx5/profile" <<PROFILE
[Groups/0]
Name=Default
Default Layout=us
DefaultIM=$method

[Groups/0/Items/0]
Name=keyboard-us
Layout=

[Groups/0/Items/1]
Name=$method
Layout=

[GroupOrder]
0=Default
PROFILE

if [ -z "${IME_TEST_SOCKET:-}" ]; then
    sh "$start" >"$scratch/engine.log" 2>&1 &
    relay_pid=$!
    for index in $(seq 1 60); do
        if [ -S "$MS_IME_SOCKET" ]; then break; fi
        if ! kill -0 "$relay_pid" 2>/dev/null; then cat "$scratch/engine.log"; exit 1; fi
        sleep .1
    done
fi
test -S "$MS_IME_SOCKET"
fcitx5 -D --disable=notificationitem,notifications --verbose='key_trace=5' >"$scratch/fcitx.log" 2>&1 &
fcitx_pid=$!
for index in $(seq 1 60); do
    if busctl --user call org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus \
        NameHasOwner s org.fcitx.Fcitx5 | grep -q '^b true$'; then break; fi
    sleep .1
done
output="$scratch/input.txt"
: >"$output"
/usr/bin/python3 tests/gtk-input-probe.py "$output" >"$scratch/gtk.log" 2>&1 &
probe_pid=$!
window=$(timeout 10 xdotool search --sync --onlyvisible --name 'Fcitx5 input probe' | head -1)
xdotool windowfocus --sync "$window"
fcitx5-remote -s "$method"
fcitx5-remote -o
sleep .2
if [ "${IME_TEST_ACTIVATE_ONLY:-0}" = 1 ] || [ "${IME_TEST_WAIT_FOR_READY:-0}" = 1 ]; then
    ready=false
    for index in $(seq 1 90); do
        if settings=$(curl -fsS --max-time 2 "http://127.0.0.1:$IME_SETTINGS_PORT/api/settings" 2>/dev/null); then
            printf '%s\n' "$settings"
            ready=true
            break
        fi
        sleep 1
    done
    if ! "$ready"; then
        printf '%s\n' 'The settings server did not become ready.' >&2
        exit 1
    fi
    if [ "${IME_TEST_ACTIVATE_ONLY:-0}" = 1 ]; then exit; fi
fi
if [ -n "${IME_TEST_WEB_OPTIONS:-}" ]; then
    curl -fsS --max-time 15 -X PUT -H 'Content-Type: application/json' \
        --data "$IME_TEST_WEB_OPTIONS" "http://127.0.0.1:$IME_SETTINGS_PORT/api/settings" >/dev/null
fi
if [ -n "${IME_TEST_INITIAL_KEYS:-}" ]; then
    # Each word is one xdotool key chord.
    xdotool key $IME_TEST_INITIAL_KEYS
fi
popup_name='^(Fcitx5 Input Window|gtk-input-probe[.]py)$'
if [ "${IME_TEST_POPUP:-0}" = 1 ] && xdotool search --onlyvisible --name "$popup_name" >/dev/null; then
    printf '%s\n' 'A candidate window is already visible before typing.' >&2
    exit 1
fi
if [ -n "$input" ]; then xdotool type --clearmodifiers --delay 50 "$input"; fi
if [ "${IME_TEST_POPUP:-0}" = 1 ]; then
    # Fcitx5 and the GTK frontend can each draw the candidate window.
    timeout 5 xdotool search --sync --onlyvisible --name "$popup_name" >/dev/null
    printf '%s\n' 'Fcitx5 candidate window is visible.'
fi
xdotool key "$@"
for index in $(seq 1 30); do
    actual=$(cat "$output")
    if [ "$actual" = "$expected" ]; then break; fi
    sleep .1
done
printf '%s -> %s\n' "$input" "$actual"
test "$actual" = "$expected"
