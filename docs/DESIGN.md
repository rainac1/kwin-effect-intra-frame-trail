# Pointer Trail — Design

Requirements, what KWin 6.8.x actually exposes, the resulting design, and how it was verified. The API claims were checked against the installed 6.8.3 headers (Plasma 6.8.3 / KWin 6.8.3 / Qt 6.11.2 / KF6 6.32) and verified by loading the plugin in a nested 6.8.x compositor, not from documentation.

## 1. Requirement

An intra-frame trail for high-polling-rate mice: at every composited frame, collect all pointer samples from the previous frame and render the current cursor image at each position. Such a mouse reports far more positions per frame than the compositor draws, and rendering the skipped ones improves the pointer's visual smoothness and click accuracy—with low latency and high performance.

## 2. Findings that constrain the design

### 2.1 KWin exposes no pointer history

- `EffectsHandler::cursorPos()`, `Effect::cursorPos()`: current position, latest value only.
- `EffectsHandler::mouseChanged(pos, oldpos, ...)`: per-event new/old position, no timestamp, and also fires on button and modifier changes.
- `InputRedirection::m_lastPosition`, `takeLastPosition()`: one `std::optional<QPointF>` slot for tablet sync; consuming (reading it clears it).

No position queue exists. KWin keeps its own history where it needs one (`mousemark` stores `QList<QPointF> Mark`, `shakecursor` feeds a `ShakeDetector`). The effect must own its own buffer.

### 2.2 `InputEventSpy` beats `mouseChanged`

`kwin/input.h` and `kwin/input_event_spy.h` are installed headers, and `input()` is an inline function in them. KWin's own `hidecursor` and `shakecursor` use:

```cpp
class X : public Effect, public InputEventSpy
```

together with `input()->installInputEventSpy(this)`.

```cpp
struct PointerMotionEvent {
    QPointF position, delta, deltaUnaccelerated;
    bool warp;
    Qt::MouseButtons buttons;
    Qt::KeyboardModifiers modifiers;
    std::chrono::microseconds timestamp;   // libinput CLOCK_MONOTONIC
};
```

Compared with `mouseChanged`: it provides a real event timestamp, so "the previous frame" is defined in event time rather than arrival time; it fires only on actual motion; and it carries `delta`. It is not part of the documented effect API, so it is isolated in a single callback to keep a fallback to `mouseChanged` cheap.

### 2.3 Sampling and drawing share one thread

- `libinput` is read on the main thread through a `QSocketNotifier` (`backends/libinput/connection.cpp`).
- Events dispatch synchronously to `InputRedirection::globalPointerChanged` and then `Effect::pointerMotion`.
- Frame drawing runs on the main thread too, where the GL context is made current (`scene/workspacescene.cpp`).
- Plugin loading is synchronous: `PluginEffectLoader` uses `KPluginMetaData::findPlugins`, and only scripted effect discovery goes through `QtConcurrent`.

