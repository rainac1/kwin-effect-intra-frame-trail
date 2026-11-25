/*
    Renders the pointer image at every position sampled during the previous
    frame. See docs/DESIGN.md for the reasoning behind the rendering and damage
    strategy.
*/

#include "traileffect.h"

#include <kwin/core/colorspace.h>
#include <kwin/core/output.h>
#include <kwin/core/renderbackend.h>
#include <kwin/core/rendertarget.h>
#include <kwin/core/renderviewport.h>
#include <kwin/cursor.h>
#include <kwin/cursorsource.h>
#include <kwin/effect/effecthandler.h>
#include <kwin/effect/globals.h>
#include <kwin/input.h>
#include <kwin/input_event.h>
#include <kwin/opengl/eglcontext.h>
#include <kwin/opengl/glframebuffer.h>
#include <kwin/opengl/glshader.h>
#include <kwin/opengl/glshadermanager.h>
#include <kwin/opengl/gltexture.h>
#include <kwin/scene/item.h>
#include <kwin/scene/itemrenderer.h>
#include <kwin/scene/scene.h>
#include <kwin/scene/workspacescene.h>

#include <KConfigGroup>
#include <KSharedConfig>
#include <QLoggingCategory>
#include <QMatrix4x4>
#include <QVector2D>
#include <QVector3D>
#include <QVector4D>

#include <algorithm>
#include <cmath>
#include <ctime>

Q_LOGGING_CATEGORY(TRAIL, "kwin_effect_trail")

namespace KWin
{

namespace
{

/** Assumed frame interval before the output refresh rate is known (60 Hz). */
constexpr Trail::TimeUs kDefaultFrameIntervalUs = 16667;
constexpr Trail::TimeUs kMinFrameIntervalUs = 1000;
constexpr Trail::TimeUs kMaxFrameIntervalUs = 100'000;

/**
 * Upper bound for a sample timestamp to be considered usable. libinput reports
 * CLOCK_MONOTONIC microseconds, but other backends (e.g. X11 events) may report
 * zero or a completely different clock; samples outside this window fall back
 * to the time at which the effect observed them.
 */
constexpr Trail::TimeUs kMaxPlausibleEventAgeUs = 1'000'000;

constexpr int kDefaultMaxSamples = 256;
constexpr int kDefaultSampleStride = 1;
constexpr bool kDefaultBlockDirectScanout = false;
constexpr bool kDefaultFadeEnabled = true;
constexpr double kDefaultFadeBase = 1.618;
constexpr double kMinFadeBase = 1.0;
constexpr double kMaxFadeBase = 16.0;
constexpr double kDefaultFadeOpticalDepth = 2.0;
constexpr double kMinFadeOpticalDepth = 0.0;
constexpr double kMaxFadeOpticalDepth = 16.0;
constexpr bool kDefaultFadeGammaCompensation = true;
constexpr double kDefaultFadeGamma = 2.2;
constexpr double kMinFadeGamma = 0.1;
constexpr double kMaxFadeGamma = 10.0;

constexpr std::size_t kRingCapacity = 1024;
constexpr std::size_t kMaxCachedTextures = 32;
constexpr int kMaxSurfaceCursorSize = 512;

/** Tolerance (logical px) for recognising a sample already covered by the real cursor. */
constexpr float kHeadMatchEpsilon = 0.01f;

/** Diagnostics are summarised at most this often. */
constexpr Trail::TimeUs kStatsIntervalUs = 2'000'000;

/**
 * Current time on the clock used by libinput for event timestamps, so that
 * sample timestamps and frame boundaries are directly comparable.
 */
Trail::TimeUs monotonicNowUs()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return Trail::TimeUs(ts.tv_sec) * 1'000'000 + Trail::TimeUs(ts.tv_nsec) / 1000;
}

/** KWin::Region operates on its own integer rectangle type. */
Rect toIntRect(const QRectF &rect)
{
    return RectF(rect).toAlignedRect();
}

Trail::TimeUs frameIntervalFromView(const RenderView *view)
{
    if (!view) {
        return kDefaultFrameIntervalUs;
    }
    const uint refreshRateMHz = view->refreshRate();
    if (refreshRateMHz == 0) {
        return kDefaultFrameIntervalUs;
    }
    return std::clamp<Trail::TimeUs>(1'000'000'000ULL / refreshRateMHz, kMinFrameIntervalUs, kMaxFrameIntervalUs);
}

bool isSameLogicalPoint(const Trail::Sample &sample, const QPointF &pos)
{
    return std::abs(sample.x - float(pos.x())) < kHeadMatchEpsilon
        && std::abs(sample.y - float(pos.y())) < kHeadMatchEpsilon;
}

Trail::FadeMode parseFadeMode(const QString &value)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized.isEmpty() || normalized == QLatin1String("geometric")) {
        return Trail::FadeMode::Geometric;
    }
    if (normalized == QLatin1String("beer-lambert")
        || normalized == QLatin1String("beerlambert")
        || normalized == QLatin1String("beer_lambert")) {
        return Trail::FadeMode::BeerLambert;
    }
    qCWarning(TRAIL) << "unknown FadeMode" << value << "; falling back to geometric";
    return Trail::FadeMode::Geometric;
}

} // namespace

TrailEffect::OutputFrameState::OutputFrameState()
    : ring(kRingCapacity)
{
}

