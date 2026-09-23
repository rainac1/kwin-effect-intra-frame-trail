# 指针轨迹 KWin Effect — 设计方案

本文档记录需求、对 KWin 6.7.5 的实际 API 调研结论、最终方案、性能与延迟分析，以及验证方法。
所有关于 KWin 的结论都来自本机安装版本（Plasma 6.7.5 / KWin 6.7.5 / Qt 6.11.2 / KF6 6.30）的
源码与已安装头文件，而不是文档推测。

---

## 1. 需求

渲染桌面每一帧时，取出**过去一帧内**指针采样到的所有数据点，并在**每个对应位置**渲染**当前的指针**图形。
要求低延迟 + 高性能。

---

## 2. 调研结论（决定方案的硬性事实）

### 2.1 KWin **没有**提供历史指针数据

| 可用 API | 提供什么 | 局限 |
| --- | --- | --- |
| `EffectsHandler::cursorPos()` / `Effect::cursorPos()` | 当前坐标 | 只有最新值 |
| `EffectsHandler::mouseChanged(pos, oldpos, …)` 信号 | 逐个事件的新/旧位置 | 无时间戳；按键/修饰键变化也会触发 |
| `InputRedirection::m_lastPosition`（`takeLastPosition()`） | `std::optional<QPointF>` **单槽** | 供数位板同步用，且是**消费型**（取走即清空） |

没有任何位置序列缓存。反证：KWin 自己需要运动历史的地方也都自己攒 —— `mousemark` 自存
`QList<QPointF> Mark`，`shakecursor` 依赖 `ShakeDetector` 自己积累采样。
**结论：缓冲区必须由 effect 自己维护。**

### 2.2 最佳事件源是 `InputEventSpy`，不是 `mouseChanged`

`kwin/input.h` 与 `kwin/input_event_spy.h` **都在已安装头文件中**，`input()` 是头文件里的 inline 函数，
KWin 自己的插件（`hidecursor`、`shakecursor`）就是 `class X : public Effect, public InputEventSpy`
加 `input()->installInputEventSpy(this)`。

```cpp
struct PointerMotionEvent {
    QPointF position, delta, deltaUnaccelerated;
    bool warp;
    Qt::MouseButtons buttons;
    Qt::KeyboardModifiers modifiers;
    std::chrono::microseconds timestamp;   // libinput 的 CLOCK_MONOTONIC 时间戳
};
```

相比 `mouseChanged`：带**事件真实时间戳**（"过去一帧"按事件时间而非到达时间定义）、只在真正移动时触发、
额外提供 `delta`。代价是它不属于文档化的 effect API（但头文件已安装、in-tree 插件广泛使用），
因此实现中把它隔离在单个回调里，便于退回 `mouseChanged`。

### 2.3 采样与绘制在**同一个线程**（主线程）

- libinput 由 `QSocketNotifier` 在主线程读取（`backends/libinput/connection.cpp`）。
- 事件同步推进到 `InputRedirection::globalPointerChanged` → `Effect::pointerMotion`。
- 帧绘制也在主线程；GL context 在主线程 make current（`scene/workspacescene.cpp`）。
- 插件加载是同步的：`PluginEffectLoader` 用 `KPluginMetaData::findPlugins`；只有 *scripted* effect 的
  发现走 `QtConcurrent`。

**所以不需要额外线程**（详见 §5）。

### 2.4 绘制契约（坐标空间是最容易踩的坑）

```cpp
void Effect::prePaintScreen(ScreenPrePaintData &data);   // data.paint 是【逻辑坐标】
void Effect::paintScreen(const RenderTarget &, const RenderViewport &,
                         int mask, const Region &deviceRegion, LogicalOutput *);  // deviceRegion 是【设备坐标】
```

- `ScreenPrePaintData::paint` 由场景转换：`deviceDamage = view->mapToDeviceCoordinatesAligned(data.paint) & deviceRect()`
  → 往 `data.paint` 里加**逻辑**矩形。
- `viewport.projectionMatrix()` 作用于**设备像素**，逻辑坐标要乘 `viewport.scale()`
  （KWin 自己的 `mouseclick` 就是 `(x + cx) * scale`）。
- effect 链语义：先被调用的 effect **最后绘制**（每个 effect 调用 `effects->paintScreen()` 委托给链上其余部分）。
  因此 `requestedEffectChainPosition()` 返回 0 即"最先调用 → 画在最上层"，
  与 `mouseclick`/`touchpoints` 一致；绘制时机是在委托之后，所以轨迹在窗口之上。
- 光标几何：`EffectWindow` 的做法是位置 `-hotspot`、尺寸 `image.size() / devicePixelRatio()`
  （见 `scene/cursoritem.cpp`），本项目照抄以保证与真实光标完全对齐。

### 2.5 纹理与混合

