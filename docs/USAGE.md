# 使用指南（面向没用过 KWin Effect 的人）

本文假设你只熟悉 Plasma 桌面本身，不需要了解 KWin 内部。先讲清楚"KWin Effect 到底是什么、
它是怎么被系统找到并启用的"，再给逐步操作。

---

## 1. 先理解：KWin Effect 是什么

**KWin 是 KDE Plasma 的合成器（compositor）**：屏幕上你看到的每一个画面——壁纸、窗口、鼠标指针——
都是 KWin 一个像素一个像素画出来的。窗口本身只提供内容，最终**每一帧**由 KWin 合成后交给显示器。

**KWin Effect（桌面效果）就是加载进 KWin 进程里的一个插件。** 它不是独立程序：

| | 普通程序 | KWin Effect |
| --- | --- | --- |
| 进程 | 自己的进程 | 运行在 KWin 进程内 |
| 界面 | 自己的窗口 | 没有，直接画在桌面画面上 |
| 能做什么 | 受窗口协议限制 | 可以挂钩合成每一帧、读取输入事件、画任意东西 |
| 生命周期 | 自己控制 | 跟着 KWin 启动/重载 |

你其实早就在用它了：**模糊（Blur）**、**窗口切换动画**、**鼠标点击动画**、**晃动指针放大
（Shake Cursor）** 全都是 KWin Effect。本项目的"指针轨迹"和它们是同一类东西。

**关键约束：Effect 必须与 KWin 同版本编译。** KWin 的头文件里明确写着这个 API 不提供二进制兼容性
（原文：*"the effect plugin must be compiled against the same kwineffects library version as KWin"*）。
所以升级 KWin 之后需要重新编译本插件。

另外，Effect 分两类，别混淆：

- **内建效果**：编译进 `kwin_wayland` 二进制里，由发行版提供（模糊、晃动指针放大等）。
- **插件效果**：运行时从磁盘加载 `.so` 文件 —— **本项目是这一类**。

---

## 2. 它是怎么被找到、被启用的（最容易卡住的一步）

KWin 启动时会用 `KPluginMetaData::findPlugins("kwin/effects/plugins")` 在 **Qt 的插件搜索路径**下
查找 `kwin/effects/plugins/` 里的 `.so` 文件，读取**内嵌在 .so 里的元数据**（即 `src/metadata.json`）。

**Qt 的默认插件搜索路径只有系统目录**（我在本机实测）：

```
/usr/lib64/qt6/plugins      ← 发行版安装的插件在这里，无需任何环境变量
/tmp                        ← 程序自身目录，与本项目无关
```

也就是说：

| 安装位置 | KWin 能否找到 | 说明 |
| --- | --- | --- |
| `/usr/lib64/qt6/plugins/kwin/effects/plugins/trail.so` | ✅ 直接找到 | 发行版打包方式，需要 root |
| `~/.local/lib64/qt6/plugins/kwin/effects/plugins/trail.so` | ❌ 默认找不到 | **必须设置 `QT_PLUGIN_PATH`**（本项目用这种方式，因为不需要 root） |

> ⚠️ 注意：`~/.local/share/kwin/effects/` 是**脚本/QML 效果**（KPackage 机制）的目录，
> **二进制插件不走这条路**。这是新手最容易搞错的地方。

**插件 id 由文件名决定**（不是元数据里的字段）：文件叫 `trail.so`，所以 kwinrc 里的开关是
`[Plugins] trailEnabled=true`。这也是本项目必须装成 `trail.so` 而不是 `libtrail.so` 的原因。

### 版本不匹配时会怎样（不会崩）

KWin 的 effect 工厂 IID 里**带着 KWin 的完整版本号**（`config-kwin.h` 定义）：

```
EffectPluginFactory_iid = "org.kde.kwin.EffectPluginFactory" + KWIN_PLUGIN_VERSION_STRING
                       = "org.kde.kwin.EffectPluginFactory6.7.5"
```