TrailEffect::TrailEffect()
{
    // KWin's only way to observe individual pointer samples: it broadcasts the
    // current position but keeps no history, so we collect it ourselves. The
    // spy is uninstalled automatically when this object is destroyed.
    input()->installInputEventSpy(this);

    // Refresh cursor textures only when the compositor signals a change rather
    // than on every frame.
    connect(effects, &EffectsHandler::cursorShapeChanged, this, [this] {
        m_cursorDirty = true;
    });

    // One sample buffer and one frame state per output. The pointer is a single
    // global device, so every output has to observe the same samples, but each
    // one consumes them in its own paint pass and keeps its own damage.
    connect(effects, &EffectsHandler::screenAdded, this, [this](LogicalOutput *screen) {
        findOrCreateFrame(screen);
    });
    connect(effects, &EffectsHandler::screenRemoved, this, [this](LogicalOutput *screen) {
        std::erase_if(m_frames, [screen](const OutputEntry &entry) {
            return entry.screen == screen;
        });
        if (m_scanoutBlockOutput == screen) {
            m_scanoutBlockOutput = nullptr;
        }
        if (m_currentScreen == screen) {
            m_currentScreen = nullptr;
        }
    });

    m_vertices3D.reserve(std::size_t(kDefaultMaxSamples) * 6);
    m_vertices2D.reserve(std::size_t(kDefaultMaxSamples) * 6);

    m_selfCheck = qEnvironmentVariableIntValue("TRAIL_KWIN_SELFCHECK") == 1;
    reconfigure(ReconfigureAll);
}

TrailEffect::~TrailEffect()
{
    effects->makeOpenGLContextCurrent();
    m_trailShader.reset();
    m_surfaceCursorFbo.reset();
    m_surfaceCursorTexture.reset();
    m_textureCache.clear();
    m_activeCursorTexture = nullptr;
}

TrailEffect::OutputFrameState *TrailEffect::findFrame(LogicalOutput *screen)
{
    for (OutputEntry &entry : m_frames) {
        if (entry.screen == screen) {
            return entry.state.get();
        }
    }
    return nullptr;
}

const TrailEffect::OutputFrameState *TrailEffect::findFrame(LogicalOutput *screen) const
{
    for (const OutputEntry &entry : m_frames) {
        if (entry.screen == screen) {
            return entry.state.get();
        }
    }
    return nullptr;
}

TrailEffect::OutputFrameState &TrailEffect::findOrCreateFrame(LogicalOutput *screen)
{
    if (OutputFrameState *existing = findFrame(screen)) {
        return *existing;
    }
    auto state = std::make_unique<OutputFrameState>();
    OutputFrameState &ref = *state;
    m_frames.push_back(OutputEntry{
        .screen = screen,
        .state = std::move(state),
    });
    return ref;
}

double TrailEffect::maxOutputScale() const
{
    double maxScale = 1.0;
    for (const LogicalOutput *screen : effects->screens()) {
        if (screen) {
            maxScale = std::max(maxScale, double(screen->scale()));
        }
    }
    return maxScale;
}

void TrailEffect::reconfigure(ReconfigureFlags flags)
{
    Q_UNUSED(flags)

    const KConfigGroup config = KSharedConfig::openConfig()->group(QStringLiteral("Effect-trail"));
    m_enabled = config.readEntry("Enabled", true);
    m_maxSamples = std::clamp(config.readEntry("MaxSamples", kDefaultMaxSamples), 1, int(kRingCapacity));
    m_sampleStride = std::max(config.readEntry("SampleStride", kDefaultSampleStride), 1);
    m_blockDirectScanout = config.readEntry("BlockDirectScanout", kDefaultBlockDirectScanout);

    m_fade.enabled = config.readEntry("Fade", kDefaultFadeEnabled);
    m_fade.mode = parseFadeMode(config.readEntry("FadeMode", QStringLiteral("geometric")));
    m_fade.base = std::clamp(config.readEntry("FadeBase", kDefaultFadeBase), kMinFadeBase, kMaxFadeBase);
    m_fade.opticalDepth = std::clamp(config.readEntry("FadeOpticalDepth", kDefaultFadeOpticalDepth),
                                     kMinFadeOpticalDepth, kMaxFadeOpticalDepth);
    m_fade.gammaCompensation = config.readEntry("FadeGammaCompensation", kDefaultFadeGammaCompensation);
    m_fade.gamma = std::clamp(config.readEntry("FadeGamma", kDefaultFadeGamma), kMinFadeGamma, kMaxFadeGamma);
    m_levelOpacities = Trail::buildLevelOpacities(m_fade);

    // Outputs that already exist must have a buffer before the first motion
    // event, otherwise the first frame after enabling would have nothing to
    // draw. screenAdded() covers outputs that appear later.
    for (LogicalOutput *screen : effects->screens()) {
        findOrCreateFrame(screen);
    }

    if (m_enabled) {
        effects->addRepaintFull();
    }
}

