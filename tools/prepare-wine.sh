#!/bin/sh
set -eu

. "$(dirname -- "$0")/common.sh"
. "$project/tools/releases.sh"
. "$project/tools/download.sh"
umask 077

if [ -n "${IME_WINE_BIN:-}" ]; then
    test -x "$IME_WINE_BIN/wine"
    exit
fi
if [ -x "$cache/wine/bin/wine" ]; then exit; fi

mkdir -p "$cache"
(
    flock 9
    if [ -x "$cache/wine/bin/wine" ]; then exit; fi
    download_verified "$cache/$wine_name" "$wine_url" "$wine_digest"
    download_verified "$cache/$wine_common_name" "$wine_common_url" "$wine_common_digest"
    scratch=$(mktemp -d "$cache/wine.XXXXXX")
    trap 'rm -rf -- "$scratch"' EXIT
    dpkg-deb -x "$cache/$wine_name" "$scratch"
    dpkg-deb -x "$cache/$wine_common_name" "$scratch"
    test -x "$scratch/opt/wine-stable/bin/wine"
    mv -T -- "$scratch/opt/wine-stable" "$cache/wine"
) 9>"$cache/wine.lock"
