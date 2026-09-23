/*
    SPDX-FileCopyrightText: 2026 Trail KWin Effect contributors
    SPDX-License-Identifier: GPL-2.0-or-later

    Renders the pointer image at every position sampled during the previous
    frame. See docs/DESIGN.md for the reasoning behind the rendering and damage
    strategy.
*/

#include "traileffect.h"

#include <kwin/core/rendertarget.h>
#include <kwin/core/renderviewport.h>
#include <kwin/effect/effecthandler.h>
#include <kwin/effect/globals.h>
#include <kwin/input.h>
#include <kwin/input_event.h>
#include <kwin/opengl/glplatform.h>
#include <kwin/opengl/glshader.h>
#include <kwin/opengl/glshadermanager.h>
#include <kwin/opengl/gltexture.h>

#include <KConfigGroup>
#include <KSharedConfig>
#include <QLoggingCategory>
#include <QMatrix4x4>

#include <algorithm>
#include <ctime>

Q_LOGGING_CATEGORY(TRAIL, "kwin_effect_trail")

namespace KWin
{

namespace
{

/** Assumed frame interval before the first one has been measured (60 Hz). */
constexpr Trail::TimeUs kDefaultFrameIntervalUs = 16667;

/**
 * Upper bound for a sample timestamp to be considered usable. libinput reports
 * CLOCK_MONOTONIC microseconds, but other backends (e.g. X11 events) may report
 * zero or a completely different clock; samples outside this window fall back
 * to the time at which the effect observed them.
 */
constexpr Trail::TimeUs kMaxPlausibleEventAgeUs = 1'000'000;

constexpr int kMaxTrailFrames = 16;
constexpr int kDefaultMaxSamples = 256;
constexpr std::size_t kRingCapacity = 1024;

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

} // namespace

TrailEffect::TrailEffect()
    : m_ring(kRingCapacity)
{
    // KWin's only way to observe individual pointer samples: it broadcasts the
    // current position but keeps no history, so we collect it ourselves. The
    // spy is uninstalled automatically when this object is destroyed.
    input()->installInputEventSpy(this);
    m_selfCheck = qEnvironmentVariableIntValue("TRAIL_KWIN_SELFCHECK") == 1;
    reconfigure(ReconfigureAll);
}

TrailEffect::~TrailEffect() = default;

void TrailEffect::reconfigure(ReconfigureFlags flags)
{
    Q_UNUSED(flags)

    const KConfigGroup config = KSharedConfig::openConfig()->group(QStringLiteral("Effect-trail"));
    m_enabled = config.readEntry("Enabled", true);
    m_trailFrames = std::clamp(config.readEntry("TrailFrames", 1), 1, kMaxTrailFrames);
    m_maxSamples = std::clamp(config.readEntry("MaxSamples", kDefaultMaxSamples), 1, int(m_ring.capacity()));

    if (m_enabled) {
        effects->addRepaintFull();
    }
}

void TrailEffect::pointerMotion(PointerMotionEvent *event)
{
    if (!m_enabled) {
        return;
    }

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
    m_ring.push(sample);
    m_lastSampleUs = now;

    // KWin repaints when the cursor moves, but the cursor may be rendered on a
    // hardware plane or hidden, so request the region explicitly. Damage is
    // coalesced by the scene, so one call per event is cheap even at 8 kHz.
    if (m_cursorShape.isValid()) {
        effects->addRepaint(Trail::cursorRect(sample, m_cursorShape));
    }
}

void TrailEffect::prePaintScreen(ScreenPrePaintData &data)
{
    const Trail::TimeUs now = monotonicNowUs();
    OutputFrameState &frame = m_frames[data.screen];

    // Samples newer than the paint pass that happened m_trailFrames frames ago.
    // With the default of one frame this is exactly the previous frame interval.
    Trail::TimeUs anchor = frame.lastPaint;
    if (anchor == 0) {
        anchor = now > kDefaultFrameIntervalUs ? now - kDefaultFrameIntervalUs : 0;
    }
    Trail::TimeUs since = anchor;
    if (m_trailFrames > 1) {
        const Trail::TimeUs back = Trail::TimeUs(m_trailFrames - 1) * frame.interval;
        since = anchor > back ? anchor - back : 0;
    }

    // Snapshot once per frame: painting then uses exactly the set whose area was
    // added to the damage region, so nothing is drawn outside the damaged area
    // and nothing needs to be redrawn later.
    frame.samples.resize(std::size_t(m_maxSamples));
    const std::size_t count = m_ring.collect(since, frame.samples.data(), std::size_t(m_maxSamples));
    frame.samples.resize(count);

    // Repaint both the cursors drawn last frame (so the old trail disappears)
    // and the ones drawn now.
    Region damage = frame.previousDamage;
    frame.damage = Region();
    if (m_enabled && m_cursorShape.isValid()) {
        for (const Trail::Sample &sample : frame.samples) {
            frame.damage += toIntRect(Trail::cursorRect(sample, m_cursorShape));
        }
    }
    damage |= frame.damage;
    if (!damage.isEmpty()) {
        data.paint += damage;
    }

    if (frame.lastPaint != 0) {
        const Trail::TimeUs delta = now - frame.lastPaint;
        if (delta > 0 && delta < kMaxPlausibleEventAgeUs) {
            // Smoothed, so display jitter does not make the trail length jump.
            frame.interval = frame.interval == 0 ? delta : (frame.interval * 3 + delta) / 4;
            m_intervalUs = frame.interval;
        }
    }
    frame.lastPaint = now;

    effects->prePaintScreen(data);
}

void TrailEffect::paintScreen(const RenderTarget &renderTarget,
                              const RenderViewport &viewport,
                              int mask,
                              const Region &deviceRegion,
                              LogicalOutput *screen)
{
    // Paint everything below the effect first, then draw on top of it.
    effects->paintScreen(renderTarget, viewport, mask, deviceRegion, screen);

    const auto it = m_frames.find(screen);
    if (it == m_frames.end()) {
        return;
    }
    OutputFrameState &frame = *it;

    // The next frame has to repaint exactly the area painted now.
    frame.previousDamage = frame.damage;

    if (!m_enabled || !effects->isOpenGLCompositing() || frame.samples.empty()) {
        return;
    }

    updateCursorShape();
    if (!m_cursorTexture || !m_cursorShape.isValid()) {
        return;
    }

    // Samples are in logical coordinates; the projection matrix maps device
    // pixels, so scale before drawing.
    const double scale = viewport.scale();
    const QSizeF deviceSize = m_cursorShape.size * scale;
    if (deviceSize.isEmpty()) {
        return;
    }
    const QPointF deviceHotspot = m_cursorShape.hotspot * scale;
    const QMatrix4x4 projection = viewport.projectionMatrix();

    selfCheckBefore(frame.damage, scale, renderTarget.size());

    const GLboolean blendWasEnabled = glIsEnabled(GL_BLEND);
    GLint previousSource = GL_ONE;
    GLint previousDestination = GL_ONE_MINUS_SRC_ALPHA;
    glGetIntegerv(GL_BLEND_SRC_RGB, &previousSource);
    glGetIntegerv(GL_BLEND_DST_RGB, &previousDestination);

    glEnable(GL_BLEND);
    // Cursor images are uploaded as premultiplied ARGB, so premultiplied
    // blending is required; this matches KWin's own texture rendering.
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    {
        ShaderBinder shader(ShaderTrait::MapTexture);
        for (const Trail::Sample &sample : frame.samples) {
            // One draw call per sample against a shared, cached vertex buffer.
            // Samples are ordered oldest first, so the newest cursor ends up on
            // top, right behind the real pointer.
            QMatrix4x4 mvp = projection;
            mvp.translate(float(sample.x * scale - deviceHotspot.x()),
                          float(sample.y * scale - deviceHotspot.y()));
            shader.shader()->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, mvp);
            m_cursorTexture->render(Region::infinite(), deviceSize, false);
        }
    }

