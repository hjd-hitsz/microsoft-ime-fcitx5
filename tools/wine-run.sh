#!/bin/sh
set -eu

. "$(dirname -- "$0")/common.sh"
umask 077
mkdir -p "$wine_home"
if [ "${1:-}" != --inside ]; then
    exec xvfb-run -a -s '-screen 0 640x480x24' /bin/sh "$0" --inside "$@"
fi
shift

wine_bin=${IME_WINE_BIN:-/usr/bin}
if [ ! -x "$wine_bin/wine" ] || [ ! -x "$wine_bin/wineserver" ]; then
    printf '%s\n' 'Wine was not found. Install Wine 11 or later with apt, or set IME_WINE_BIN.' >&2
    exit 1
fi

if [ "${IME_WINE_ONLINE_PROBE:-0}" = 1 ]; then
    set -- --ro-bind-try /run/systemd/resolve /run/systemd/resolve -- "$@"
else
    set -- --unshare-net -- "$@"
fi
exec bwrap --die-with-parent --unshare-pid \
    --ro-bind / / --dev /dev --proc /proc --tmpfs "$HOME" --tmpfs /tmp --tmpfs /run \
    --bind "$state" "$state" --ro-bind "$engine_dir" "$engine_dir" \
    --ro-bind "$wine_bin/.." "$wine_bin/.." \
    --ro-bind /tmp/.X11-unix /tmp/.X11-unix --ro-bind "$XAUTHORITY" "$XAUTHORITY" \
    --unsetenv DBUS_SESSION_BUS_ADDRESS --unsetenv XDG_RUNTIME_DIR \
    --unsetenv GTK_IM_MODULE --unsetenv QT_IM_MODULE --unsetenv XMODIFIERS \
    --setenv HOME "$wine_home" --setenv WINEARCH win64 \
    --setenv WINEPREFIX "$wine_home/prefix" --setenv PATH "$wine_bin:/usr/bin:/bin" \
    --setenv WINEDLLOVERRIDES "mscoree,mshtml=" --setenv LC_ALL zh_CN.UTF-8 \
    --chdir "$engine_dir" "$@"
