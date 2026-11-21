# 微软中文输入法 Fcitx5 贡献指南

本项目通过 Wine 为 Fcitx5 提供微软「中文输入法」1.0.0.17694 的 x64 拼音引擎。为 Ubuntu 22.04 和 24.04 分别构建 amd64 安装包。安装方式见 [README.md](README.md)，已验证的输入行为和实验范围见 [docs/features.md](docs/features.md)。

## 项目结构与入口

- `src/fcitx-addon.cpp` 注册 Fcitx5 输入法 `microsoft-chinese-keyboard`，通过 Unix 套接字发送按键并显示、提交引擎返回的候选。`src/key-protocol.hpp` 定义插件和宿主共享的按键协议常量。
- `tools/start.sh` 准备引擎并启动 `src/wine-relay.mjs`。转发进程连接套接字和 Wine 子进程的标准输入输出。`src/engine-host.cpp` 从 `wmain` 进入，调用原版 COM 数据源生成候选和处理选词反馈。
- `src/settings-server.mjs` 提供本机 `/api/settings` 接口并托管 `web/`。微软选项经宿主写入 Wine 注册表并重启宿主回读，注册表映射见 `src/native-settings.hpp`。Fcitx5 显示与快捷键选项写入 `fcitx5/conf/microsoft-chinese-keyboard.conf`。
- `tools/` 维护构建、下载和安装流程。`packaging/` 维护 `.deb` 包的元数据及维护脚本、Fcitx5 注册、桌面入口和 systemd 用户单元。`tests/` 存放 GTK 上屏和原版设置的端到端测试。

## 构建与开发

在项目根目录执行。Ubuntu 22.04 amd64 构建机启用 `universe`。构建依赖包括 `g++`、`g++-mingw-w64-x86-64-posix`、`libfcitx5core-dev` 和 `pkg-config`，打包另需 `curl`、`dpkg-dev` 和 `xz-utils`。源码运行使用 Node.js 18 或更新版本，并通过 `apt` 安装 Wine 11 或更新版本。

```sh
./tools/build.sh
./tools/package-deb.sh
```

`build.sh` 生成 `build/engine-host.exe` 和 `build/fcitx/microsoft-chinese-keyboard.so`。`package-deb.sh` 会重新构建，打包固定版本的 Node.js，按构建环境生成共享库依赖，并将 `.deb` 文件和 SHA-256 校验文件写入 `dist/`。打包时可通过 `IME_NODE_ARCHIVE` 提供已有的官方 Node.js 归档。桌面开发时可用 `./tools/install.sh` 安装到当前用户并启用 systemd 用户套接字，用 `./tools/uninstall.sh` 移除用户级插件和服务。

延续邻近代码的写法。C++ 使用 C++17 和四空格缩进，构建启用 `-Wall -Wextra -Werror`。Shell 脚本使用 POSIX `sh`，入口脚本使用 `set -eu`。JavaScript 使用原生 `.mjs` 模块、两空格缩进和分号，服务端使用 Node.js 内置模块。桌面与网页的用户可见文案沿用简体中文。

README 按实际操作顺序写明前置条件和安装命令。命令和扩展名按原样书写，兼容范围以已知的运行要求为准。

## 测试与验证

改动按键、候选或上屏行为时运行 GTK 测试。改动设置页、设置接口或注册表映射时运行原版设置测试。GTK 测试需要 Fcitx5 GTK3 前端、Xvfb、`xdotool` 和系统 Python 3 的 GTK3 绑定。以下命令在独立 D-Bus 会话中运行，两个测试各用一个临时 Wine 前缀，设置测试也使用临时 Fcitx5 配置目录。

```sh
dbus-run-session -- sh -eu <<'SH'
scratch=$(mktemp -d)
relay_pid=
trap '
    if [ -n "$relay_pid" ]; then
        kill "$relay_pid" 2>/dev/null || true
        wait "$relay_pid" || true
    fi
    rm -rf -- "$scratch"
' EXIT

export IME_STATE_DIR="$scratch/gtk-state" IME_SETTINGS_PORT=17866
./tools/prepare.sh
./tests/fcitx-session.sh zwsrf 中文输入法
IME_TEST_GLOBAL_CONFIG='[Hotkey]\nAltTriggerKeys=\n[Hotkey/TriggerKeys]\n0=Control+space\n1=Shift_L' \
    ./tests/fcitx-session.sh nihao nihao你好 Shift_L Shift_L n i h a o space
IME_TEST_OPTIONS='PageSize=7' ./tests/fcitx-session.sh shi 是 bracketright bracketleft 1
IME_TEST_OPTIONS='PageSize=7' ./tests/fcitx-session.sh shi 石 bracketright 1

export IME_STATE_DIR="$scratch/settings-state" IME_SETTINGS_PORT=17867
export XDG_CONFIG_HOME="$scratch/config" XDG_RUNTIME_DIR="$scratch/runtime"
export MS_IME_SOCKET="$XDG_RUNTIME_DIR/ime.sock"
mkdir -m 700 "$XDG_RUNTIME_DIR"
./tools/prepare.sh
./tools/start.sh &
relay_pid=$!
node tests/native-settings.mjs
SH
```