- `GLTexture::upload(QImage)` 以 `OutputTransform::FlipY` 建立内容变换，
  `GLTexture::render()` 内部处理纹理矩阵与 Y 翻转（与 `magnifier` 同一路径），因此直接复用而不自己算 texcoord。
- 光标图像上传为**预乘 ARGB**，必须用**预乘混合** `glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA)`，
  这与场景渲染器一致（`scene/itemrenderer_opengl.cpp`）；用错会出现暗边。
- 上传只在光标图像变化时发生（比较 `QImage::cacheKey()`），例如从普通箭头切到文本光标。

### 2.6 插件契约

- `kcoreaddons_add_plugin(trail SOURCES … INSTALL_NAMESPACE "kwin/effects/plugins")` 生成动态插件；
  必须 `BUILD_SHARED_LIBS=ON`，否则会构建成静态库而无法被加载。
- 工厂宏 `KWIN_EFFECT_FACTORY_SUPPORTED(TrailEffect, "metadata.json", return effects->isOpenGLCompositing();)`。
- **插件 id 由文件名决定**（KDE 会警告 metadata 中显式写 `Id`）：文件必须是 `trail.so` 而不是 `libtrail.so`
  （故 `set_target_properties(trail PROPERTIES PREFIX "")`），kwinrc 开关为 `[Plugins] trailEnabled=true`。
- ABI：头文件里明确写了 effect 插件必须与 KWin 同版本编译（本机 KWin 6.7.5 ↔ kwin-devel 6.7.5）。
  实际保护机制是**插件工厂 IID 里带完整版本号**：
  `EffectPluginFactory_iid = "org.kde.kwin.EffectPluginFactory" KWIN_PLUGIN_VERSION_STRING`
  （`config-kwin.h` → `"6.7.5"`）。`PluginEffectLoader::factory()` 先读元数据比对 IID，
  不匹配就 `qCDebug` 一行并返回 `nullptr`，**不会调用 `loader.instance()`**，因此插件工厂与
  `createEffect()` 都不会执行。实测把 IID 改成 6.7.4 后：KWin 记录
  `has not matching plugin version` + `Couldn't get an EffectPluginFactory`，其余效果照常加载，
  合成器不崩溃。注意该日志是 `qCDebug(KWIN_CORE)`，默认不可见；且**元数据层面的发现与版本无关**，
  所以插件仍会出现在效果列表里，只是 `isEffectSupported()` 为 false（列出来但不可勾选）。
- `kcoreaddons_add_plugin` 安装到 `${KDE_INSTALL_PLUGINDIR}/kwin/effects/plugins`。ECM 只有在
  安装前缀等于 Qt 前缀时才会用 `qt6` 子目录；用户前缀下必须显式指定
  `KDE_INSTALL_PLUGINDIR=${KDE_INSTALL_LIBDIR}/qt6/plugins`，才能落到
  `~/.local/lib64/qt6/plugins/kwin/effects/plugins/`。
- **AUTOMOC 看不见宏里的 JSON 文件名**，因此改 `metadata.json` 不会自动重生成内嵌元数据；
  `helpers/build.sh` 检测到该文件比产物新时会清掉 autogen 目录强制重生成。

### 2.7 重要发现：截图/录屏**不包含** effect 覆盖层

`src/plugins/screenshot/screenshotlayer.cpp` 与 `src/plugins/screencast/*` 直接
`sceneView.paint()` / `scene->renderer()->renderItem()` 渲染场景（并单独渲染 cursor item），
**绕过了 effect 链**。因此 Spectacle 截图里永远看不到本 effect 的覆盖层 —— 这不是 bug，
而是 KWin 的设计。自动化验证像素只能靠 effect 内部回读（见 §7.3）。

### 2.8 会干扰测试的内建效果

`shakecursor`（晃动指针放大）默认启用（`EnabledByDefault: true`），
而测试注入器正是"快速来回移动指针"，会触发它。测试会话里必须显式
`shakecursorEnabled=false`（`helpers/run-nested.sh` 已处理）。

---

## 3. 总体架构

```
输入线程/主线程                                    主线程（每帧一次）
┌──────────────────────┐                    ┌───────────────────────────────┐
│ InputEventSpy        │  push(O(1))        │ prePaintScreen                │
│ pointerMotion(event) │ ────────────────►  │  按时间窗取快照（一次性）      │
│  · position          │  无锁有界环形缓冲   │  计算损伤 = 上帧 ∪ 本帧        │
│  · timestamp         │  (1024 槽, 预分配)  │  data.paint += 损伤（逻辑坐标）│
└──────────────────────┘                    └───────────────┬───────────────┘
                                                            │
                                            ┌───────────────▼───────────────┐
                                            │ paintScreen                   │
                                            │  effects->paintScreen(...) 先 │
                                            │  ShaderBinder(MapTexture)     │
                                            │  逐个 sample：MVP 平移 + render│
                                            │  混合状态保存/恢复             │
                                            └───────────────────────────────┘
```

