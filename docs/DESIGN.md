Pointer Trail -- Design
=======================

Requirements, what KWin 6.7.5 actually exposes, the resulting design, and how it was
verified. The KWin claims come from the installed 6.7.5 sources and headers
(Plasma 6.7.5 / KWin 6.7.5 / Qt 6.11.2 / KF6 6.30), not from documentation.


1. Requirement
--------------

An intra-frame trail for high-polling-rate mice: at every composited frame, collect all
pointer samples from the previous frame and render the current cursor image at each
position. Such a mouse reports far more positions per frame than the compositor draws,
and rendering the skipped ones improves the pointer's visual smoothness and click
accuracy. Low latency, high performance.


2. Findings that constrain the design
-------------------------------------

2.1 KWin exposes no pointer history

    EffectsHandler::cursorPos(), Effect::cursorPos()
        current position, latest value only

    EffectsHandler::mouseChanged(pos, oldpos, ...)
        per-event new/old position, no timestamp, also fires on button and
        modifier changes

    InputRedirection::m_lastPosition, takeLastPosition()
        one std::optional<QPointF> slot, for tablet sync, consuming:
        reading it clears it

No position queue exists. KWin keeps its own history where it needs one: mousemark
stores QList<QPointF> Mark, shakecursor feeds a ShakeDetector. The effect has to own
its buffer.

2.2 InputEventSpy beats mouseChanged

kwin/input.h and kwin/input_event_spy.h are installed headers, and input() is an inline
function in them. kwin's own hidecursor and shakecursor are

    class X : public Effect, public InputEventSpy

plus input()->installInputEventSpy(this).

    struct PointerMotionEvent {
        QPointF position, delta, deltaUnaccelerated;
        bool warp;
        Qt::MouseButtons buttons;
        Qt::KeyboardModifiers modifiers;
        std::chrono::microseconds timestamp;   // libinput CLOCK_MONOTONIC
    };

Compared with mouseChanged: a real event timestamp, so "the previous frame" is defined
in event time rather than arrival time; it fires only on actual motion; it carries
delta. It is not part of the documented effect API, so it is isolated in a single
callback to keep a fallback to mouseChanged cheap.

2.3 Sampling and drawing share one thread

    libinput is read on the main thread through a QSocketNotifier
    (backends/libinput/connection.cpp)

    events dispatch synchronously to InputRedirection::globalPointerChanged and then
    Effect::pointerMotion

    frame drawing is on the main thread too, where the GL context is made current
    (scene/workspacescene.cpp)

    plugin loading is synchronous: PluginEffectLoader uses
    KPluginMetaData::findPlugins, and only scripted effect discovery goes through
    QtConcurrent

No extra thread. See section 5.

2.4 The drawing contract

    void Effect::prePaintScreen(ScreenPrePaintData &data);
        // data.paint is in logical coordinates

    void Effect::paintScreen(const RenderTarget &, const RenderViewport &,
                             int mask, const Region &deviceRegion, LogicalOutput *);
        // deviceRegion is in device coordinates

Coordinate spaces are the trap here.

    ScreenPrePaintData::paint is converted by the scene:
    deviceDamage = view->mapToDeviceCoordinatesAligned(data.paint) & deviceRect().
    Add logical rects to data.paint.

    viewport.projectionMatrix() operates on device pixels; logical coordinates have to
    be multiplied by viewport.scale(). kwin's mouseclick does (x + cx) * scale.

    Effect chain order: the effect called first draws last, because every effect
    delegates onwards through effects->paintScreen(). Returning 0 from
    requestedEffectChainPosition() therefore means "called first, drawn on top",
    matching mouseclick and touchpoints. Drawing happens after the delegation, so the
    trail sits above windows.

    Cursor geometry, copied from scene/cursoritem.cpp to stay aligned with the real
    cursor: position - hotspot, size image.size() / devicePixelRatio().

2.5 Textures and blending

    GLTexture::upload(QImage) sets the content transform to OutputTransform::FlipY and
    GLTexture::render() handles the texture matrix and the Y flip, the same path
    magnifier uses, so no hand-rolled texcoords.

    Cursor images upload as premultiplied ARGB and have to blend premultiplied:
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA), matching the scene renderer
    (scene/itemrenderer_opengl.cpp). The wrong function leaves dark fringes.

    The image uploads only when it changes (QImage::cacheKey()), for example when the
    pointer switches between arrow and text cursor.

