#!/bin/sh
set -eu

data=${XDG_DATA_HOME:-$HOME/.local/share}
config=${XDG_CONFIG_HOME:-$HOME/.config}
method=microsoft-chinese-keyboard
systemctl --user disable --now microsoft-ime-engine.socket
systemctl --user stop microsoft-ime-engine.service
rm -f "$config/systemd/user/microsoft-ime-engine.socket" "$config/systemd/user/microsoft-ime-engine.service" \
    "$data/fcitx5/addon/$method.conf" "$data/fcitx5/inputmethod/$method.conf" \
    "$HOME/.local/share/microsoft-ime-fcitx5/$method.so" \
    "$HOME/.local/share/microsoft-ime-fcitx5/start-engine.sh"
systemctl --user daemon-reload