void TrailEffect::pointerMotion(PointerMotionEvent *event)
{
    if (!m_enabled) {
        return;
    }

    // SampleStride drops the samples in between here, before they are buffered,
    // so the trail is built from every m_sampleStride-th position only. One
    // counter for the whole input stream, so every output sees the same points.
    if (++m_strideCounter < m_sampleStride) {
        return;
    }
    m_strideCounter = 0;

    const Trail::TimeUs now = monotonicNowUs();

    // Prefer the timestamp of the input event itself (libinput reports
    // CLOCK_MONOTONIC microseconds) and fall back to the arrival time when a
    // backend reports something unusable.
    Trail::TimeUs time = Trail::TimeUs(event->timestamp.count());
    if (time == 0 || time > now || now - time > kMaxPlausibleEventAgeUs) {
        time = now;
    }

    const Trail::Sample sample{
        .x = float(event->position.x()),
        .y = float(event->position.y()),
        .time = time,
    };

    const QRectF sampleRect = m_cursorShape.isValid()
        ? Trail::cursorRect(sample, m_cursorShape)
        : QRectF(sample.x - 1.0, sample.y - 1.0, 2.0, 2.0);
    const Rect intRect = toIntRect(sampleRect);

    // Push into every output's ring, and coalesce addRepaint() to at most once
    // per output per frame interval. prePaintScreen() adds the exact damage for
    // every collected sample, so waking the render loop once per frame is
    // enough even when the mouse reports at 8 kHz.
    bool needsRepaint = false;
    for (OutputEntry &entry : m_frames) {
        OutputFrameState &frame = *entry.state;
        frame.ring.push(sample);
        if (!frame.repaintPending && (!entry.screen || entry.screen->geometry().intersects(intRect))) {
            frame.repaintPending = true;
            needsRepaint = true;
        }
    }

    if (needsRepaint) {
        effects->addRepaint(sampleRect);
    }
}

void TrailEffect::prePaintScreen(ScreenPrePaintData &data)
{
    // Capture paths render the very same scene through the very same effect
    // chain, but into a target of their own (screencast, screenshot, color
    // picker, screen transform). Leave per-output state untouched for them.
    m_currentView = data.view;
    if (!isOutputRenderPass(data.screen)) {
        m_currentScreen = nullptr;
        effects->prePaintScreen(data);
        return;
    }
    m_currentScreen = data.screen;

    const Trail::TimeUs now = monotonicNowUs();
    OutputFrameState &frame = findOrCreateFrame(data.screen);
    frame.repaintPending = false;

    const double targetScale = std::max(maxOutputScale(), data.view ? double(data.view->scale()) : 1.0);

    // Refresh the cursor texture *before* computing damage so damage and
    // drawing always use the exact same cursor geometry within one frame.
    if (m_enabled && (m_cursorDirty || !m_activeCursorTexture || !m_cursorShape.isValid() || cursorGeometryChanged(targetScale))) {
        effects->makeOpenGLContextCurrent();
        refreshCursorShape(data.view, targetScale);
        m_cursorDirty = false;
    }

    // Snapshot geometry and head pointer position together with the samples.
    frame.cursorShape = m_cursorShape;
    Cursor *cursor = Cursors::self()->currentCursor();
    frame.hasHead = cursor && !Cursors::self()->isCursorHidden();
    frame.headPos = frame.hasHead ? cursor->pos() : QPointF();

    const bool vrrActive = data.frame && (data.frame->presentationMode() == PresentationMode::AdaptiveSync
                                          || data.frame->presentationMode() == PresentationMode::AdaptiveAsync);
    if (vrrActive != frame.vrrActive) {
        frame.vrrActive = vrrActive;
        qCDebug(TRAIL) << (vrrActive ? "trail off: the frame is presented with adaptive sync"
                                     : "trail on: adaptive sync is not in force");
    }

    const Trail::TimeUs frameIntervalUs = frameIntervalFromView(data.view);
    const bool drawable = canDraw();

    if (vrrActive) {
        frame.samples.clear();
        frame.ring.reset();
    } else if (drawable) {
        Trail::TimeUs since = frame.lastCollect;
        if (since == 0) {
            since = now > frameIntervalUs ? now - frameIntervalUs : 0;
        }

        frame.samples.resize(std::size_t(m_maxSamples));
        const std::size_t count = frame.ring.collect(since, frame.samples.data(), std::size_t(m_maxSamples));
        frame.samples.resize(count);

        if (!frame.samples.empty()) {
            frame.lastCollect = std::max(frame.lastCollect, frame.samples.back().time);

            // Drop only samples that sit at the exact same position as the
            // already-drawn real cursor, and samples outside this output's
            // logical bounds. All intermediate motion samples are kept raw.
            const Rect screenGeometry = data.screen->geometry();
            std::erase_if(frame.samples, [&](const Trail::Sample &sample) {
                if (frame.hasHead && isSameLogicalPoint(sample, frame.headPos)) {
                    return true;
                }
                return !screenGeometry.intersects(toIntRect(Trail::cursorRect(sample, frame.cursorShape)));
            });
        }
    } else {
        frame.samples.clear();
    }

    // Repaint both the cursors drawn last frame (so the old trail disappears)
    // and the ones drawn now.
    Region damage = frame.previousDamage;
    frame.damage = Region();
    for (const Trail::Sample &sample : frame.samples) {
        frame.damage += toIntRect(Trail::cursorRect(sample, frame.cursorShape));
    }
    damage |= frame.damage;
    if (!damage.isEmpty()) {
        data.paint += damage;
    }

    m_scanoutBlockOutput = (!frame.damage.isEmpty() || !frame.previousDamage.isEmpty()) ? data.screen : nullptr;

    effects->prePaintScreen(data);
}