2.6 Plugin contract

    kcoreaddons_add_plugin(trail SOURCES ... INSTALL_NAMESPACE "kwin/effects/plugins")
    requires BUILD_SHARED_LIBS=ON; otherwise it builds a static plugin that cannot be
    loaded.

    Factory macro: KWIN_EFFECT_FACTORY_SUPPORTED(TrailEffect, "metadata.json",
    return effects->isOpenGLCompositing();).

    The plugin id is the file name, not a metadata field (KDE warns when metadata sets
    Id). The file has to be trail.so, not libtrail.so, hence
    set_target_properties(trail PROPERTIES PREFIX ""), and kwinrc uses
    [Plugins] trailEnabled=true.

    ABI: the headers state that effect plugins must be compiled against the same
    kwineffects version as kwin (6.7.5 against kwin-devel 6.7.5). The actual guard is
    the factory IID, which embeds the full version:

        EffectPluginFactory_iid = "org.kde.kwin.EffectPluginFactory"
                                  KWIN_PLUGIN_VERSION_STRING

    PluginEffectLoader::factory() compares it against the metadata and, on a mismatch,
    logs one qCDebug and returns nullptr without calling loader.instance(). Neither the
    factory nor createEffect() runs. With the IID rewritten to 6.7.4, kwin logs
    "has not matching plugin version" and "Couldn't get an EffectPluginFactory", other
    effects load, and the compositor does not crash. The log is qCDebug(KWIN_CORE),
    invisible by default, and because discovery is metadata-only the effect still shows
    up in the list with isEffectSupported() false.

    kcoreaddons_add_plugin installs into ${KDE_INSTALL_PLUGINDIR}/kwin/effects/plugins.
    ECM only uses the qt6 subdirectory when the prefix equals Qt's prefix, so a user
    prefix needs KDE_INSTALL_PLUGINDIR=${KDE_INSTALL_LIBDIR}/qt6/plugins explicitly to
    land in ~/.local/lib64/qt6/plugins/kwin/effects/plugins/.

    AUTOMOC cannot see the JSON file name inside the macro, so editing metadata.json
    does not regenerate the embedded metadata. helpers/build.sh deletes the autogen
    directory when metadata.json is newer than the built plugin.

2.7 Screenshots and screencasts do not include the overlay

Spectacle and a screencast show the scene without the overlay. Automated pixel
verification therefore has to read back inside the effect (section 7).

2.8 Effects that interfere with observation

shakecursor is enabled by default (EnabledByDefault: true) and magnifies the pointer
during fast back-and-forth motion, which is exactly the motion that reveals a trail.
Nested sessions set shakecursorEnabled=false; helpers/run-nested.sh does it.


3. Architecture
---------------

    input/main thread                          main thread, once per frame
    +----------------------+               +---------------------------------+
    | InputEventSpy        |  push, O(1)   | prePaintScreen                  |
    | pointerMotion(event) | ------------> |  snapshot by time window        |
    |  position            |  lock-free    |  damage = prev frame U this one |
    |  timestamp           |  ring, 1024   |  data.paint += damage (logical) |
    +----------------------+               +----------------+----------------+
                                                            |
                                           +----------------v----------------+
                                           | paintScreen                     |
                                           |  effects->paintScreen(...)      |
                                           |  ShaderBinder(MapTexture)       |
                                           |  per sample: MVP translate, draw|
                                           |  save/restore blend state       |
                                           +---------------------------------+

    src/trailsample.{h,cpp}   sample struct, ring buffer, cursor rect geometry.
                              No KWin or GL dependencies, unit-testable.
    src/traileffect.{h,cpp}   Effect + InputEventSpy, GL drawing, damage, config
    src/main.cpp              plugin factory
    src/metadata.json         embedded metadata


4. Sample buffer
----------------

    class SampleRing {          // fixed capacity 1024, power of two, masked modulo
        void push(const Sample &) noexcept;   // producer: O(1), no allocation
        std::size_t collect(TimeUs since, Sample *out, std::size_t maxOut) noexcept;
    };

