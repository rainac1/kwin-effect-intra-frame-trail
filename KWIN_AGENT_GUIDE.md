# KWin Effect 自动化开发与安全调试规范 (Fedora / Plasma 6)

本文件是针对 AI Agent 在开发和调试 KWin Effect（C++ / QML）时的行为准则与操作手册。**必须严格遵守隔离原则，杜绝导致宿主桌面崩溃或文件污染的操作。**

---

## 1. 核心安全红线 (Safety Invariants)

- ❌ **绝对禁止** 在宿主会话中运行 `kwin_wayland --replace` 或重启宿主显示管理器服务。
- ❌ **绝对禁止** 使用 `sudo make install` 将未验证的插件直接写入 `/usr` 系统目录。
- ❌ **绝对禁止** 在无 `--socket` 参数的情况下启动裸 `kwin_wayland`。
- ✅ **必须** 将插件安装至用户本地目录 `~/.local`。
- ✅ **必须** 所有运行、测试、重载操作均在 **隔离的嵌套 Wayland 实例** 中执行。
- ✅ **必须** 显式注入本地环境变量（如 `QT_PLUGIN_PATH`），确保嵌套实例加载的是新构建的本地插件。

---

## 2. 环境变量与目录规范

在执行任何构建、测试、启动命令前，需确认以下路径基准：

- **安装前缀 (Install Prefix)**: `~/.local`
- **本地插件路径 (Plugin Path)**: `~/.local/lib64/qt6/plugins` 或 `~/.local/lib/qt6/plugins`
- **本地特效元数据**: `~/.local/share/kwin/effects/`
- **调试会话套接字**: `wayland-dev`

```bash
# 嵌套测试的标准环境变量前缀
export QT_PLUGIN_PATH="$HOME/.local/lib64/qt6/plugins:$HOME/.local/lib/qt6/plugins:$QT_PLUGIN_PATH"
export XDG_DATA_DIRS="$HOME/.local/share:$XDG_DATA_DIRS"
export WAYLAND_DISPLAY="wayland-dev"
```

---

## 3. 标准开发流水线 (Standard Workflow)

Agent 在编写、修改代码后，应按以下顺序执行验证：

### 阶段 1：构建与本地部署 (Build & Install)
```bash
# 1. CMake 配置 (确保指向 ~/.local)
cmake -B build -S . \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_INSTALL_PREFIX=$HOME/.local

# 2. 编译并安装到本地
cmake --build build -j$(nproc)
cmake --install build
```

### 阶段 2：启动隔离测试环境 (Spawn Sandbox)
在子进程/新终端窗口启动嵌套环境，**务必使用 `dbus-run-session` 隔离总线**，并启用详细日志：

```bash
QT_PLUGIN_PATH="$HOME/.local/lib64/qt6/plugins:$HOME/.local/lib/qt6/plugins:$QT_PLUGIN_PATH" \
XDG_DATA_DIRS="$HOME/.local/share:$XDG_DATA_DIRS" \
QT_LOGGING_RULES="kwin_*.debug=true;kwin_effect_*.debug=true" \
dbus-run-session kwin_wayland --socket wayland-dev --width 1280 --height 720 konsole
```

### 阶段 3：在沙盒内注入测试应用 (Test Target)
向嵌套窗口中启动图形程序以触发特效逻辑：
```bash
# 启动测试窗口
WAYLAND_DISPLAY=wayland-dev kwrite &
# 或者启动 Dolphin 进行窗口动效测试
WAYLAND_DISPLAY=wayland-dev dolphin &
```

### 阶段 4：配置热重载与启用插件 (Enable & Reload)
若特效未默认启用，可通过嵌套环境内的 D-Bus / kwriteconfig6 开启：
```bash
# 修改嵌套环境配置
kwriteconfig6 --file kwinrc --group Plugins --key <EFFECT_NAME>Enabled true
```

---

## 4. 故障排查与断点捕获 (Debugging & Diagnostics)

### 场景 A：排查插件是否被正确加载
若嵌套窗口启动后看不到特效，执行以下检查：
1. 检查日志中是否有 `kwin_core: Couldn't create effect` 或找不到 plugin 的报错。
2. 确认 `metadata.json` 里的 `KPlugin.Id` 与 C++ 中导出的 `KWIN_EFFECT_FACTORY` / `K_PLUGIN_CLASS_WITH_JSON` 严格一致。

### 场景 B：捕获段错误（Crash / Segfault）
如果修改导致 KWin 崩溃，**禁止在宿主中调试**，使用 GDB 托管嵌套实例运行：
```bash
QT_PLUGIN_PATH="$HOME/.local/lib64/qt6/plugins:$HOME/.local/lib/qt6/plugins:$QT_PLUGIN_PATH" \
gdb -ex run --args kwin_wayland --socket wayland-dev --width 1280 --height 720 konsole
```
崩溃发生后，在 GDB 中执行 `bt`（Backtrace）提取调用栈并分析修复。

### 场景 C：清理残留测试进程
如果测试会话挂起或套接字占用，使用安全清理命令：
```bash
# 仅清理指定套接字相关的嵌套进程，不得误杀宿主 kwin_wayland
pkill -f "kwin_wayland.*--socket wayland-dev" || true
rm -f /run/user/$UID/wayland-dev*
```

---

## 5. Plasma 6 技术规范速查

1. **配置工具**: 使用 `kwriteconfig6` / `kreadconfig6`（严禁使用 KDE 5 的 `kwriteconfig5`）。
2. **构建框架**: 基于 **KF6**（Extra CMake Modules、KF6CoreAddons、KF6Config 等）。
3. **命令行参数**: Plasma 6 的 `kwin_wayland` 在检测到现有图形环境时会自动进入嵌套模式，**无 `--nested` 参数**。