bool TrailEffect::paintScreen(const RenderTarget &renderTarget,
                              const RenderViewport &viewport,
                              int mask,
                              const Region &deviceRegion,
                              LogicalOutput *screen)
{
    const bool painted = effects->paintScreen(renderTarget, viewport, mask, deviceRegion, screen);
    if (!painted) {
        return false;
    }

    if (!isOutputRenderPass(screen)) {
        return true;
    }

    OutputFrameState *framePtr = findFrame(screen);
    if (!framePtr) {
        return true;
    }
    OutputFrameState &frame = *framePtr;

    // The next frame has to repaint exactly the area painted now.
    const Region currentDamage = frame.damage;
    frame.previousDamage = currentDamage;
    frame.damage = Region();

    if (!m_enabled || !effects->isOpenGLCompositing() || frame.samples.empty()) {
        return true;
    }

    if (!m_activeCursorTexture || !frame.cursorShape.isValid()) {
        return true;
    }

    const double scale = viewport.scale();
    const QSizeF deviceSize = frame.cursorShape.size * scale;
    if (deviceSize.isEmpty()) {
        return true;
    }
    const QPointF deviceHotspot = frame.cursorShape.hotspot * scale;
    const QMatrix4x4 projection = viewport.projectionMatrix();

    selfCheckBefore(currentDamage, scale, renderTarget.size());

    const GLboolean blendWasEnabled = glIsEnabled(GL_BLEND);
    GLint previousSource = GL_ONE;
    GLint previousDestination = GL_ONE_MINUS_SRC_ALPHA;
    glGetIntegerv(GL_BLEND_SRC_RGB, &previousSource);
    glGetIntegerv(GL_BLEND_DST_RGB, &previousDestination);

    glEnable(GL_BLEND);
    // Cursor textures are premultiplied RGBA, matching KWin's scene renderer.
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    if (ensureTrailShader()) {
        const QMatrix4x4 texMatrix = m_activeCursorTexture->matrix(NormalizedCoordinates);
        const QVector2D uvTL = (texMatrix * QVector4D(0.0f, 0.0f, 0.0f, 1.0f)).toVector2D();
        const QVector2D uvTR = (texMatrix * QVector4D(1.0f, 0.0f, 0.0f, 1.0f)).toVector2D();
        const QVector2D uvBR = (texMatrix * QVector4D(1.0f, 1.0f, 0.0f, 1.0f)).toVector2D();
        const QVector2D uvBL = (texMatrix * QVector4D(0.0f, 1.0f, 0.0f, 1.0f)).toVector2D();

        const float width = float(deviceSize.width());
        const float height = float(deviceSize.height());

        const std::size_t sampleCount = frame.samples.size();
        // When the real cursor is visible it occupies k = 0 (opacity 1.0), so
        // the trail samples ordered newest-to-oldest take k = 1 .. n - 1.
        const std::size_t headOffset = frame.hasHead ? 1 : 0;
        const std::size_t totalCursors = sampleCount + headOffset;

        m_vertices3D.clear();
        m_vertices3D.reserve(sampleCount * 6);
        for (std::size_t i = 0; i < sampleCount; ++i) {
            const Trail::Sample &sample = frame.samples[i];
            const std::size_t k = (sampleCount - 1 - i) + headOffset;
            const float x0 = float(sample.x * scale - deviceHotspot.x());
            const float y0 = float(sample.y * scale - deviceHotspot.y());
            const float x1 = x0 + width;
            const float y1 = y0 + height;
            const float alpha = Trail::fractalSampleOpacity(k, totalCursors, m_levelOpacities);

            const GLVertex3D tl{QVector3D(x0, y0, alpha), uvTL};
            const GLVertex3D tr{QVector3D(x1, y0, alpha), uvTR};
            const GLVertex3D br{QVector3D(x1, y1, alpha), uvBR};
            const GLVertex3D bl{QVector3D(x0, y1, alpha), uvBL};

            m_vertices3D.push_back(tl);
            m_vertices3D.push_back(tr);
            m_vertices3D.push_back(br);
            m_vertices3D.push_back(tl);
            m_vertices3D.push_back(br);
            m_vertices3D.push_back(bl);
        }

        ShaderBinder shader(m_trailShader.get());
        m_trailShader->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, projection);
        m_trailShader->setUniform(GLShader::IntUniform::Sampler, 0);
        m_trailShader->setColorspaceUniforms(ColorDescription::sRGB, renderTarget.colorDescription(), RenderingIntent::Perceptual);

        if (m_locTextureMatrix >= 0) {
            m_trailShader->setUniform(m_locTextureMatrix, texMatrix);
        }
        const bool maskCursor = frame.hasHead && width > 0.0f && height > 0.0f;
        if (m_locMaskCursor >= 0) {
            m_trailShader->setUniform(m_locMaskCursor, maskCursor ? 1 : 0);
        }
        if (maskCursor) {
            const QVector2D headOrigin(float(frame.headPos.x() * scale - deviceHotspot.x()),
                                       float(frame.headPos.y() * scale - deviceHotspot.y()));
            const QVector2D invSize(1.0f / width, 1.0f / height);
            if (m_locCursorOrigin >= 0) {
                m_trailShader->setUniform(m_locCursorOrigin, headOrigin);
            }
            if (m_locCursorInvSize >= 0) {
                m_trailShader->setUniform(m_locCursorInvSize, invSize);
            }
        }

        m_activeCursorTexture->bind();
        GLVertexBuffer *vbo = GLVertexBuffer::streamingBuffer();
        vbo->reset();
        vbo->setVertices(m_vertices3D);
        vbo->render(GL_TRIANGLES);
        m_activeCursorTexture->unbind();
    } else {
        paintFallback(renderTarget, projection, frame, scale, deviceSize, deviceHotspot);
    }

    glBlendFunc(previousSource, previousDestination);
    if (!blendWasEnabled) {
        glDisable(GL_BLEND);
    }

    // Schedule the single erasing frame so the trail disappears as soon as the
    // pointer comes to rest.
    if (!currentDamage.isEmpty()) {
        effects->addRepaint(currentDamage);
    }

    selfCheckAfter();
    reportFrameStats(frame.samples.size());
    return true;
}