`PluginEffectLoader::factory()` 在读插件元数据后、**实例化插件之前**就比对它：

```cpp
QPluginLoader loader(info.fileName());
if (loader.metaData().value("IID").toString() != EffectPluginFactory_iid) {
    qCDebug(KWIN_CORE) << info.pluginId() << " has not matching plugin version, ...";
    return nullptr;          // ← 到此为止，不会调用 loader.instance()
}
factory = qobject_cast<KPluginFactory *>(loader.instance());
```

我在嵌套会话里把插件的 IID **二进制改写**成 `6.7.4`（等价于"KWin 升级了、插件没重编译"），
实测 KWin 6.7.5 的输出：

```
kwin_core: "trail"  has not matching plugin version, expected  org.kde.kwin.EffectPluginFactory6.7.5 got  "org.kde.kwin.EffectPluginFactory6.7.4"
kwin_core: Couldn't get an EffectPluginFactory for:  "trail"
```

结论：

- **不会崩溃**。KWin 只打一行调试日志就跳过这个插件，其它效果照常加载，合成器继续运行。
- 效果**仍会出现在桌面效果列表里**（因为是靠元数据发现的，元数据与版本无关），但
  `isEffectSupported` 返回 `false`，也就是**列出来但不可勾选**。
- 这些都只是 `qCDebug`：**默认日志级别下完全看不到**。所以升级 KWin 后的典型表现是
  "效果悄悄不见了/变灰了"，而不是报错弹窗。
- 反过来，**如果版本号相同但 ABI 实际不兼容**（例如有人手工改造过 KWin），这道检查会通过，
  插件就会被真正实例化 —— 那种情况下有可能崩溃。KWin effect 运行在合成器进程内、没有沙箱，
  所以"同名版本的不兼容构建"和"插件自身 bug"都可能导致整个桌面崩溃。这正是本项目
  坚持先在**嵌套会话**里测试的原因（见 §8）。

---

## 3. 前置条件

| 需要什么 | 本机已验证 |
| --- | --- |
| KWin 版本 | `kwin_wayland --version` → `kwin 6.7.5` |
| 编译用的 KWin 头文件版本 | 必须也是 6.7.5（本项目用 `kwin-devel-6.7.5`） |
| Qt6 / KF6 | 6.11.2 / 6.30 |

若系统没装 `kwin-devel`、`qt6-qtbase-devel` 等开发包且你没有 root，用本文方案里的
"本地 sysroot" 方式即可（见 §4）。

---

## 4. 构建与安装

```bash
cd <仓库目录>

# 仅在缺少系统开发包时执行一次：把 Fedora 的 devel RPM 解包到 ./.sysroot（不需要 root）
tools/bootstrap-dev-sysroot.sh

# 配置 + 编译 + 跑单元测试 + 安装到 ~/.local
helpers/build.sh
```

产物：`~/.local/lib64/qt6/plugins/kwin/effects/plugins/trail.so`

可调环境变量：

```bash
PREFIX=/some/prefix helpers/build.sh      # 换安装前缀
BUILD_TYPE=Debug  helpers/build.sh        # 换构建类型
BUILD_DIR=build-dbg helpers/build.sh      # 换构建目录
```

---

## 5. 在真实桌面里启用（三步）

### 步骤 1：让正在运行的 KWin 能找到插件

因为装在用户目录，需要把该目录加入 Qt 插件搜索路径。写一个环境变量文件：

```bash
helpers/install-session-env.sh          # 写入 ~/.config/environment.d/50-kwin-trail.conf
```

内容等价于：

```ini
QT_PLUGIN_PATH=/home/<你>/.local/lib64/qt6/plugins
```

然后**注销并重新登录**（Plasma 6 的 KWin 由 systemd 用户服务启动，只在登录时读取环境变量）。
重新登录后确认：

```bash
systemctl --user show-environment | grep QT_PLUGIN_PATH
```

### 步骤 2：确认 KWin 真的认识这个插件