如示例端口已被占用，改用空闲端口。GTK 测试默认用空格选词，也可在期望文本后传入 `xdotool` 按键序列。`IME_TEST_GLOBAL_CONFIG` 写入测试会话的 Fcitx5 全局配置，可复现全局快捷键与插件快捷键冲突。设置 `IME_TEST_POPUP=1` 时，测试会先确认 X11 候选窗口可见。原版设置测试检查纠错、模糊拼音和候选变化，并核对设置回读。

打包改动使用 [apt 会话测试](tests/deb-apt-session.sh) 验收。在以 systemd 为 PID 1 的临时 Ubuntu 22.04 amd64 容器中，准备 `dbus-user-session`、Fcitx5 GTK 测试依赖、普通测试账户及已运行的 `user@UID.service`。以容器内的 root 运行脚本，依次传入账户名、上一版和当前版的 `.deb` 文件。测试核对 `apt` 安装的 Wine、微软 MSI 和许可证，并验证首次连接拉起引擎、候选窗显示、GTK 上屏、用户会话重启，以及清除包时保留用户数据。

## 配置与发布

- 每次提交用户可感知的功能、修复或安装变化时，在 [CHANGELOG.md](CHANGELOG.md) 的「未发布」段落下添加以 `- ` 开头的条目，说明用户能观察到的变化。
- [自动发布工作流](.github/workflows/auto-release.yml) 每周日北京时间 12:16 检查 `main`。最新正式版之后有新提交且「未发布」有条目时，自动递增补丁版本，归档变更日志并推送版本提交及标签。手动运行自动发布工作流可预览结果。
- 手动发布时同步更新 `packaging/deb/control` 的 `Version` 并把待发布条目归档到变更日志的 `v` 版本标题下，再提交并推送同版本标签。[发布工作流](.github/workflows/release.yml) 核对版本，在 Ubuntu 22.04 和 24.04 amd64 容器中构建并校验安装包，读取对应的变更日志作为 GitHub Release 说明。
- `tools/releases.sh` 固定微软 MSI 和 Node.js 的版本、下载地址及 SHA-256。`apt` 安装 `.deb` 时下载并校验微软 MSI，供各用户首次切换输入法时初始化。`./tools/prepare.sh /path/to/ChineseKeyboard.msi` 可使用已有的官方 MSI，仍会校验摘要。更新微软引擎版本时，重新验证 COM 接口、候选数据格式和上屏行为。
- 用户服务从 `${XDG_CONFIG_HOME:-$HOME/.config}/microsoft-ime-fcitx5/service.env` 读取环境变量。设置页默认监听 `127.0.0.1:17865`，由 `IME_SETTINGS_PORT` 覆盖。`IME_STATE_DIR` 和 `IME_CACHE_DIR` 使用绝对路径，默认分别取 XDG 状态和缓存目录下的 `microsoft-ime-fcitx5/`。`IME_WINE_BIN` 指向包含 `wine` 和 `wineserver` 的目录。手动启动时用 `MS_IME_SOCKET` 指定套接字，插件和测试客户端使用同一路径。
- 正式服务通过 Bubblewrap 将 Wine 置于独立的网络命名空间。微软引擎及词库由目标机准备，个人设置与学习数据存于用户 Wine 前缀。构建产物留在 `build/` 和 `dist/`，调试日志留在 `logs/`，这些目录均由 `.gitignore` 忽略。

## 提交与合并请求

以目标仓库近期提交和合并请求模板为准。缺少既有约定时，建议用 `type(scope): 中文摘要` 概括单一变更，范围取实际修改的模块。合并请求说明变更目的、影响范围、运行的验证命令与结果，并列出尚未执行的相关验证。涉及输入行为时附上拼音、选词按键和预期上屏文本，涉及设置页时附上操作步骤和回读结果。