void TrailEffect::postPaintScreen()
{
    // If direct scanout bypassed paintScreen() for this output pass, drop the
    // unrendered per-frame damage so isActive() does not stay stuck true.
    if (m_currentScreen) {
        if (OutputFrameState *frame = findFrame(m_currentScreen)) {
            frame->damage = Region();
            frame->samples.clear();
        }
        m_currentScreen = nullptr;
    }
    effects->postPaintScreen();
}

void TrailEffect::paintFallback(const RenderTarget &renderTarget,
                                const QMatrix4x4 &projection,
                                const OutputFrameState &frame,
                                double scale,
                                const QSizeF &deviceSize,
                                const QPointF &deviceHotspot)
{
    if (!m_fade.enabled) {
        // Single batched draw call using KWin's built-in shader.
        const QMatrix4x4 texMatrix = m_activeCursorTexture->matrix(NormalizedCoordinates);
        const QVector2D uvTL = (texMatrix * QVector4D(0.0f, 0.0f, 0.0f, 1.0f)).toVector2D();
        const QVector2D uvTR = (texMatrix * QVector4D(1.0f, 0.0f, 0.0f, 1.0f)).toVector2D();
        const QVector2D uvBR = (texMatrix * QVector4D(1.0f, 1.0f, 0.0f, 1.0f)).toVector2D();
        const QVector2D uvBL = (texMatrix * QVector4D(0.0f, 1.0f, 0.0f, 1.0f)).toVector2D();

        const float width = float(deviceSize.width());
        const float height = float(deviceSize.height());

        m_vertices2D.clear();
        m_vertices2D.reserve(frame.samples.size() * 6);
        for (const Trail::Sample &sample : frame.samples) {
            const float x0 = float(sample.x * scale - deviceHotspot.x());
            const float y0 = float(sample.y * scale - deviceHotspot.y());
            const float x1 = x0 + width;
            const float y1 = y0 + height;

            const GLVertex2D tl{QVector2D(x0, y0), uvTL};
            const GLVertex2D tr{QVector2D(x1, y0), uvTR};
            const GLVertex2D br{QVector2D(x1, y1), uvBR};
            const GLVertex2D bl{QVector2D(x0, y1), uvBL};

            m_vertices2D.push_back(tl);
            m_vertices2D.push_back(tr);
            m_vertices2D.push_back(br);
            m_vertices2D.push_back(tl);
            m_vertices2D.push_back(br);
            m_vertices2D.push_back(bl);
        }

        ShaderBinder shader(ShaderTrait::MapTexture | ShaderTrait::TransformColorspace);
        shader.shader()->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, projection);
        shader.shader()->setColorspaceUniforms(ColorDescription::sRGB, renderTarget.colorDescription(), RenderingIntent::Perceptual);

        m_activeCursorTexture->bind();
        GLVertexBuffer *vbo = GLVertexBuffer::streamingBuffer();
        vbo->reset();
        vbo->setVertices(m_vertices2D);
        vbo->render(GL_TRIANGLES);
        m_activeCursorTexture->unbind();
        return;
    }

    ShaderBinder shader(ShaderTrait::MapTexture | ShaderTrait::Modulate | ShaderTrait::TransformColorspace);
    shader.shader()->setColorspaceUniforms(ColorDescription::sRGB, renderTarget.colorDescription(), RenderingIntent::Perceptual);
    const std::size_t sampleCount = frame.samples.size();
    const std::size_t headOffset = frame.hasHead ? 1 : 0;
    const std::size_t totalCursors = sampleCount + headOffset;
    for (std::size_t i = 0; i < sampleCount; ++i) {
        const Trail::Sample &sample = frame.samples[i];
        const std::size_t k = (sampleCount - 1 - i) + headOffset;
        const float alpha = Trail::fractalSampleOpacity(k, totalCursors, m_levelOpacities);
        QMatrix4x4 mvp = projection;
        mvp.translate(float(sample.x * scale - deviceHotspot.x()),
                      float(sample.y * scale - deviceHotspot.y()));
        shader.shader()->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, mvp);
        shader.shader()->setUniform(GLShader::Vec4Uniform::ModulationConstant, QVector4D(alpha, alpha, alpha, alpha));
        m_activeCursorTexture->render(Region::infinite(), deviceSize, false);
    }
}