文件划分：

| 文件 | 职责 |
| --- | --- |
| `src/trailsample.{h,cpp}` | 采样结构 + 环形缓冲 + 光标矩形几何。**不依赖 KWin/GL**，可单元测试 |
| `src/traileffect.{h,cpp}` | `Effect` + `InputEventSpy` 实现，GL 绘制、损伤管理、配置 |
| `src/main.cpp`, `src/metadata.json` | 插件工厂与内嵌元数据 |

---

## 4. 采样缓冲区设计

```cpp
class SampleRing {                 // 固定容量 1024（向上取 2 的幂，用掩码取模）
    void push(const Sample &) noexcept;                        // 生产者：O(1)，不分配
    std::size_t collect(TimeUs since, Sample *out, std::size_t maxOut) noexcept;
};
```

- **为什么要缓冲区**：KWin 只广播事件、不保存历史（§2.1），"过去一帧的采样"必须自己留存。
- **为什么有界 + 预分配**：8 kHz 鼠标下每帧会有上百个采样；用 `std::vector` 反复增长/分配会直接变成
  延迟与内存抖动，无界容器则内存无上限。
- **原子索引是防御性的，不是必需**：生产者与消费者今天都在主线程，用普通索引同样正确。
  保留 `acquire/release` 的成本是每事件 1 次 relaxed load + 1 次 release store（纳秒级），
  换来的是"正确性不依赖当前线程模型"：KWin 历史上输入曾在独立线程，spy 回调也可能被其它路径触发。
- **满了丢新采样，而不是覆盖**：绝不覆盖绘制方可能正在读的槽位（避免数据竞争）；
  被 `since` 过滤掉的旧采样在下次 `collect()` 时被丢弃，所以缓冲会立即恢复，
  `droppedSamples()` 可观测。丢采样也符合帧预算：每帧能画的指针数量本就有上限。
- **`maxOut` 保留最新**：候选超过上限时移动拷贝窗口，只保留**最新**的采样（它们离真实指针最近、最可见）。

### 时间戳策略

`pointerMotion` 优先使用事件自带的 `timestamp`（libinput 给的是 CLOCK_MONOTONIC 微秒），
若为 0、在未来、或超过 1 秒则回退为到达时刻。帧边界也读 `CLOCK_MONOTONIC`，两者可直接比较。
这条回退是必要的：X11 等后端的事件时间不是同一时钟。

---

## 5. 低延迟设计

| 措施 | 说明 |
| --- | --- |
| **不用额外线程** | 采样与绘制都在合成器主线程。跨线程只会引入唤醒/调度延迟，而延迟正是本效果最敏感指标 |
| **事件到达即入队** | `InputEventSpy` 在 `InputRedirection::processMotionInternal` 中同步回调，无排队、无 IPC |
| **当帧可见** | 采样在 T 时刻产生，第一个在 T 之后绘制的帧就会画出它，不额外延迟一帧 |
| **不做定时器轮询** | 不需要 `QTimer` 驱动；帧节奏由 KWin 的 RenderLoop/vblank 决定，`addRepaint` 只请求损伤 |
| **时间窗精确** | 默认 `TrailFrames=1` 时窗口起点就是**上一次绘制的时刻**，正好是"过去一帧" |
| **单帧快照** | `prePaintScreen` 一次性取快照，`paintScreen` 只画这批；绘制范围与损伤范围严格一致 |

---

## 6. 高性能设计

| 措施 | 效果 |
| --- | --- |
| **单帧一次快照，非每采样查询** | O(窗口内采样数)，典型 2–20 |
| **损伤最小化** | 只把 **上帧已画区域 ∪ 本帧将画区域** 加入 `data.paint`：旧轨迹被擦除，且不做全屏重绘 |
| **纹理复用** | 光标纹理仅在 `cacheKey()` 变化时上传；每帧 0 次上传是常态 |
| **顶点缓冲复用** | `GLTexture::render()` 内部缓存静态 VBO，尺寸不变则**不重新上传顶点** |
| **无稳态分配** | 采样数组与回读缓冲全部预分配复用 |
| **零锁** | 输入路径无互斥量，不会因锁竞争产生抖动 |
| **按需阻断直通扫描** | `blocksDirectScanout()` 只在**轨迹存在期间**返回 true；指针静止时全屏窗口仍可直通显示 |
| **GL 状态最小改动** | 只保存/恢复 blend 使能与 blend func，shader 用 `ShaderBinder` RAII push/pop |

每帧开销量级：`collect()` ≤ 1024 次比较；绘制 = N 次 uniform 设置 + N 次 draw call
（N = 鼠标采样率 ÷ 刷新率 × TrailFrames，典型 ≤ 20，上限由 `MaxSamples` 约束）。
按 1000 Hz 鼠标 / 60 Hz 屏幕 / TrailFrames=1 估算约 17 次 draw call/帧，像素填充才是主要成本。