KWin 提供 D-Bus 接口查询效果状态，这是最可靠的检查手段：

```bash
# 是否被 KWin 发现（false = 插件没被找到，回到步骤 1 检查路径/是否重新登录）
gdbus call --session --dest org.kde.KWin --object-path /Effects \
    --method org.kde.kwin.Effects.isEffectSupported trail

# 是否已加载（启用）
gdbus call --session --dest org.kde.KWin --object-path /Effects \
    --method org.kde.kwin.Effects.isEffectLoaded trail
```

本机目前的实测输出（可用来对照）：

```
blur         supported=true   loaded=true     ← 内建效果，已启用
shakecursor  supported=true   loaded=false    ← 内建效果，被手动关掉
trail        supported=false  loaded=false    ← 用户目录插件，KWin 还没被告诉去哪找（步骤 1 未做）
```

### 步骤 3：启用

两种方式，任选：

```bash
# 方式 A：命令行写入 kwinrc
helpers/enable-effect.sh enable

# 方式 B：图形界面
#   系统设置 → 窗口管理 → 桌面效果 → 搜索 "Pointer Trail" → 勾选
```

**方式 B 里能搜到 "Pointer Trail"，就说明步骤 1 成功了。** 勾选后立即生效，不需要重启。

也可以不改配置、临时加载：

```bash
gdbus call --session --dest org.kde.KWin --object-path /Effects \
    --method org.kde.kwin.Effects.loadEffect trail
```

### 看到什么才算成功

快速甩动鼠标：指针后方会跟出一串指针残影。默认 `TrailFrames=1` 时轨迹只有**一帧**那么长，
甩得越快越明显；想更容易看到就把 `TrailFrames` 调到 4~8（见 §6）。

---

## 6. 配置

配置文件 `~/.config/kwinrc`，组 `[Effect-trail]`：

```ini
[Effect-trail]
Enabled=true        ; 运行期总开关（不卸载插件，只让它不画）
TrailFrames=1       ; 采样保留多少个帧间隔：1 = 严格“过去一帧”，调大 = 更长轨迹
MaxSamples=256      ; 每帧最多画多少个指针（帧预算保护，正常用不到改）
```

命令行修改示例：

```bash
kwriteconfig6 --file kwinrc --group Effect-trail --key TrailFrames 6
kwriteconfig6 --file kwinrc --group Effect-trail --key Enabled true
```

KWin 会监视 kwinrc 并在改动后自动重新读取；若没生效，可强制：

```bash
gdbus call --session --dest org.kde.KWin --object-path /Effects \
    --method org.kde.kwin.Effects.reconfigureEffect trail
```

---

## 7. 关闭与卸载

```bash
helpers/enable-effect.sh disable       # 关闭（保留文件）
helpers/enable-effect.sh status        # 查看当前开关
helpers/install-session-env.sh --remove # 撤销环境变量（下次登录后生效）
rm -rf ~/.local/lib64/qt6/plugins/kwin/effects/plugins/trail.so   # 删除插件
```

---

## 8. 不想动当前桌面？用隔离的嵌套会话

**开发/试用本插件时，最安全的方式是嵌套会话**：它会启动一个**独立的** KWin（自己的 D-Bus、
配置目录和 socket），显示在宿主桌面的一个窗口里。宿主桌面完全不受影响。

```bash
# 端到端自动测试：启动嵌套 KWin + 注入指针路径 + 检查是否真的画上了像素
helpers/nested-e2e.sh

# 手动把玩：启动嵌套 KWin（内含一个 konsole 窗口），日志直接打在终端
helpers/run-nested.sh

# 往嵌套会话里再开一个程序（在另一个终端里）
WAYLAND_DISPLAY=wayland-dev QT_PLUGIN_PATH="$HOME/.local/lib64/qt6/plugins" dolphin
```

> ❗ 项目规范（见 `KWIN_AGENT_GUIDE.md`）**禁止**在宿主会话里执行
> `kwin_wayland --replace` 或重启显示管理器——那会让你的桌面黑屏/闪烁/丢失窗口。
> 所有测试都必须在嵌套实例里做。