bool TrailEffect::ensureTrailShader()
{
    if (m_trailShader) {
        return true;
    }
    if (m_trailShaderAttempted) {
        return false;
    }
    m_trailShaderAttempted = true;

    const EglContext *ctx = EglContext::currentContext();
    const bool isGLES = ctx && ctx->openglVersionString().startsWith("OpenGL ES");

    const QByteArray header = isGLES
        ? QByteArrayLiteral("#version 300 es\nprecision highp float;\n")
        : QByteArrayLiteral("#version 140\n");

    const QByteArray vertexSource = header + QByteArrayLiteral(R"(
in vec3 position;
in vec2 texcoord;

uniform mat4 modelViewProjectionMatrix;

out vec2 texcoord0;
out vec2 v_devicePos;
out float v_alpha;

void main()
{
    texcoord0 = texcoord;
    v_devicePos = position.xy;
    v_alpha = position.z;
    gl_Position = modelViewProjectionMatrix * vec4(position.xy, 0.0, 1.0);
}
)");

    const QByteArray fragmentSource = header + QByteArrayLiteral(R"(
#include "colormanagement.glsl"

uniform sampler2D sampler;
uniform mat4 u_textureMatrix;
uniform vec2 u_cursorOrigin;
uniform vec2 u_cursorInvSize;
uniform int u_maskCursor;

in vec2 texcoord0;
in vec2 v_devicePos;
in float v_alpha;

out vec4 fragColor;

void main()
{
    vec4 color = texture(sampler, texcoord0);
    if (color.a <= 0.0) {
        discard;
    }

    float mask = v_alpha;
    if (u_maskCursor != 0) {
        vec2 headUv = (v_devicePos - u_cursorOrigin) * u_cursorInvSize;
        if (headUv.x >= 0.0 && headUv.x <= 1.0 && headUv.y >= 0.0 && headUv.y <= 1.0) {
            vec2 mappedHeadUv = (u_textureMatrix * vec4(headUv, 0.0, 1.0)).xy;
            mask *= (1.0 - texture(sampler, mappedHeadUv).a);
        }
    }
    if (mask <= 0.0) {
        discard;
    }

    color = sourceEncodingToNitsInDestinationColorspace(color);
    color = nitsToDestinationEncoding(color);
    fragColor = color * mask;
}
)");

    m_trailShader = ShaderManager::instance()->loadShaderFromCode(vertexSource, fragmentSource);
    if (!m_trailShader) {
        qCWarning(TRAIL) << "failed to compile custom batched trail shader; falling back to built-in KWin shader";
        return false;
    }

    m_locTextureMatrix = m_trailShader->uniformLocation("u_textureMatrix");
    m_locCursorOrigin = m_trailShader->uniformLocation("u_cursorOrigin");
    m_locCursorInvSize = m_trailShader->uniformLocation("u_cursorInvSize");
    m_locMaskCursor = m_trailShader->uniformLocation("u_maskCursor");
    qCDebug(TRAIL) << "compiled custom batched trail shader (GLES:" << isGLES << ")";
    return true;
}

void TrailEffect::selfCheckBefore(const Region &logicalDamage, double scale, const QSize &targetSize)
{
    m_selfCheckRect = QRect();
    if (!m_selfCheck || logicalDamage.isEmpty()) {
        return;
    }

    const QRectF logical = QRectF(logicalDamage.boundingRect());
    const QRect rect = QRectF(logical.topLeft() * scale, logical.size() * scale).toAlignedRect()
        & QRect(QPoint(), targetSize);
    if (rect.isEmpty() || rect.width() * rect.height() > 1024 * 1024) {
        return;
    }
    m_selfCheckRect = rect;

    const std::size_t bytes = std::size_t(rect.width()) * std::size_t(rect.height()) * 4;
    m_selfCheckBefore.resize(bytes);
    m_selfCheckAfter.resize(bytes);

    GLint previousAlignment = 4;
    glGetIntegerv(GL_PACK_ALIGNMENT, &previousAlignment);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(rect.x(), rect.y(), rect.width(), rect.height(), GL_RGBA, GL_UNSIGNED_BYTE, m_selfCheckBefore.data());
    glPixelStorei(GL_PACK_ALIGNMENT, previousAlignment);
}

void TrailEffect::selfCheckAfter()
{
    if (!m_selfCheck || m_selfCheckRect.isEmpty()) {
        return;
    }

    GLint previousAlignment = 4;
    glGetIntegerv(GL_PACK_ALIGNMENT, &previousAlignment);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(m_selfCheckRect.x(), m_selfCheckRect.y(), m_selfCheckRect.width(), m_selfCheckRect.height(),
                 GL_RGBA, GL_UNSIGNED_BYTE, m_selfCheckAfter.data());
    glPixelStorei(GL_PACK_ALIGNMENT, previousAlignment);

    std::size_t changed = 0;
    for (std::size_t i = 0; i + 3 < m_selfCheckAfter.size(); i += 4) {
        if (m_selfCheckBefore[i] != m_selfCheckAfter[i]
            || m_selfCheckBefore[i + 1] != m_selfCheckAfter[i + 1]
            || m_selfCheckBefore[i + 2] != m_selfCheckAfter[i + 2]
            || m_selfCheckBefore[i + 3] != m_selfCheckAfter[i + 3]) {
            ++changed;
        }
    }

    const Trail::TimeUs now = monotonicNowUs();
    if (now - m_selfCheckReported >= kStatsIntervalUs) {
        m_selfCheckReported = now;
        const std::size_t total = std::size_t(m_selfCheckRect.width()) * std::size_t(m_selfCheckRect.height());
        qCDebug(TRAIL) << "selfcheck: pointer draw changed" << changed << "of" << total
                       << "pixels in" << m_selfCheckRect;
        if (changed == 0) {
            qCWarning(TRAIL) << "selfcheck: the pointer draw did not change any pixel";
        }
    }
}

