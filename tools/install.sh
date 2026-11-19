#!/bin/sh
set -eu

. "$(dirname -- "$0")/common.sh"
data=${XDG_DATA_HOME:-$HOME/.local/share}
config=${XDG_CONFIG_HOME:-$HOME/.config}
method=microsoft-chinese-keyboard
launcher="$HOME/.local/share/microsoft-ime-fcitx5"
build_dir="$project/build"
node_binary=$(command -v node)
quote() {
    printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"
}

test -x "$build_dir/engine-host.exe" || sh "$project/tools/build.sh"
sh "$project/tools/prepare.sh" "$@"

mkdir -p "$launcher" "$data/fcitx5/addon" "$data/fcitx5/inputmethod" "$config/systemd/user"
install -m 0755 "$build_dir/fcitx/$method.so" "$launcher/$method.so.new"
mv -f "$launcher/$method.so.new" "$launcher/$method.so"
sed "\|^Library=|cLibrary=$launcher/$method" "$project/packaging/fcitx/addon.conf" >"$data/fcitx5/addon/$method.conf"
install -m 0644 "$project/packaging/fcitx/inputmethod.conf" "$data/fcitx5/inputmethod/$method.conf"

{
    printf '%s\n' '#!/bin/sh' 'set -eu'
    printf 'export IME_NODE_BINARY=%s\n' "$(quote "$node_binary")"
    printf 'export IME_STATE_DIR=%s\n' "$(quote "$state")"
    printf 'export IME_CACHE_DIR=%s\n' "$(quote "$cache")"
    if [ -n "${IME_WINE_BIN:-}" ]; then printf 'export IME_WINE_BIN=%s\n' "$(quote "$IME_WINE_BIN")"; fi
    if [ -n "${IME_SETTINGS_PORT:-}" ]; then printf 'export IME_SETTINGS_PORT=%s\n' "$(quote "$IME_SETTINGS_PORT")"; fi
    printf 'exec /bin/sh %s\n' "$(quote "$project/tools/start.sh")"
} >"$launcher/start-engine.sh.new"
chmod 0755 "$launcher/start-engine.sh.new"
mv -f "$launcher/start-engine.sh.new" "$launcher/start-engine.sh"

install -m 0644 "$project/packaging/systemd/microsoft-ime-engine.socket" "$config/systemd/user/"
sed 's|^ExecStart=.*|ExecStart=/bin/sh "%h/.local/share/microsoft-ime-fcitx5/start-engine.sh"|' \
    "$project/packaging/systemd/microsoft-ime-engine.service" >"$config/systemd/user/microsoft-ime-engine.service"
systemctl --user daemon-reload
systemctl --user enable --now microsoft-ime-engine.socket
if systemctl --user is-active --quiet microsoft-ime-engine.service; then
    systemctl --user restart microsoft-ime-engine.service
fi
if ! busctl --user call org.fcitx.Fcitx5 /controller org.fcitx.Fcitx.Controller1 Refresh; then
    printf '%s\n' 'Fcitx5 will detect the addon when the session starts.'
fi
printf 'Local settings after activation: http://127.0.0.1:%s/\n' "${IME_SETTINGS_PORT:-17865}"
