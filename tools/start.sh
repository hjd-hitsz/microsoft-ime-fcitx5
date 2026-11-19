#!/bin/sh
set -eu

. "$(dirname -- "$0")/common.sh"
socket=${MS_IME_SOCKET:-${XDG_RUNTIME_DIR:?}/microsoft-ime-engine.sock}
unset IME_WINE_ONLINE_PROBE
/bin/sh "$project/tools/prepare.sh"
exec "$node_binary" "$project/src/wine-relay.mjs" "$socket" /bin/sh "$project/tools/wine-run.sh" /bin/sh -c '
    trap "wineserver -k >/dev/null 2>&1 || true" EXIT
    export WINEDEBUG=-all
    wine engine-host.exe --interactive-with-learning
'
