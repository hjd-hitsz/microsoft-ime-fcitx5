#!/bin/sh
set -eu

. "$(dirname -- "$0")/common.sh"
. "$project/tools/releases.sh"
. "$project/tools/download.sh"
if [ "$(dpkg --print-architecture)" != amd64 ]; then
    printf '%s\n' 'This package requires an amd64 build host.' >&2
    exit 1
fi

umask 022
mkdir -p "$project/build" "$project/dist" "$cache"
sh "$project/tools/build.sh"
node_archive=${IME_NODE_ARCHIVE:-$cache/$node_release.tar.xz}
if [ -n "${IME_NODE_ARCHIVE:-}" ]; then
    verify_digest "$node_archive" "$node_digest"
else
    download_verified "$node_archive" "$node_url" "$node_digest"
fi
scratch=$(mktemp -d "$project/build/package.XXXXXX")
trap 'rm -rf -- "$scratch"' EXIT
root="$scratch/debian/microsoft-ime-fcitx5"
runtime="$root/usr/lib/microsoft-ime-fcitx5"
addon_dir="$root/usr/lib/$(dpkg-architecture -qDEB_HOST_MULTIARCH)/fcitx5"
share="$root/usr/share"
units="$root/usr/lib/systemd/user"
doc="$share/doc/microsoft-ime-fcitx5"
method=microsoft-chinese-keyboard
mkdir -p "$root/DEBIAN" "$runtime/src" "$runtime/tools" "$runtime/web" "$addon_dir" \
    "$share/fcitx5/addon" "$share/fcitx5/inputmethod" "$share/applications" \
    "$units" "$doc" "$root/usr/bin"
install -m 0755 "$project/build/engine-host.exe" "$runtime/"
install -m 0755 "$project/build/fcitx/$method.so" "$addon_dir/"
install -m 0644 "$project/src/"*.mjs "$runtime/src/"
install -m 0644 "$project/web/"* "$runtime/web/"
for script in common releases download prepare start wine-run; do
    install -m 0755 "$project/tools/$script.sh" "$runtime/tools/"
done
install -m 0644 "$project/tools/stage-engine.mjs" "$runtime/tools/"
install -m 0644 "$project/packaging/fcitx/addon.conf" "$share/fcitx5/addon/$method.conf"
install -m 0644 "$project/packaging/fcitx/inputmethod.conf" "$share/fcitx5/inputmethod/$method.conf"
install -m 0644 "$project/packaging/systemd/"* "$units/"
install -m 0644 "$project/packaging/desktop/"* "$share/applications/"
install -m 0755 "$project/packaging/deb/microsoft-ime-fcitx5" "$root/usr/bin/"
install -m 0644 "$project/README.md" "$doc/"
install -m 0644 "$project/LICENSE" "$doc/copyright"
ln -s copyright "$doc/LICENSE"
install -D -m 0644 "$project/docs/features.md" "$doc/docs/features.md"
tar -xJf "$node_archive" -C "$scratch" "$node_release/bin/node" "$node_release/LICENSE"
install -m 0755 "$scratch/$node_release/bin/node" "$runtime/"
install -m 0644 "$scratch/$node_release/LICENSE" "$doc/copyright.node"
printf '%s\n' 'Source: microsoft-ime-fcitx5' '' 'Package: microsoft-ime-fcitx5' 'Architecture: amd64' \
    >"$scratch/debian/control"
shared_dependencies=$(
    cd "$scratch"
    dpkg-shlibdeps -O "${addon_dir#"$scratch/"}/$method.so" "${runtime#"$scratch/"}/node"
)
awk -v dependencies="${shared_dependencies#shlibs:Depends=}" '{ sub(/\$\{shlibs:Depends\}/, dependencies); print }' \
    "$project/packaging/deb/control" >"$root/DEBIAN/control"
printf 'Installed-Size: %s\n' "$(du -sk "$root/usr" | cut -f1)" >>"$root/DEBIAN/control"
for script in postinst prerm postrm; do
    install -m 0755 "$project/packaging/deb/$script" "$root/DEBIAN/"
done
(
    cd "$root"
    find usr -type f -print0 | sort -z | xargs -0 md5sum >DEBIAN/md5sums
)
version=$(sed -n 's/^Version: //p' "$root/DEBIAN/control")
artifact="$project/dist/microsoft-ime-fcitx5_${version}_amd64.deb"
dpkg-deb --root-owner-group -Zxz --build "$root" "$artifact"
(cd "$project/dist" && sha256sum "$(basename "$artifact")" >"$(basename "$artifact").sha256")
