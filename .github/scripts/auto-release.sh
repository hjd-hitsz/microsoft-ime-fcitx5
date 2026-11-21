#!/bin/sh
set -eu

cd "$(dirname -- "$0")/../.."
: "${GH_REPO:?需要设置 GH_REPO}"
preview=${PREVIEW:-true}
control=packaging/deb/control
changelog=CHANGELOG.md
trailer=IME-Auto-Release

latest_release_tag() {
    gh api "repos/$GH_REPO/releases/latest" --jq '.tag_name'
}

dispatch_release() {
    tag=$1
    commit=$2
    if [ "$preview" != false ]; then
        printf '预览：将为 %s 触发发布。\n' "$tag"
        return
    fi
    active=$(gh run list --workflow release.yml --branch "$tag" --commit "$commit" \
        --json status --jq 'any(.[]; .status != "completed")')
    if [ "$active" = true ]; then
        printf '%s 的发布工作流正在运行。\n' "$tag"
        return
    fi
    gh workflow run release.yml --ref "$tag"
}

latest=$(latest_release_tag)
version=$(sed -n 's/^Version: //p' "$control")
dpkg --validate-version "$version"
if ! printf '%s\n' "$latest" | grep -Eq '^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$'; then
    printf '最新正式版 %s 使用手动发布流程。\n' "$latest"
    exit 0
fi

head=$(git rev-parse HEAD)
baseline=$(git rev-parse "$latest^{commit}")
git merge-base --is-ancestor "$baseline" "$head"
if [ "v$version" != "$latest" ]; then
    tag=v$version
    if dpkg --compare-versions "$version" gt "${latest#v}" &&
        commit=$(git rev-parse --verify --quiet "refs/tags/$tag^{commit}") &&
        git merge-base --is-ancestor "$commit" "$head" &&
        [ "$(git log -1 --format="%(trailers:key=$trailer,valueonly)" "$commit")" = "$tag" ]; then
        dispatch_release "$tag" "$commit"
    else
        printf '主分支版本 %s 与最新正式版 %s 不同，等待手动发布。\n' "$version" "$latest"
    fi
    exit 0
fi

if [ "$head" = "$baseline" ]; then
    printf '主分支已发布为 %s。\n' "$latest"
    exit 0
fi

entries=$(awk '
    /^## 未发布$/ { in_section = 1; next }
    in_section && /^## / { exit }
    in_section && /^- [^[:space:]]/ { count++ }
    END { if (!in_section) exit 1; print count + 0 }
' "$changelog") || {
    printf '%s 需要包含「未发布」二级标题。\n' "$changelog" >&2
    exit 1
}
if [ "$entries" -eq 0 ]; then
    printf '自 %s 以来没有待发布的变更日志条目。\n' "$latest"
    exit 0
fi

next="${version%.*}.$((${version##*.} + 1))"
tag=v$next
if git show-ref --verify --quiet "refs/tags/$tag"; then
    printf '标签 %s 已存在，保留现有发布。\n' "$tag"
    exit 0
fi
if [ "$preview" != false ]; then
    printf '预览：将从 %s 发布 %s，归档 %s 条变更。\n' "$head" "$tag" "$entries"
    exit 0
fi

test -z "$(git status --porcelain)"
test "$(latest_release_tag)" = "$latest"
sed -i "s/^Version: $version\$/Version: $next/" "$control"
test "$(sed -n 's/^Version: //p' "$control")" = "$next"
sed -i "s/microsoft-ime-fcitx5_${version}_/microsoft-ime-fcitx5_${next}_/g" README.md
sed -i "s/^## 未发布\$/&\\n\\n## $tag/" "$changelog"
git diff --check
git add -- "$control" "$changelog" README.md
git -c user.name='github-actions[bot]' \
    -c user.email='41898282+github-actions[bot]@users.noreply.github.com' \
    commit -m "chore(release): 准备 $tag 发布" \
    -m "- 更新 Debian 软件包版本和安装示例，归档未发布变更。" -m "$trailer: $tag"
git tag "$tag"
git push --atomic origin HEAD:refs/heads/main "refs/tags/$tag"
dispatch_release "$tag" "$(git rev-parse HEAD)"