bool TrailEffect::cursorGeometryChanged(double targetScale) const
{
    const Cursor *cursor = Cursors::self()->currentCursor();
    if (!cursor) {
        return true;
    }
    const CursorSource *source = cursor->source();
    if (source != m_cursorImageState.source
        || cursor->rect().size() != m_cursorImageState.size
        || cursor->hotspot() != m_cursorImageState.hotspot) {
        return true;
    }
    if (qobject_cast<const SurfaceCursorSource *>(source)
        && std::abs(targetScale - m_cursorImageState.surfaceScale) > 1e-3) {
        return true;
    }
    return false;
}

GLTexture *TrailEffect::getOrUploadCachedTexture(const QImage &image)
{
    const qint64 key = qint64(image.cacheKey());
    const QSize size = image.size();

    for (auto it = m_textureCache.begin(); it != m_textureCache.end(); ++it) {
        if (it->key == key && it->size == size) {
            if (std::next(it) != m_textureCache.end()) {
                std::rotate(it, std::next(it), m_textureCache.end());
            }
            return m_textureCache.back().texture.get();
        }
    }

    std::unique_ptr<GLTexture> texture = GLTexture::upload(image);
    if (!texture) {
        return nullptr;
    }
    texture->setFilter(GL_LINEAR);
    texture->setWrapMode(GL_CLAMP_TO_EDGE);

    if (m_textureCache.size() >= kMaxCachedTextures) {
        m_textureCache.erase(m_textureCache.begin());
    }
    GLTexture *raw = texture.get();
    m_textureCache.push_back(CachedTexture{
        .key = key,
        .size = size,
        .texture = std::move(texture),
    });
    qCDebug(TRAIL) << "cursor image uploaded to cache (" << m_textureCache.size() << "entries):"
                   << image.size() << "hotspot" << m_cursorShape.hotspot;
    return raw;
}

bool TrailEffect::renderCursorItemToTexture(const Cursor *cursor, RenderView *view, double scale)
{
    if (!cursor || !view) {
        return false;
    }
    WorkspaceScene *scene = effects->scene();
    Item *cursorItem = scene ? scene->cursorItem() : nullptr;
    ItemRenderer *renderer = (scene && view->renderDevice()) ? scene->renderer(view->renderDevice()) : nullptr;
    if (!cursorItem || !renderer || !cursorItem->isVisible()) {
        return false;
    }

    const RectF localBounds = cursorItem->boundingRect();
    if (localBounds.isEmpty()) {
        return false;
    }
    const RectF sceneBounds = cursorItem->mapToScene(localBounds);
    const QSize pixelSize(std::max(1, int(std::ceil(sceneBounds.width() * scale))),
                          std::max(1, int(std::ceil(sceneBounds.height() * scale))));
    if (pixelSize.width() > kMaxSurfaceCursorSize || pixelSize.height() > kMaxSurfaceCursorSize) {
        return false;
    }

    if (!m_surfaceCursorTexture || m_surfaceCursorTexture->size() != pixelSize || !m_surfaceCursorFbo || !m_surfaceCursorFbo->valid()) {
        m_surfaceCursorFbo.reset();
        m_surfaceCursorTexture = GLTexture::allocate(GL_RGBA8, pixelSize);
        if (!m_surfaceCursorTexture) {
            return false;
        }
        m_surfaceCursorTexture->setFilter(GL_LINEAR);
        m_surfaceCursorTexture->setWrapMode(GL_CLAMP_TO_EDGE);
        m_surfaceCursorFbo = std::make_unique<GLFramebuffer>(m_surfaceCursorTexture.get());
        if (!m_surfaceCursorFbo->valid()) {
            m_surfaceCursorFbo.reset();
            m_surfaceCursorTexture.reset();
            return false;
        }
        qCDebug(TRAIL) << "allocated GPU FBO for surface cursor:" << pixelSize << "scale" << scale;
    }

    // Use a logical renderRect matching pixelSize/scale so the rendered FBO
    // maps 1:1 with no fractional stretching when drawn.
    const QSizeF logicalSize(double(pixelSize.width()) / scale, double(pixelSize.height()) / scale);
    const RectF renderRect(sceneBounds.topLeft(), logicalSize);
    const RenderTarget target(m_surfaceCursorFbo.get(), ColorDescription::sRGB);
    const RenderViewport targetViewport(renderRect, scale, target, QPoint());

    const GLboolean scissorWasEnabled = glIsEnabled(GL_SCISSOR_TEST);
    if (scissorWasEnabled) {
        glDisable(GL_SCISSOR_TEST);
    }

    GLFramebuffer::pushFramebuffer(m_surfaceCursorFbo.get());
    renderer->beginFrame(target, targetViewport);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    const bool ok = renderer->renderItem(target, targetViewport, cursorItem, 0, Region::infinite(), WindowPaintData(), {}, {});
    renderer->endFrame();
    GLFramebuffer::popFramebuffer();

    if (scissorWasEnabled) {
        glEnable(GL_SCISSOR_TEST);
    }

    if (!ok) {
        return false;
    }

    m_cursorShape.size = logicalSize;
    m_cursorShape.hotspot = cursorItem->position() - sceneBounds.topLeft();
    m_activeCursorTexture = m_surfaceCursorTexture.get();
    return true;
}