为什么嵌套里能立刻用、而真实桌面要重新登录？因为 `helpers/run-nested.sh` 自己在启动时
注入了 `QT_PLUGIN_PATH`（见 §2），而你已经运行的 KWin 只认它自己启动时的环境变量。

---

## 9. 排查速查表

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| 桌面效果里搜不到 "Pointer Trail"；`isEffectSupported trail` = false | KWin 的插件搜索路径里没有 `~/.local/lib64/qt6/plugins` | `helpers/install-session-env.sh` 后**注销重登** |
| `isEffectSupported` = true 但勾选后没反应 | `[Effect-trail] Enabled=false`，或 `isSupported()` 判定当前不是 OpenGL 合成 | 检查 kwinrc；确认没在用软件合成 (`KWIN_COMPOSE=Q`? 一般不用管) |
| 桌面效果里能看到但**无法勾选**（灰掉）；`isEffectSupported trail` = false；日志有 `has not matching plugin version` | KWin 升级过，插件是旧版本编译的（IID 版本号不匹配） | 重新 `helpers/build.sh` 即可；不会崩溃，见 §2 |
| 日志里完全没有 `kwin_effect_trail` | 插件没被加载（未启用 / 未发现） | 回到 §5 步骤 1、2 |
| **截图里看不到轨迹** | KWin 的截图与录屏**绕过 effect 链**（直接渲染场景） | 这是 KWin 设计如此，不是 bug。自动化验证用 `TRAIL_KWIN_SELFCHECK=1` |
| 甩鼠标时指针反而变大 | 这是内建的"晃动指针放大"效果 | 关掉它：`kwriteconfig6 --file kwinrc --group Plugins --key shakecursorEnabled false` |
| 看不到效果日志 | KWin 默认把日志送到 journald | 用 `journalctl --user -u plasma-kwin_wayland -f`，或嵌套会话（`QT_FORCE_STDERR_LOGGING=1`） |

### 看日志

```bash
# 真实会话
journalctl --user -u plasma-kwin_wayland -f | grep -E "kwin_effect_trail|effect"

# 嵌套会话（helpers/run-nested.sh 已自动开启调试日志并输出到终端）
QT_LOGGING_RULES="kwin_effect_trail.debug=true" helpers/run-nested.sh
```

---

## 10. 术语速查

| 术语 | 含义 |
| --- | --- |
| 合成器 / compositing | KWin 把各窗口画面合成为最终屏幕画面的过程 |
| effect chain | 同一帧里多个 effect 依次处理；本项目的绘制在窗口之上 |
| damage / repaint 区域 | 本帧需要重画的区域；只重画变化部分才能省电、省 GPU |
| output / scale | 一个显示输出（屏幕）；`scale` 是分数缩放比，逻辑坐标 × scale = 设备像素 |
| `KPluginMetaData` | KDE 用来发现插件的元数据（本项目内嵌在 `trail.so` 里） |
| `KWIN_EFFECT_FACTORY` | KWin 要求 effect 使用的插件工厂宏，写错了 KWin 就不会加载 |
| `kwinrc` / `[Plugins]` | KWin 的配置文件；`[Plugins] <id>Enabled=true` 控制效果开关 |
| `InputEventSpy` | KWin 内部"旁听"输入事件的接口，本项目用它采样指针位置（不影响事件传递） |

---

## 11. 一分钟上手（最短路径）

```bash
tools/bootstrap-dev-sysroot.sh      # 只在缺开发包时
helpers/build.sh                    # 编译安装到 ~/.local
helpers/install-session-env.sh      # 让 KWin 能找到插件
# 注销、重新登录
helpers/enable-effect.sh enable     # 或系统设置 → 桌面效果里勾选
# 快速甩动鼠标即可看到指针残影

# 想马上看效果、又不想重新登录：
helpers/nested-e2e.sh
```