Why a buffer at all: kwin only broadcasts events and never stores history (2.1), so the
previous frame's samples have to be retained here.

Why bounded and preallocated: an 8 kHz mouse produces hundreds of samples per frame. A
growing std::vector is latency and memory churn; an unbounded container is unbounded
memory.

Atomic indices are defensive, not required. Producer and consumer both run on the main
thread today, so plain indices would be correct. Acquire/release costs one relaxed load
plus one release store per event, nanoseconds, and makes correctness independent of the
current threading model: kwin read input on a separate thread historically, and the spy
callback can be reached from other paths.

Full drops new samples instead of overwriting, so a slot the painter may be reading is
never overwritten. Samples older than since are discarded by the next collect(), so the
ring recovers immediately, and droppedSamples() makes it observable. Dropping also fits
the frame budget, since cursors per frame are capped anyway.

maxOut keeps the newest: when candidates exceed the cap, the copy window slides to
retain the newest samples, which are closest to the real pointer and most visible.

The window anchor is the last sample actually drawn, not the wall clock.
frame.lastCollect advances only when samples were really collected. If a frame cannot
draw because the cursor image is temporarily unavailable, its samples stay in the ring
and fall inside the window next frame instead of being dropped silently. The anchor has
to be a sample timestamp rather than the frame-start CLOCK_MONOTONIC: when the
compositor starts a frame, events may still sit in the input queue and be pushed to the
buffer after collect(), with timestamps earlier than the wall-clock reading, and a
wall-clock anchor would exclude them, reliably dropping one sample per frame. Samples
enqueue in arrival order, so anything arriving after the newest sample drawn is newer,
and a sample-timestamp anchor cannot miss. The window start is computed by
Trail::samplingWindowStart(), a pure function and unit-testable.

Timestamps. pointerMotion prefers the event timestamp (libinput gives CLOCK_MONOTONIC
microseconds) and falls back to arrival time when it is zero, in the future, or more
than a second old. Frame boundaries read CLOCK_MONOTONIC too, so the two are directly
comparable. The fallback is needed because X11 and other backends do not use the same
clock.


5. Low latency
--------------

    no extra thread           sampling and drawing on the compositor main thread; a
                              thread hop only adds wake-up and scheduling latency,
                              the metric this effect is most sensitive to

    enqueue on arrival        InputEventSpy is called synchronously from
                              InputRedirection::processMotionInternal, no queue and
                              no IPC

    visible in the same frame a sample taken at T is drawn by the first frame painted
                              after T, with no extra frame of delay

    no timer polling          no QTimer; cadence comes from kwin's render loop and
                              vblank, addRepaint only requests damage

    exact time window         at TrailFrames=1 the window start is the previous paint,
                              which is exactly the previous frame

    single per-frame snapshot prePaintScreen snapshots once and paintScreen draws only
                              that batch, so the drawn area and the damage are the same


6. Performance
--------------

    one snapshot per frame    O(samples in window), typically 2 to 20, instead of a
                              query per sample

    minimal damage            data.paint gets the previous frame's rects unioned with
                              this frame's, so the old trail is erased and nothing
                              repaints full-screen

    cursor image on demand    motion does not touch cursorImage(); the image is fetched
                              and uploaded only when content or geometry changes

    texture reuse             uploads only on content change; zero uploads while the
                              pointer is still or moves within one shape

    vertex buffer reuse       GLTexture::render() caches a static VBO, so an unchanged
                              size means no vertex re-upload

    no steady-state alloc     sample arrays and readback buffers are preallocated
                              and reused

    lock-free                 no mutex on the input path, so no contention jitter

    scanout blocked briefly   blocksDirectScanout() returns true only while a trail
                              exists; a fullscreen window still scans out directly
                              when the pointer is still

    minimal GL state churn    only blend enable and blend function are saved and
                              restored; shaders go through ShaderBinder RAII push/pop

Per-frame cost: collect() is at most 1024 comparisons; drawing is N uniform updates and
N draw calls, where N = sample rate / refresh rate * TrailFrames, typically at most 20
and capped by MaxSamples. At 1000 Hz, 60 Hz and TrailFrames=1 that is about 17 draw
calls per frame; pixel fill dominates.

6.1 Damage and drawing must share one source