void TrailEffect::refreshCursorShape(RenderView *view, double targetScale)
{
    const Cursor *cursor = Cursors::self()->currentCursor();
    const CursorSource *source = cursor ? cursor->source() : nullptr;
    m_cursorImageState = CursorImageState{
        .source = source,
        .size = cursor ? cursor->rect().size() : QSizeF(),
        .hotspot = cursor ? cursor->hotspot() : QPointF(),
        .surfaceScale = targetScale,
    };

    if (!cursor || !source || source->isBlank()) {
        m_activeCursorTexture = nullptr;
        m_cursorShape = Trail::CursorShape();
        return;
    }

    // Fast path 1: theme/shape cursors already have a CPU QImage in memory;
    // reuse uploaded GLTextures via LRU cache with zero GPU readback.
    if (const auto *shapeSource = qobject_cast<const ShapeCursorSource *>(source)) {
        const QImage image = shapeSource->image();
        if (image.isNull()) {
            m_activeCursorTexture = nullptr;
            m_cursorShape = Trail::CursorShape();
            return;
        }
        const qreal dpr = image.devicePixelRatio() > 0.0 ? image.devicePixelRatio() : 1.0;
        m_cursorShape.size = QSizeF(image.size()) / dpr;
        m_cursorShape.hotspot = shapeSource->hotspot();
        m_activeCursorTexture = getOrUploadCachedTexture(image);
        return;
    }

    // Fast path 2: Wayland client surface cursors render straight into a
    // reusable GPU FBO texture, avoiding effects->cursorImage()'s glReadPixels
    // pipeline stall and CPU->GPU re-upload.
    if (qobject_cast<const SurfaceCursorSource *>(source)) {
        if (renderCursorItemToTexture(cursor, view, targetScale)) {
            return;
        }
    }

    // Fallback: grab via KWin's platform API and cache by QImage cacheKey.
    const PlatformCursorImage cursorImage = effects->cursorImage();
    const QImage image = cursorImage.image();
    if (image.isNull()) {
        m_activeCursorTexture = nullptr;
        m_cursorShape = Trail::CursorShape();
        return;
    }

    const qreal dpr = image.devicePixelRatio() > 0.0 ? image.devicePixelRatio() : 1.0;
    m_cursorShape.size = QSizeF(image.size()) / dpr;
    m_cursorShape.hotspot = cursorImage.hotSpot();
    m_activeCursorTexture = getOrUploadCachedTexture(image);
}

bool TrailEffect::canDraw() const
{
    return m_enabled && m_activeCursorTexture && m_cursorShape.isValid();
}

bool TrailEffect::isOutputRenderPass(LogicalOutput *screen) const
{
    if (!m_currentView || !screen) {
        return false;
    }
    BackendOutput *const viewOutput = m_currentView->backendOutput();
    return viewOutput && viewOutput == screen->backendOutput();
}

bool TrailEffect::isActive() const
{
    if (!m_enabled) {
        return false;
    }
    for (const OutputEntry &entry : m_frames) {
        const OutputFrameState &frame = *entry.state;
        if (!frame.ring.empty() || !frame.previousDamage.isEmpty() || !frame.damage.isEmpty()) {
            return true;
        }
    }
    return false;
}

bool TrailEffect::blocksDirectScanout() const
{
    return m_enabled && m_blockDirectScanout && m_scanoutBlockOutput;
}

int TrailEffect::requestedEffectChainPosition() const
{
    // Effects are called in ascending position and each one paints after
    // delegating with effects->paintScreen(), so position 0 draws last (on top
    // of windows), matching KWin's own pointer overlay effects.
    return 0;
}

void TrailEffect::reportFrameStats(std::size_t cursorCount)
{
    ++m_statsFrames;
    m_statsCursors += cursorCount;

    const Trail::TimeUs now = monotonicNowUs();
    if (m_statsSince == 0) {
        m_statsSince = now;
        return;
    }
    if (now - m_statsSince < kStatsIntervalUs) {
        return;
    }

    std::uint64_t dropped = 0;
    for (const OutputEntry &entry : m_frames) {
        dropped += entry.state->ring.droppedSamples();
    }

    const double seconds = double(now - m_statsSince) / 1'000'000.0;
    qCDebug(TRAIL) << "trail stats:"
                   << double(m_statsCursors) / seconds << "cursors/s,"
                   << double(m_statsFrames) / seconds << "frames/s,"
                   << double(m_statsCursors) / std::max<std::uint64_t>(1, m_statsFrames) << "cursors/frame,"
                   << dropped << "dropped in total";

    m_statsSince = now;
    m_statsFrames = 0;
    m_statsCursors = 0;
}

} // namespace KWin