    glBlendFunc(previousSource, previousDestination);
    if (!blendWasEnabled) {
        glDisable(GL_BLEND);
    }

    selfCheckAfter();
    reportFrameStats(frame.samples.size());
}

void TrailEffect::selfCheckBefore(const Region &logicalDamage, double scale, const QSize &targetSize)
{
    m_selfCheckRect = QRect();
    if (!m_selfCheck || logicalDamage.isEmpty()) {
        return;
    }

    // The damage region is in logical coordinates, the framebuffer in device
    // pixels. Only a bounded area is read back, because glReadPixels stalls the
    // pipeline; this runs in diagnosis mode only.
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

void TrailEffect::updateCursorShape()
{
    const PlatformCursorImage cursor = effects->cursorImage();
    const QImage image = cursor.image();
    if (image.isNull()) {
        m_cursorTexture.reset();
        m_cursorShape = Trail::CursorShape();
        return;
    }

    const qreal dpr = image.devicePixelRatio() > 0.0 ? image.devicePixelRatio() : 1.0;
    m_cursorShape.size = QSizeF(image.size()) / dpr;
    m_cursorShape.hotspot = cursor.hotSpot();

    // Uploading is only needed when the pointer image actually changed, e.g.
    // when moving from a window to a text field.
    if (!m_cursorTexture || m_cursorImageKey != qint64(image.cacheKey())) {
        m_cursorTexture = GLTexture::upload(image);
        m_cursorImageKey = qint64(image.cacheKey());
        qCDebug(TRAIL) << "cursor image uploaded:" << image.size() << "hotspot" << m_cursorShape.hotspot;
    }
}

bool TrailEffect::blocksDirectScanout() const
{
    // While a trail is on screen the frame cannot be a direct scanout of a
    // single window, but an idle pointer must not keep a fullscreen window from
    // being scanned out, so this only reports true for the duration of the
    // trail.
    if (!m_enabled || m_lastSampleUs == 0) {
        return false;
    }
    const Trail::TimeUs interval = m_intervalUs > 0 ? m_intervalUs : kDefaultFrameIntervalUs;
    const Trail::TimeUs window = Trail::TimeUs(std::max(1, m_trailFrames)) * interval;
    const Trail::TimeUs now = monotonicNowUs();
    return now > m_lastSampleUs && (now - m_lastSampleUs) < window;
}

int TrailEffect::requestedEffectChainPosition() const
{
    // Effects are called in ascending position and each one paints after
    // delegating with effects->paintScreen(), so the first effect in the chain
    // is drawn last. Position 0 therefore puts the trail on top of the windows,
    // which is what KWin's own pointer overlay effects do.
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

    const double seconds = double(now - m_statsSince) / 1'000'000.0;
    qCDebug(TRAIL) << "trail stats:"
                   << double(m_statsCursors) / seconds << "cursors/s,"
                   << double(m_statsFrames) / seconds << "frames/s,"
                   << double(m_statsCursors) / std::max<std::uint64_t>(1, m_statsFrames) << "cursors/frame,"
                   << m_ring.droppedSamples() << "dropped in total";

    m_statsSince = now;
    m_statsFrames = 0;
    m_statsCursors = 0;
}

} // namespace KWin
