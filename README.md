# 微软中文输入法 Fcitx5

在 Linux 桌面上通过 Fcitx5 使用微软拼音。

## 安装依赖

输入法需要 Wine 11 或更新版本。在 Ubuntu 上，启用 `universe` 并添加与你的 Ubuntu 版本对应的 WineHQ 软件源。

```sh
sudo apt update && sudo apt install software-properties-common ca-certificates curl
sudo add-apt-repository --no-update universe
sudo dpkg --add-architecture i386
sudo install -d -m 0755 /etc/apt/keyrings
sudo curl -fsSL https://dl.winehq.org/wine-builds/winehq.key -o /etc/apt/keyrings/winehq-archive.key
. /etc/os-release
sudo curl -fsSL "https://dl.winehq.org/wine-builds/ubuntu/dists/$VERSION_CODENAME/winehq-$VERSION_CODENAME.sources" \
  -o "/etc/apt/sources.list.d/winehq-$VERSION_CODENAME.sources"
```

更新软件包列表，安装 Fcitx5、桌面配置工具和 Wine。

```sh
sudo apt update && sudo apt install fcitx5 fcitx5-config-qt fcitx5-frontend-gtk3 im-config winehq-stable
```

## 安装软件包

从 [GitHub Releases](https://github.com/hjd-hitsz/microsoft-ime-fcitx5/releases/latest) 下载 `.deb` 安装包。根据系统版本选择文件名中带 `ubuntu22.04` 或 `ubuntu24.04` 的包，在文件所在目录运行以下命令，把示例文件名换成你下载的文件名。安装期间保持联网，安装程序会从微软下载输入法文件。

```sh
sudo apt install ./microsoft-ime-fcitx5_0.1.1_ubuntu24.04_amd64.deb
```

## 从源码安装

完成上面的依赖安装后，解压源码，在源码目录打开终端。运行以下命令安装构建工具并生成安装包。

```sh
sudo apt install g++ g++-mingw-w64-x86-64-posix libfcitx5core-dev pkg-config \
  curl ca-certificates dpkg-dev xz-utils && ./tools/package-deb.sh
```

构建完成后，进入 `dist/` 并安装刚生成的包。

```sh
cd dist
sudo apt install ./microsoft-ime-fcitx5_0.1.1_amd64.deb
```

## 启用输入法

1. 如果桌面还没启用 Fcitx5，运行 `im-config -n fcitx5`。安装后注销并重新登录。
2. 运行 `fcitx5-configtool`，把「微软中文输入法」添加到输入法列表。如果找不到，取消勾选「仅显示当前语言」。
3. 打开文本编辑器，从 Fcitx5 菜单切换到「微软中文输入法」。首次切换后稍等片刻，再输入 `nihao` 并按空格，就能打出「你好」。单按 `Shift` 切换中英文。

## 打开设置

在系统应用菜单中搜索并打开「微软中文输入法设置」，或者在终端运行 `microsoft-ime-fcitx5 settings`，浏览器会打开设置页。输入法已启动时，也可以直接访问 [http://127.0.0.1:17865/](http://127.0.0.1:17865/)。

## 许可证

本项目的代码采用 [MIT 许可证](LICENSE)。Wine 和微软输入法按各自的许可证提供。
