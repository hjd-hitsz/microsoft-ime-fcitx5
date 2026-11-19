# Paths shared by the build, engine preparation, and user service.
project=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cache=${IME_CACHE_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/microsoft-ime-fcitx5}
state=${IME_STATE_DIR:-${XDG_STATE_HOME:-$HOME/.local/state}/microsoft-ime-fcitx5}
wine_home="$state/wine"
engine_dir="$project/build"
node_binary=${IME_NODE_BINARY:-node}
if [ -x "$project/engine-host.exe" ]; then
    engine_dir="$project"
    node_binary="$project/node"
fi