No extra thread is needed (see [Section 5](#5-low-latency)).

### 2.4 The drawing contract

```cpp
void Effect::prePaintScreen(ScreenPrePaintData &data);
// data.paint is in logical coordinates

bool Effect::paintScreen(const RenderTarget &, const RenderViewport &,
                         int mask, const Region &deviceRegion, LogicalOutput *);
// deviceRegion is in device coordinates
// returns false when the scene could not be painted (e.g. GPU reset)
```

- **Coordinate spaces** are the main trap:
  - `ScreenPrePaintData::paint` is converted by the scene: `deviceDamage = view->mapToDeviceCoordinatesAligned(data.paint) & deviceRect()`. Add logical rects to `data.paint`.
  - `viewport.projectionMatrix()` operates on device pixels; logical coordinates have to be multiplied by `viewport.scale()`. KWin's `mouseclick` does `(x + cx) * scale`.
- **Effect chain order:** The effect called first draws last, because every effect delegates onwards through `effects->paintScreen()`. Returning `0` from `requestedEffectChainPosition()` therefore means "called first, drawn on top", matching `mouseclick` and `touchpoints`. Drawing happens after the delegation, so the trail sits above windows.
- **Failure propagation:** Since 6.8, `EffectsHandler::paintScreen()` and `Effect::paintScreen()` return `bool`: `false` means the scene could not be painted (for example, after a GPU reset), and every effect must stop rendering immediately. The delegation result is therefore checked before drawing and returned to the caller; otherwise the trail would be drawn into a target the scene never reached.
- **Cursor geometry:** Copied from `scene/cursoritem.cpp` to stay aligned with the real cursor: `position - hotspot`, size `image.size() / devicePixelRatio()`.
- **Color management:** The cursor image is display-referred sRGB: `ShapeCursorSource` returns a theme image, and for a client-provided cursor `Application::cursorImage() -> grabCursorOpenGL()` renders the cursor item into a `RenderTarget` that defaults to `ColorDescription::sRGB` and reads it back. The scene renderer converts an item from its own color description into `renderTarget.colorDescription()` (`scene/itemrenderer_opengl.cpp`), so the trail must do the same or it bypasses the display brightness, Night Light, and HDR mapping that the target description carries.

### 2.5 Textures and blending

- `GLTexture::upload(QImage)` sets the content transform to `OutputTransform::FlipY`, and `GLTexture::render()` handles the texture matrix and the Y flip—the same path `magnifier` uses, so no hand-rolled texcoords are needed.
- Cursor images upload as premultiplied ARGB and must blend premultiplied: `glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA)`, matching the scene renderer (`scene/itemrenderer_opengl.cpp`). The wrong blend function leaves dark fringes.
- The image uploads only when it changes (`QImage::cacheKey()`), for example when the pointer switches between arrow and text cursor.
- Color management is part of the draw, not the upload. The shader uses `ShaderTrait::MapTexture | ShaderTrait::TransformColorspace` and every draw sets `shader->setColorspaceUniforms(ColorDescription::sRGB, renderTarget.colorDescription(), RenderingIntent::Perceptual)`, the same pattern `startupfeedback` and `offscreeneffect` use. Drawing with a plain `MapTexture` shader wrote raw sRGB into the target, ignoring display brightness (`CreateColorDescription()` folds brightness, dimming, and HDR into the output's reference luminance, and only converting to the target's description applies it). On an HDR output, the untransformed write was also far too dark.

### 2.6 Plugin contract

- `kcoreaddons_add_plugin(trail SOURCES ... INSTALL_NAMESPACE "kwin/effects/plugins")` requires `BUILD_SHARED_LIBS=ON`; otherwise it builds a static plugin that cannot be loaded.
- Factory macro: `KWIN_EFFECT_FACTORY_SUPPORTED(TrailEffect, "metadata.json", return effects->isOpenGLCompositing();)`.
- The plugin ID is the file name, not a metadata field (KDE warns when `metadata.json` sets `Id`). The file must be `trail.so`, not `libtrail.so`, hence `set_target_properties(trail PROPERTIES PREFIX "")`, and `kwinrc` uses `[Plugins] trailEnabled=true`.
- **ABI & version guard:** The headers state that effect plugins must be compiled against the same `kwineffects` version as KWin (e.g., `6.8.3` against `kwin-devel` `6.8.3`). The actual guard is the factory IID, which embeds the full version:
  ```cpp
  EffectPluginFactory_iid = "org.kde.kwin.EffectPluginFactory"
                            KWIN_PLUGIN_VERSION_STRING
  ```
  `PluginEffectLoader::factory()` compares it against the metadata and, on a mismatch, logs one `qCDebug` and returns `nullptr` without calling `loader.instance()`. Neither the factory nor `createEffect()` runs. With the IID rewritten to an older point release, KWin logs `"has not matching plugin version"` and `"Couldn't get an EffectPluginFactory"`, other effects load, and the compositor does not crash. The log is `qCDebug(KWIN_CORE)` (invisible by default), and because discovery is metadata-only the effect still shows up in the list with `isEffectSupported()` returning `false`.
- `kcoreaddons_add_plugin` installs into `${KDE_INSTALL_PLUGINDIR}/kwin/effects/plugins`. ECM only appends the `qt6` component when the prefix equals Qt's prefix, so the project sets `KDE_INSTALL_PLUGINDIR=${KDE_INSTALL_LIBDIR}/qt6/plugins` explicitly to land in `/usr/lib64/qt6/plugins/kwin/effects/plugins/`.
- AUTOMOC cannot see the JSON file name inside the macro, so editing `metadata.json` does not regenerate the embedded metadata; delete `build/src/trail_autogen` when `metadata.json` is newer than the built plugin.

### 2.7 Screenshots and screencasts do not include the overlay

Spectacle and screencasts show the scene without the overlay. Automated pixel verification therefore has to read back inside the effect ([Section 7](#7-verification)).

### 2.8 Effects that interfere with observation

`shakecursor` is enabled by default (`EnabledByDefault: true`) and magnifies the pointer during fast back-and-forth motion, which is exactly the motion that reveals a trail. Nested sessions set `shakecursorEnabled=false` in their own `kwinrc` ([Section 7](#7-verification)).

## 3. Architecture

```text
input/main thread                          main thread, once per frame, per output
+----------------------+               +---------------------------------+
| InputEventSpy        |  push, O(1)   | prePaintScreen                  |
| pointerMotion(event) | ------------> |  snapshot by time window        |
|  position            |  lock-free    |  damage = prev frame U this one |
|  timestamp           |  ring per out |  data.paint += damage (logical) |
+----------------------+               +----------------+----------------+
                                                        |
                                       +----------------v----------------+
                                       | paintScreen                     |
                                       |  effects->paintScreen(...)      |
                                       |  ShaderBinder(MapTexture        |
                                       |    + TransformColorspace)       |
                                       |  setColorspaceUniforms(sRGB,    |
                                       |    target)                      |
                                       |  per sample: MVP translate, draw|
                                       |  save/restore blend state       |
                                       +---------------------------------+
```

Every output is painted in its own pass, with its own viewport, scale, and damage region, so every output owns a `SampleRing` and an `OutputFrameState` instead of sharing one. `pointerMotion()` feeds all of them.

| File | Responsibility |
| :--- | :--- |
| `src/trailsample.{h,cpp}` | Sample struct, ring buffer, cursor rect geometry (no KWin or GL dependencies; unit-testable) |
| `src/traileffect.{h,cpp}` | `Effect` + `InputEventSpy`, GL drawing, damage, configuration |
| `src/main.cpp` | Plugin factory |
| `src/metadata.json` | Embedded metadata |

## 4. Sample buffer

```cpp
class SampleRing {          // fixed capacity 1024, power of two, masked modulo
    void push(const Sample &) noexcept;   // producer: O(1), no allocation
    std::size_t collect(TimeUs since, Sample *out, std::size_t maxOut) noexcept;
};
```

- **Why a buffer at all:** KWin only broadcasts events and never stores history ([2.1](#21-kwin-exposes-no-pointer-history)), so the previous frame's samples have to be retained here.
- **Why bounded and preallocated:** An 8 kHz mouse produces hundreds of samples per frame. A growing `std::vector` causes latency and memory churn; an unbounded container risks unbounded memory growth.
- **Atomic indices are defensive, not required:** Producer and consumer both run on the main thread today, so plain indices would be correct. Acquire/release costs one relaxed load plus one release store per event (nanoseconds), and makes correctness independent of the current threading model: KWin read input on a separate thread historically, and the spy callback can be reached from other paths.
- **Overflow policy:** When full, new samples are dropped instead of overwriting slots the painter may be reading. Samples older than `since` are discarded by the next `collect()`, so the ring recovers immediately, and `droppedSamples()` makes it observable. Dropping also fits the frame budget, since cursors per frame are capped anyway.
- **`maxOut` keeps the newest:** When candidates exceed the cap, the copy window slides to retain the newest samples, which are closest to the real pointer and most visible.
- **Window anchor:** The anchor is the last sample actually drawn, not the wall clock. `frame.lastCollect` advances only when samples were really collected. If a frame cannot draw because the cursor image is temporarily unavailable, its samples stay in the ring and fall inside the window next frame instead of being dropped silently. The anchor must be a sample timestamp rather than the frame-start `CLOCK_MONOTONIC`: when the compositor starts a frame, events may still sit in the input queue and be pushed to the buffer after `collect()`, with timestamps earlier than the wall-clock reading, and a wall-clock anchor would exclude them—reliably dropping one sample per frame. Samples enqueue in arrival order, so anything arriving after the newest sample drawn is newer, and a sample-timestamp anchor cannot miss. With the trail fixed to one frame, that anchor is also the window start: there is nothing to widen it by.
- **Timestamps:** `pointerMotion` prefers the event timestamp (`libinput` gives `CLOCK_MONOTONIC` microseconds) and falls back to arrival time when it is zero, in the future, or more than a second old. Frame boundaries read `CLOCK_MONOTONIC` too, so the two are directly comparable. The fallback is needed because X11 and other backends do not use the same clock.

## 5. Low latency

| Property | Mechanism |
| :--- | :--- |
| No extra thread | Sampling and drawing run on the compositor main thread; a thread hop only adds wake-up and scheduling latency, the metric this effect is most sensitive to |
| Enqueue on arrival | `InputEventSpy` is called synchronously from `InputRedirection::processMotionInternal`, with no queue and no IPC |
| Visible in the same frame | A sample taken at `T` is drawn by the first frame painted after `T`, with no extra frame of delay |
| No timer polling | No `QTimer`; cadence comes from KWin's render loop and vblank, and `addRepaint` only requests damage |
| Exact time window | The window starts at the newest sample drawn last frame, so every sample is drawn in the very next frame |
| Single per-frame snapshot | `prePaintScreen` snapshots once and `paintScreen` draws only that batch, so the drawn area and the damage match |

## 6. Performance

| Property | Mechanism |
| :--- | :--- |
| One snapshot per frame | `O(samples in window)`, typically 2 to 20, instead of a query per sample |
| Minimal damage | `data.paint` gets the previous frame's rects unioned with this frame's, so the old trail is erased without full-screen repaints |
| Cursor image on demand | Motion does not touch `cursorImage()`; the image is fetched and uploaded only when content or geometry changes |
| Texture reuse | Uploads only on content change; zero uploads while the pointer is still or moves within one shape |
| Vertex buffer reuse | `GLTexture::render()` caches a static VBO, so an unchanged size means no vertex re-upload |
| No steady-state allocation | Sample arrays and readback buffers are preallocated and reused |
| Lock-free | No mutex on the input path, so no contention jitter |
| Scanout blocked briefly | `blocksDirectScanout()` returns `true` only for the frames that draw or erase a trail ([6.3](#63-direct-scanout-is-blocked-per-frame-not-by-a-timer)), not for a time window after the last sample; a still pointer leaves a fullscreen window on its own plane |
| Minimal GL state churn | Only blend enable and blend function are saved and restored; shaders go through `ShaderBinder` RAII push/pop |

**Per-frame cost:** `collect()` performs at most 1024 comparisons; drawing is `N` uniform updates and `N` draw calls, where `N = sample rate / refresh rate`, typically at most 20 and capped by `MaxSamples`. At 1000 Hz and 60 Hz that is about 17 draw calls per frame; pixel fill dominates.

### 6.1 Damage and drawing must share one source

`frame.damage` from `prePaintScreen()` must map pixel-for-pixel onto what `paintScreen()` draws. Pixels outside the damage are neither cleared nor repainted and stay on screen forever.

The first implementation refreshed the cursor image inside `paintScreen()`, after damage was computed, which introduced a one-frame phase shift: damage used the old cursor geometry, while drawing used the new one. When the pointer changed shape or size mid-motion (e.g., arrow to I-beam inside a text field), the part of the new geometry outside the old damage was drawn into an unowned region while `frame.previousDamage` recorded the old geometry. By the next frame the pointer was gone and the region was never repainted, leaving behind a small cursor smear at the moment the cursor changed shape.

The fix requires all three parts:

1. Refresh the cursor image at the top of `prePaintScreen()`, before damage is computed, so damage and drawing use the same geometry within one frame.
2. Re-fetch the image only when it actually changed, so correctness does not cost a per-frame fetch.
3. Do not call `collect()` in frames that cannot draw, leaving the samples for the next frame (see the window anchor in [Section 4](#4-sample-buffer)).

**Invariant:** `frame.previousDamage` equals exactly the rects drawn last frame, so `damage = previousDamage U this frame's rects` always covers every pixel ever drawn.

### 6.2 The trail has to schedule its own erasing frame

The trail is drawn after `effects->paintScreen()`, on top of the scene, so the only thing that removes it is a later frame repainting the region it was drawn into. While the pointer moves that happens automatically: every motion event calls `addRepaint()` for the new cursor rect, and the frame it triggers repaints `frame.previousDamage` as well.

When the pointer stops, no further motion event arrives. The frame that the last event scheduled paints the trail, and nothing else produces damage afterwards—the compositor is damage-driven, and even the `blocksDirectScanout()` flip that ends the trail needs a frame before it can be evaluated. That frame is composited as well, because `frame.previousDamage` is still non-empty while the trail is on screen ([6.3](#63-direct-scanout-is-blocked-per-frame-not-by-a-timer)), so the erase cannot be given up in favour of a scanout. The last copy is drawn exactly where the real pointer is, so it composites over it and stays on screen as a bright edge around the pointer until something unrelated repaints that area.

`paintScreen()` therefore calls `effects->addRepaint(frame.damage)` after drawing. The next frame collects no samples, so `prePaintScreen()` puts `frame.previousDamage` into `data.paint` and `paintScreen()` returns before drawing, which erases the trail; no further repaint is requested, so the effect does not keep rendering on its own. The request is only made when a trail was really drawn, so a still pointer costs exactly one extra frame.

### 6.3 Direct scanout is blocked per frame, not by a timer

`blocksDirectScanout()` is not a hint. When any effect reports `true`, KWin discards every overlay, underlay, and scanout layer of that output and renders the whole scene into one buffer (`WorkspaceScene::layerCandidates()` returns only the container item). That pulls the pointer off the cursor plane and—the expensive part—a fullscreen window off its own plane.

A browser playing a fullscreen video is the worst case. Its video surface is a plane candidate when it is a `dmabuf`, opaque, and updating faster than 20 Hz (`isCandidate()`), and without a trail the output is presented by page flips while the compositor never reads the video at all. One composited frame turns that into a full-screen conversion of the video into the scene buffer, and going back to a plane discards the surface's texture and its accumulated damage (`prepareDirectScanout()`), so every switch costs an import and can present a stale frame. The restriction is inherent: KWin cannot blend composited content over a plane, so a trail that has to be visible above a fullscreen window always forces that window to be composited while the trail is on screen. The only thing the effect controls is how many frames that is.

The effect therefore answers for the frame it has just prepared. `prePaintScreen()` records the output whose frame has a non-empty `frame.damage` (the frame that draws the trail) or `frame.previousDamage` (the frame that still has to erase it), and `blocksDirectScanout()` returns exactly that: the frame that draws a trail and the frame that erases it again, and nothing once the pointer has been still for a frame.

The time window this replaced—a multiple of the smoothed paint interval after the last motion event—was wrong in both directions. That interval was measured between output passes, so a compositor that was idle while the video was scanned out inflated it and kept the video off its plane for many frames after the trail had already been erased. Conversely, an erasing frame that arrived after the window had expired was itself allowed to scan out, which dropped the repaint of `frame.previousDamage` and left the exact residue [6.2](#62-the-trail-has-to-schedule-its-own-erasing-frame) exists to prevent. Both issues disappear when answering for the frame that was just prepared instead.

A capture pass (screencast, screenshot) returns before the decision is taken, so it leaves the recorded output alone, and `paintScreen()` is only reached for output passes as well.

### 6.4 The trail is not drawn while VRR is in force

Adaptive sync runs the display at the refresh rate of the content on it: a fullscreen 60 fps video on a 120 Hz output refreshes at 60 Hz, and a game runs at its own rate. The frame being prepared carries that state in `OutputFrame::presentationMode()`, which is `AdaptiveSync` or `AdaptiveAsync` whenever KWin puts the output into adaptive sync for it. The effect reads that flag from `ScreenPrePaintData::frame` instead of inferring anything from the observed frame rate.

Nothing is drawn while it holds, because the trail is what makes the drop obvious: drawn at the content's rate, it steps visibly with it instead of smoothing the pointer, and it also forces whatever is underneath to be composited instead of scanned out ([6.3](#63-direct-scanout-is-blocked-per-frame-not-by-a-timer)). The effect stands back until the display is at its own rate again.

This is what makes the VRR policy matter. On **Always**, every frame is presented in adaptive sync mode, so the trail is never drawn; on **Automatic**, only the windows that ask for adaptive sync—typically games and fullscreen video players—put the output into that mode, and the trail is unaffected everywhere else. The README therefore tells users to leave it on **Automatic** or turn it **Off** rather than **Always**.

While it is off, samples are dropped instead of held: otherwise, the window they cover would span the entire time the trail was off, and the frame that resumes would flash a smear of positions the pointer passed through long ago.

The pointer stutters in that situation with the effect disabled as well—it is the refresh rate, not the trail—so this does not fix that; it keeps the effect out of the way of a display doing what adaptive sync asks of it.

## 7. Verification

There are no automated tests. Verification means running the plugin:

- A nested `kwin_wayland` under `dbus-run-session` with its own `XDG_CONFIG_HOME` and `QT_LOGGING_RULES` starts an isolated compositor with its own D-Bus, config, and socket—leaving the host untouched and printing logs to the terminal (see [USAGE.md](USAGE.md#nested-session)).
- `TRAIL_KWIN_SELFCHECK=1` makes the effect `glReadPixels` the same region before and after drawing and count the changed pixels. Captures do not include the overlay ([2.7](#27-screenshots-and-screencasts-do-not-include-the-overlay)), so this is the only automatic proof that pixels were written.

Measured once on a 1280×720 output with a host fractional scale of 1.75 (2240×1260 device pixels):

```text
kwin_effect_trail: cursor image uploaded: QSize(56, 56) hotspot QPointF(4,4)
kwin_effect_trail: trail stats: 994.8 cursors/s, 119.9 frames/s, 8.30 cursors/frame, 0 dropped
kwin_effect_trail: selfcheck: pointer draw changed 231 of 3480 pixels in QRect(427,677 60x58)
```

Three things to check against it:

- **Time window:** The log above reads `8.30 cursors/frame` while the host injected 250 samples/s at 120 fps (i.e., 2.08 per frame interval), because that earlier run still kept samples for several frame intervals. With the trail fixed to one frame, the stats should read about 2 cursors/frame at that input rate.
- **Coordinates:** Damage rects reach `x=2060`, inside the 2240-wide device buffer, confirming the logical-to-device scaling of `1.75`.
- **Pixels written:** A one-sample rect is about `60x58` (`32 logical px * 1.75 = 56`, rounded outward), and pixels changed every frame, confirming that the draw path executes.

## 8. Configuration

`~/.config/kwinrc`, group `[Effect-trail]`:

| Key | Default | Description |
| :--- | :--- | :--- |
| `Enabled` | `true` | Runtime switch, independent of `[Plugins] trailEnabled` |
| `MaxSamples` | `256` | Per-frame cursor cap |
| `SampleStride` | `1` | Keep one out of every `N` samples, drop the rest |
| `BlockDirectScanout` | `false` | Block direct scanout on active frames (`false` preserves hardware cursor plane and fullscreen direct scanout) |
| `Fade` | `true` | Enable dyadic fractal opacity across the frame's `n` cursors |
| `FadeBase` | `1.2446` | Floating-point base `b` (`1.0`–`16.0`) in `pow(b, ctz(k) - m)` (`2.0` gives `lowbit(k) / 2^m`) |

`SampleStride` is applied in `pointerMotion()`, before samples are pushed into the ring: the counter keeps one event out of every `N` and returns for the others, so dropped positions never reach a buffer and `prePaintScreen()` and the drawing code are unaffected. The whole input stream shares one counter, so every output keeps the same positions. A value below `1` is read as `1`, which keeps every sample.

Enabling the effect itself uses `[Plugins] trailEnabled=true` plus a `loadEffect` D-Bus call, or the checkbox under **System Settings → Window Management → Desktop Effects**.

## 9. Known limits and future work

1. **Multiple outputs:** Every output owns its own sample buffer and damage (`std::map<LogicalOutput*, OutputFrameState>`), because KWin paints each output in its own pass with its own viewport, scale, and damage region. A single shared ring let the output that happened to be painted first consume the samples of all the others, so only one monitor ever showed a trail. `pointerMotion()` pushes each sample into every known output's ring; entries are created in `reconfigure()` and on `screenAdded()`, and removed on `screenRemoved()`. Mixed-scaling setups still use one cursor texture, so a trail on an output whose scale differs from the one the pointer is on is drawn with that output's scale from a texture grabbed for the pointer's output; the geometry is correct, only the sampling resolution is not always optimal.
2. **Draw call count:** All samples in a frame are batched into a single `GLVertexBuffer` `GL_TRIANGLES` draw call (`GLVertex3D`, carrying per-sample opacity in `position.z`), using a custom shader that `#include "colormanagement.glsl"` and discards transparent texels before colorspace conversion. If compilation ever fails, the effect falls back to KWin's built-in shaders.
3. **Fractal opacity:** Controlled by `Fade` (default `true`) and floating-point `FadeBase` (default `1.2446`, clamped to `[1.0, 16.0]`). Within a frame of `n` cursors numbered `k = 0 .. n - 1` from newest to oldest (with the real pointer occupying `k = 0` at opacity `1.0`), `m = floor(log2(n - 1)) + 1 = bit_width(n - 1)` and cursor `k >= 1` receives opacity `pow(FadeBase, ctz(k) - m)` (`lowbit(k) / 2^m` for `FadeBase == 2.0`).
4. **No KCM:** Configuration is `kwinrc`-only.
5. **`InputEventSpy` API stability:** Undocumented effect API, isolated in one callback so it can be swapped. `cursorGeometryChanged()` and the zero-readback cursor paths use `Cursors::self()`, `ShapeCursorSource`, `SurfaceCursorSource`, and `WorkspaceScene::cursorItem()` from KWin's installed headers (`ShapeCursorSource` uses a 32-entry LRU texture cache; `SurfaceCursorSource` renders directly into a reusable GPU FBO with no `glReadPixels` stall, falling back to `effects->cursorImage()` if unavailable).
6. **Bottom edge with Edge in fullscreen on a fractional scale:** With `BlockDirectScanout=false` (the default), fullscreen clients stay on their own plane and the hardware cursor plane is preserved; the issue only applies when `BlockDirectScanout=true` is explicitly enabled.
7. **`TRAIL_KWIN_SELFCHECK`:** Remains in the code, off by default, with the environment variable read once at construction. It is the only automatic way to verify overlay pixels, at the cost of a synchronous `glReadPixels` stall.
8. **Newest sample overdraw:** Samples sitting at the exact current pointer position are omitted before damage calculation, and the batched fragment shader masks every remaining trail fragment by `(1.0 - headCursorAlpha)` using the cursor texture itself. On hardware-cursor setups (`BlockDirectScanout=false`), the real cursor also sits above the primary plane in display hardware.
9. **Fullscreen plane compositing:** `BlockDirectScanout` defaults to `false`, so fullscreen videos and games stay on zero-copy direct scanout and the hardware cursor stays on its own plane. Set `BlockDirectScanout=true` if you want the trail drawn above fullscreen direct-scanout windows (at the cost of pulling them and the cursor off their planes while moving).