frame.damage from prePaintScreen() has to map pixel-for-pixel onto what paintScreen()
draws. Pixels outside the damage are neither cleared nor repainted and stay on screen
forever.

The first implementation refreshed the cursor image inside paintScreen(), after damage
was computed, which introduced a one-frame phase shift: damage used the old cursor
geometry, drawing used the new one. When the pointer changed shape or size mid-motion,
arrow to I-beam inside a text field, the part of the new geometry outside the old
damage was drawn into an unowned region while frame.previousDamage recorded the old
geometry. By the next frame the pointer was gone and the region was never repainted.
The symptom was a small cursor smear left behind at the moment the cursor changed
shape.

The fix, all three parts required:

    refresh the cursor image at the top of prePaintScreen(), before damage is computed,
    so damage and drawing use the same geometry within one frame

    re-fetch the image only when it actually changed, so correctness does not cost a
    per-frame fetch

    do not collect() in frames that cannot draw, leaving the samples for the next frame;
    see the window anchor in section 4

Invariant: frame.previousDamage equals exactly the rects drawn last frame, so
damage = previousDamage U this frame's rects always covers every pixel ever drawn.


7. Verification
---------------

No automated tests. Verification means running it.

    helpers/run-nested.sh starts an isolated nested kwin with its own D-Bus, config and
    socket, host untouched, logs on the terminal. TRAIL_FRAMES, CLIENT and
    WIDTH/HEIGHT come from the environment.

    TRAIL_KWIN_SELFCHECK=1 makes the effect glReadPixels the same region before and
    after drawing and count the changed pixels. Captures do not include the overlay
    (2.7), so this is the only automatic proof that pixels were written.

Measured once on a 1280x720 output with a host fractional scale of 1.75, so 2240x1260
device pixels:

    kwin_effect_trail: cursor image uploaded: QSize(56, 56) hotspot QPointF(4,4)
    kwin_effect_trail: trail stats: 994.8 cursors/s, 119.9 frames/s, 8.30 cursors/frame, 0 dropped
    kwin_effect_trail: selfcheck: pointer draw changed 231 of 3480 pixels in QRect(427,677 60x58)

Three things to check against it:

    time window     250 samples/s injected at 120 fps gives 2.08 samples/frame;
                    TrailFrames=4 gives 8.30 cursors/frame, matching the log

    coordinates     damage rects reach x=2060, inside the 2240-wide device buffer, so
                    the logical to device scaling of 1.75 is right

    pixels written  a one-sample rect is about 60x58 (32 logical px * 1.75 = 56,
                    rounded outward), and pixels changed every frame, so the draw path
                    really executes


8. Configuration
----------------

~/.config/kwinrc, group [Effect-trail]:

    Enabled       default true    runtime switch, independent of [Plugins] trailEnabled
    TrailFrames   default 1       frame intervals of samples to keep
    MaxSamples    default 256     per-frame cursor cap

Enabling the effect itself: [Plugins] trailEnabled=true, or
helpers/enable-effect.sh enable.


9. Known limits and future work
-------------------------------

1. Multiple outputs. Logical and device coordinates are already handled per output
   (QHash<LogicalOutput*, OutputFrameState> holds each output's snapshot, damage and
   frame interval), but multi-monitor and mixed-scaling setups are untested.

2. Draw call count. One draw call per sample, sharing a static VBO and one texture. At
   very high sample rates this can collapse into a single draw call with a custom
   GLVertex2D array and a TextureMatrix from GLTexture::matrix(NormalizedCoordinates),
   at the cost of handling the Y flip manually.

3. Fading. ShaderTrait::Modulate plus Vec4Uniform::ModulationConstant allows global
   opacity or fading older samples; premultiplied textures require scaling RGB and A
   together.

4. No KCM. Configuration is kwinrc-only.

5. InputEventSpy API stability. Undocumented effect API, isolated in one callback so it
   can be swapped. cursorGeometryChanged() likewise uses Cursors::self() from
   kwin/cursor.h; it and effects->cursorImage() are installed headers, but a kwin
   change means a coordinated update.

6. TRAIL_KWIN_SELFCHECK stays in the code, off by default, the environment read once at
   construction. It is the only automatic way to verify overlay pixels, at the cost of
   a synchronous glReadPixels stall.
