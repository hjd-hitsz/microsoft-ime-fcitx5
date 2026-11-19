#!/bin/sh
set -eu

. "$(dirname -- "$0")/common.sh"
. "$project/tools/releases.sh"
. "$project/tools/download.sh"
umask 077
mkdir -p "$cache" "$state"
exec 9>"$state/setup.lock"
flock 9
msi=${1:-$cache/$msi_name}
marker="$state/engine-version"
if [ "$#" -gt 0 ]; then
    verify_digest "$msi" "$msi_digest"
fi
sh "$project/tools/prepare-wine.sh"
if [ -f "$wine_home/prefix/user.reg" ] && [ -f "$marker" ] &&
    [ "$(cat "$marker")" = "$msi_digest" ]; then
    exit
fi
if [ "$#" -eq 0 ]; then download_verified "$msi" "$msi_url" "$msi_digest"; fi
scratch=$(mktemp -d "$cache/stage.XXXXXX")
trap 'rm -rf -- "$scratch"' EXIT
mkdir -p "$scratch/tables" "$scratch/extracted"
msiextract -C "$scratch/extracted" "$msi" >/dev/null
for file in File Registry; do
    msiinfo export "$msi" "$file" >"$scratch/tables/$file.tsv"
done
"$node_binary" "$project/tools/stage-engine.mjs" "$wine_home/prefix" \
    "$scratch/extracted/BingKeyboard" "$scratch/tables" "$state/engine.reg"

sh "$project/tools/wine-run.sh" /bin/sh -c '
    set -e
    trap "wineserver -k >/dev/null 2>&1 || true" EXIT
    export WINEDEBUG=-all
    timeout 90 wine reg import "$(timeout 75 wine winepath.exe -w "$1")"
    key="HKCU\Software\Microsoft\BingKeyboard\CHS"
    for name in EnableSelfLearning EnableSmartSelfLearning; do
        if ! timeout 15 wine reg query "$key" /v "$name" >/dev/null 2>&1; then
            timeout 15 wine reg add "$key" /v "$name" /t REG_DWORD /d 1 /f
        fi
    done
    timeout 15 wineserver -w
' sh "$state/engine.reg"
printf '%s\n' "$msi_digest" >"$marker"
