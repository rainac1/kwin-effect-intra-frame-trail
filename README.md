# Pointer Trail — 指针轨迹 KWin Effect

在桌面的每一帧里，取出**过去一帧内**指针采样到的所有位置，并在每个位置渲染**当前的指针图形**。
用 KWin 的原生 C++/OpenGL effect 实现，目标是最小延迟与最小开销。

```
注入 250 采样/s，渲染 120 fps，TrailFrames=4
→ 实测 8.30 指针/帧（250/120 × 4 = 8.33）          0 丢采样
```

设计依据、API 调研证据与性能分析见 [`docs/DESIGN.md`](docs/DESIGN.md)。

---

## 环境要求

| 组件 | 版本（已验证） |
| --- | --- |
| KWin | 6.7.5（**必须与编译时的 kwin-devel 完全同版本**，effect ABI 不稳定） |
| Qt | 6.11.2 |
| KF6 | 6.30 |
| 构建 | CMake ≥ 3.20、Ninja、GCC 16 |

## 快速开始

```bash
# 1) 构建 + 单元测试 + 安装到 ~/.local
helpers/build.sh

# 2) 让正在运行的 KWin 能找到用户目录里的插件，然后【注销并重新登录】
helpers/install-session-env.sh

# 3) 启用（也可在 系统设置 → 窗口管理 → 桌面效果 里勾选 "Pointer Trail"）
helpers/enable-effect.sh enable

# 或者：在隔离的嵌套合成器里直接端到端测试（无需重新登录，不触碰当前会话）
helpers/nested-e2e.sh
```

> **为什么需要第 2 步**：KWin 通过 Qt 的插件搜索路径查找二进制 effect，而 Qt 默认只搜索
> 系统目录（`/usr/lib64/qt6/plugins`），**不包含** `~/.local`。这一步会写入
> `~/.config/environment.d/50-kwin-trail.conf`。细节见 [`docs/USAGE.md`](docs/USAGE.md)。
>
> 不确定插件有没有被 KWin 认出来？`helpers/enable-effect.sh status` 会直接问运行中的 KWin。

没有系统级 `kwin-devel` / `qt6-qtbase-devel` 时（无 root、容器、CI 等），
`helpers/build.sh` 会自动使用本地 sysroot；首次需先准备：

```bash
tools/bootstrap-dev-sysroot.sh   # 下载并解包 Fedora 的 devel RPM 到 ./.sysroot，无需 root
helpers/build.sh
```

构建路径解析顺序：若 `.sysroot/usr` 存在则 `CMAKE_PREFIX_PATH` 指向它，否则用系统安装的开发包。

## 配置

`~/.config/kwinrc`：

```ini
[Plugins]
trailEnabled=true          ; 是否加载本 effect（也可用 helpers/enable-effect.sh）

[Effect-trail]
Enabled=true               ; 运行期总开关
TrailFrames=1              ; 保留多少个帧间隔的采样；1 = 严格“过去一帧”
MaxSamples=256             ; 每帧最多绘制的指针数量上限
```

## 项目结构

```
src/trailsample.{h,cpp}    采样环形缓冲 + 光标几何（不依赖 KWin/GL，可单元测试）
src/traileffect.{h,cpp}    Effect + InputEventSpy：采样、损伤、GL 绘制
src/main.cpp               插件工厂
src/metadata.json          插件元数据（id 由文件名 trail.so 决定，勿写 Id）
tests/                     单元测试 + 插件可发现性测试
tools/bootstrap-dev-sysroot.sh   无 root 构建本地开发 sysroot
tools/fake-input-injector.cpp    测试用指针注入器（org_kde_kwin_fake_input）
helpers/build.sh           配置/构建/测试/安装
helpers/run-nested.sh      启动隔离的嵌套 KWin 会话
helpers/nested-e2e.sh      端到端测试：注入指针路径 + 像素级自检
helpers/enable-effect.sh   在用户会话中启用/停用，并用 D-Bus 查询真实状态
helpers/install-session-env.sh  让 KWin 能找到用户目录里的插件（需重新登录）
docs/USAGE.md              面向新手的详细使用说明（概念、启用、排查）
docs/DESIGN.md             设计方案与调研结论
```

## 测试

```bash
ctest --test-dir build --output-on-failure   # 单元测试 + 插件元数据
helpers/nested-e2e.sh                        # 嵌套合成器端到端
```

`helpers/nested-e2e.sh` 会启动一个**独立的**嵌套 `kwin_wayland`（自有 D-Bus、配置与 socket，
不干扰宿主会话），通过 KWin 自带的 fake-input 协议注入指针路径，并检查 effect 是否真的写入了像素。

### 调试日志

```bash
helpers/run-nested.sh                       # 默认已开 kwin_effect_trail.debug
QT_LOGGING_RULES="kwin_effect_trail.debug=true" CLIENT=konsole helpers/run-nested.sh
```

`TRAIL_KWIN_SELFCHECK=1` 开启像素级自检（绘制前后 `glReadPixels` 比较）。

## 三个容易踩的坑

1. **截图/录屏看不到本 effect 的覆盖层**。KWin 的 screenshot 与 screencast 直接渲染场景
   （`sceneView.paint()`），绕过 effect 链。这是设计如此，不是本项目的 bug；
   自动化验证请用 `TRAIL_KWIN_SELFCHECK=1`。
2. **晃动指针放大（Shake Cursor）会干扰测试**。它默认启用，而测试注入器正是快速来回移动指针。
   `helpers/run-nested.sh` 已在测试配置里写 `shakecursorEnabled=false`。
3. **手工启动嵌套 KWin 时必须隔离 `XDG_CONFIG_HOME`**（以及用 `dbus-run-session` 隔离总线）。
   否则嵌套实例会改写你**真实的** `~/.config/kwinrc` 与 `~/.config/kglobalshortcutsrc`，
   并且可能导致宿主 KWin 的全局快捷键组件（`/component/kwin`）变成 `isActive=false`，
   症状是 **Meta+D、Alt+Tab 等 KWin 快捷键全部无响应**（该状态是运行期的，没有配置项可修，
   只能重新登录或重启合成器恢复）。检查方法：

   ```bash
   # true = 正常；false = 该组件的快捷键不会触发
   gdbus call --session --dest org.kde.kglobalaccel --object-path /component/kwin \
       --method org.kde.kglobalaccel.Component.isActive
   ```

   `helpers/run-nested.sh` 已经隔离了 `XDG_CONFIG_HOME`/`XDG_CACHE_HOME` 并走
   `dbus-run-session`，所以用它做测试不会影响当前会话。

## 安全须知

- 所有测试都在**隔离的嵌套实例**中进行，不会 `--replace` 宿主合成器，也不写入 `/usr`。
- 安装前缀固定为用户目录 `~/.local`；可用 `PREFIX=... helpers/build.sh` 覆盖。

## 许可

GPL-2.0-or-later（见 `LICENSE`）。所有源文件带 SPDX 标识。
