# 微软中文输入法 Fcitx5

在 Linux 桌面上通过 Fcitx5 使用微软拼音。

## 安装依赖

在 Ubuntu 上，先启用 `universe` 软件源。

```sh
sudo add-apt-repository --no-update universe
```

更新软件包列表，安装 Fcitx5 及其桌面配置工具。

```sh
sudo apt update && sudo apt install fcitx5 fcitx5-config-qt fcitx5-frontend-gtk3 im-config
```

## 安装软件包

从 [GitHub Releases](https://github.com/hjd-hitsz/microsoft-ime-fcitx5/releases/latest) 下载适用于 Ubuntu 22.04 amd64 的 `.deb` 安装包。安装包由 GitHub Actions 构建，下载页提供对应的 `.sha256` 校验文件。在文件所在目录打开终端，把命令中的文件名换成下载的文件名。`apt` 会一并安装其余依赖。

```sh
sudo apt install ./microsoft-ime-fcitx5_0.1.0-3_amd64.deb
```

## 从源码安装

完成上面的依赖安装后，解压源码，在源码目录打开终端。保持联网，运行以下命令安装构建工具并生成安装包。

```sh
sudo apt install g++ g++-mingw-w64-x86-64-posix libfcitx5core-dev pkg-config \
  curl ca-certificates dpkg-dev xz-utils && ./tools/package-deb.sh
```

构建完成后，进入 `dist/`，用上面的 `apt install` 命令安装刚生成的文件。

## 启用输入法

1. 如果桌面还没启用 Fcitx5，运行 `im-config -n fcitx5`。安装后注销并重新登录。
2. 从应用菜单打开「微软中文输入法设置」。首次打开时，程序会自动下载 Wine 和微软输入法文件，可能需要几分钟。保持联网，等浏览器显示设置页面。
3. 运行 `fcitx5-configtool`，把「微软中文输入法」添加到输入法列表。如果找不到，取消勾选「仅显示当前语言」。
4. 打开文本编辑器，从 Fcitx5 菜单切换到「微软中文输入法」。输入 `nihao` 后按空格，就能打出「你好」。单按 `Shift` 切换中英文。
