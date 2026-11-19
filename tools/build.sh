#!/bin/sh
set -eu

. "$(dirname -- "$0")/common.sh"
build_dir="$project/build"
mkdir -p "$build_dir/fcitx"
cxx=${CXX:-g++}
win_cxx=${MINGW_CXX:-x86_64-w64-mingw32-g++-posix}
"$win_cxx" -std=c++17 -Wall -Wextra -Werror -O2 -static -municode \
    "$project/src/engine-host.cpp" -o "$build_dir/engine-host.exe.new" -lole32 -loleaut32 -luuid

if [ -n "${FCITX_INCLUDE_ROOT:-}" ]; then
    "$cxx" -std=c++17 -Wall -Wextra -Werror -O2 -fPIC -shared \
        -isystem "$FCITX_INCLUDE_ROOT/Core" -isystem "$FCITX_INCLUDE_ROOT/Utils" -isystem "$FCITX_INCLUDE_ROOT/Config" \
        "$project/src/fcitx-addon.cpp" -o "$build_dir/fcitx/microsoft-chinese-keyboard.so.new" \
        -l:libFcitx5Core.so.7 -l:libFcitx5Utils.so.2 -l:libFcitx5Config.so.6
else
    # pkg-config supplies the flags for the Fcitx5 development packages installed on this machine.
    "$cxx" -std=c++17 -Wall -Wextra -Werror -O2 -fPIC -shared \
        $(pkg-config --cflags Fcitx5Core Fcitx5Config Fcitx5Utils) \
        "$project/src/fcitx-addon.cpp" -o "$build_dir/fcitx/microsoft-chinese-keyboard.so.new" \
        $(pkg-config --libs Fcitx5Core Fcitx5Config Fcitx5Utils)
fi
mv -f "$build_dir/engine-host.exe.new" "$build_dir/engine-host.exe"
mv -f "$build_dir/fcitx/microsoft-chinese-keyboard.so.new" "$build_dir/fcitx/microsoft-chinese-keyboard.so"
