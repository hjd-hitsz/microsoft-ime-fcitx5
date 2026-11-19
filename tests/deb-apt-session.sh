#!/bin/sh
set -eu

if [ "$#" -ne 3 ] || [ "$(id -u)" -ne 0 ] || ! systemd-detect-virt --container --quiet; then
    printf '%s\n' 'Usage in a disposable systemd container as root: deb-apt-session.sh TEST_USER PREVIOUS_DEB CURRENT_DEB' >&2
    exit 2
fi

test_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
account=$1
previous=$(realpath "$2")
current=$(realpath "$3")
uid=$(id -u "$account")
test "$uid" -ne 0
home=$(getent passwd "$account" | cut -d: -f6)
runtime="/run/user/$uid"
manager="user@$uid.service"
socket=microsoft-ime-engine.socket
service=microsoft-ime-engine.service
socket_path="$runtime/microsoft-ime-engine.sock"
state="$home/.local/state/microsoft-ime-fcitx5"
method=microsoft-ime-fcitx5
version=$(dpkg-deb -f "$current" Version)
export DEBIAN_FRONTEND=noninteractive

userctl() {
    systemctl --user --machine "$uid@" "$@"
}

trap 'status=$?
if [ "$status" -ne 0 ]; then
    userctl --no-pager status "$socket" "$service" >&2 || true
    journalctl -b "_SYSTEMD_USER_UNIT=$service" -n 30 --no-pager >&2 || true
fi
exit "$status"' EXIT

install_package() {
    apt-get -o 'Dpkg::Options::=--path-include=/usr/share/doc/microsoft-ime-fcitx5' \
        -o 'Dpkg::Options::=--path-include=/usr/share/doc/microsoft-ime-fcitx5/*' \
        install -y --no-install-recommends "$1"
}

gtk_session() {
    runuser -u "$account" -- env \
        IME_TEST_INSTALLED=1 IME_TEST_SOCKET="$socket_path" IME_SETTINGS_PORT=17865 \
        "$@" "$test_dir/fcitx-session.sh" zwsrf 中文输入法
}

verify_activation() {
    userctl is-enabled "$socket"
    userctl is-active "$socket"
    test -S "$socket_path"
    if userctl is-active --quiet "$service"; then
        printf '%s\n' 'The engine started before an input method connected.' >&2
        exit 1
    fi
    gtk_session IME_TEST_WAIT_FOR_READY=1 IME_TEST_POPUP=1
    userctl is-active "$service"
}

verify_installed_dependencies() {
    wine_version=$(wine --version)
    wine_version=${wine_version#wine-}
    test "${wine_version%%.*}" -ge 11
    . /usr/lib/microsoft-ime-fcitx5/tools/releases.sh
    runuser -u "$account" -- test -r "/var/lib/$method/$msi_name"
    test -s "/usr/share/doc/$method/copyright"
}

verify_purge() {
    apt-get purge -y "$method"
    if dpkg-query -W "$method" >/dev/null 2>&1; then exit 1; fi
    if userctl is-active --quiet "$socket" || userctl is-active --quiet "$service"; then exit 1; fi
    test ! -e "/etc/systemd/user/sockets.target.wants/$socket"
    test ! -e "/etc/systemd/user/$socket"
    test ! -S "$socket_path"
    test ! -e "/var/lib/$method/$msi_name"
    test -f "$state/wine/prefix/user.reg"
    test -f "$state/test-user-data"
}

systemctl is-active "$manager"
userctl is-active sockets.target
if dpkg-query -W "$method" >/dev/null 2>&1; then
    printf '%s\n' 'The test container must start with the input method uninstalled.' >&2
    exit 1
fi
test ! -e "$state"

printf '%s\n' 'Install the previous package with a running user manager'
install_package "$previous"
userctl show "$socket" -p UnitFileState -p ActiveState

printf '%s\n' 'Upgrade through APT and verify activation without manual startup'
install_package "$current"
test "$(dpkg-query -W -f='${Version}' "$method")" = "$version"
test -z "$(dpkg --verify "$method")"
verify_installed_dependencies
verify_activation
runuser -u "$account" -- touch "$state/test-user-data"

printf '%s\n' 'Restart the systemd user manager and verify automatic socket startup'
systemctl restart "$manager"
verify_activation

printf '%s\n' 'Purge the package and verify that personalization survives'
verify_purge
runuser -u "$account" -- mv "$state" "$state-upgrade"

printf '%s\n' 'Install the current package with no prior engine state'
install_package "$current"
test ! -e "$state"
verify_installed_dependencies
verify_activation
runuser -u "$account" -- touch "$state/test-user-data"
verify_purge

printf '%s\n' 'APT installation, upgrade, activation, GTK input, session restart and purge passed.'