---

## 7. 验证

### 7.1 单元测试（无需合成器）

`tests/trailsample_test.cpp`：容量取整、时间窗过滤（含边界包含）、消费后不重复返回、
`maxOut` 保留最新、满缓冲丢新采样与恢复、`reset`、hotspot 偏移矩形。

### 7.2 插件可发现性测试

`tests/plugin_metadata_test.cpp` 用 **KWin 自己的加载方式**
（`KPluginMetaData::findPlugins("kwin/effects/plugins")`）验证：id 必须是 `trail`（即文件名不能被加
`lib` 前缀）、名称/分类正确、且 `EnabledByDefault=false`。

### 7.3 嵌套合成器端到端测试（不触碰宿主会话）

`helpers/nested-e2e.sh`：
1. `dbus-run-session kwin_wayland --socket wayland-dev --width 1280 --height 720` 启动隔离实例，
   使用独立 `XDG_CONFIG_HOME`/`XDG_CACHE_HOME`，通过 `QT_PLUGIN_PATH` 指向 `~/.local` 插件。
2. `tools/fake-input-injector` 通过 KWin 自带的 `org_kde_kwin_fake_input` 协议注入指针路径
   （需要 `KWIN_WAYLAND_NO_PERMISSION_CHECKS=1`，因为该接口在 KWin 的安全黑名单里）。
3. `TRAIL_KWIN_SELFCHECK=1` 时，effect 在绘制前后 `glReadPixels` 同一区域并比较，
   统计真正被改写的像素数（因为截图路径绕过 effect，见 §2.7）。

实测结果（1280×720 输出、宿主分数缩放 1.75 → 2240×1260 设备像素）：

```
fake-input-injector: injected 1492 motion events
kwin_effect_trail: cursor image uploaded: QSize(56, 56) hotspot QPointF(4,4)
kwin_effect_trail: trail stats: 994.8 cursors/s, 119.9 frames/s, 8.30 cursors/frame, 0 dropped
kwin_effect_trail: selfcheck: pointer draw changed 231 of 3480 pixels in QRect(427,677 60x58)
```

可核对的三点：

- **时间窗正确**：注入 250 采样/s，渲染 120 fps → 2.08 采样/帧；`TrailFrames=4` → 8.30 指针/帧，
  与日志完全一致。
- **坐标正确**：损伤矩形最大到 `x≈2060`，落在 2240 宽的设备缓冲区里 → 逻辑→设备缩放（×1.75）正确。
- **确实写入像素**：单采样矩形约 60×58（32 逻辑像素 × 1.75 ≈ 56 加向外取整），
  且每帧都有像素被改写 → 绘制路径真实生效，而非只调用了 API。

---

## 8. 配置项

`~/.config/kwinrc`，组 `[Effect-trail]`：

| 键 | 默认 | 含义 |
| --- | --- | --- |
| `Enabled` | `true` | 运行期总开关（与 `[Plugins] trailEnabled` 独立） |
| `TrailFrames` | `1` | 采样保留多少个帧间隔；1 即"过去一帧"，调大得到更长的轨迹 |
| `MaxSamples` | `256` | 每帧最多绘制的指针数上限（帧预算保护） |

启用效果本身：`[Plugins] trailEnabled=true`（或 `helpers/enable-effect.sh enable`）。

---

## 9. 已知限制与后续优化

1. **多输出**：`LOGICAL`/设备坐标已按每输出独立状态处理（`QHash<LogicalOutput*, OutputFrameState>` 保存
   各自的采样快照、损伤与帧间隔），但尚未在多显示器与混合缩放环境下实测。
2. **绘制调用数**：目前每个采样一次 draw call（复用同一静态 VBO 与同一纹理）。
   若采样率极高可合并为**单次 draw call**：自建 `GLVertex2D` 顶点数组，
   `TextureMatrix` 用 `GLTexture::matrix(NormalizedCoordinates)`，但需要自行处理 Y 翻转。
3. **透明度与渐隐**：可加 `ShaderTrait::Modulate` + `Vec4Uniform::ModulationConstant`
   做整体透明度或旧采样渐隐（预乘纹理需同时缩放 RGB 与 A）。
4. **配置界面**：尚无 KCM，只能手改 kwinrc。
5. **`InputEventSpy` 的 API 稳定性**：非文档化 effect API，已隔离在单个回调中以便替换。
6. **`TRAIL_KWIN_SELFCHECK`** 诊断路径保留在代码中（默认关闭，构造时读一次环境变量）：
   它是目前唯一能自动验证覆盖层像素的手段，代价是开启时会有 `glReadPixels` 同步停顿。
