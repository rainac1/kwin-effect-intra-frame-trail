#pragma once

#include "trailsample.h"

#include <kwin/effect/effect.h>
#include <kwin/input_event_spy.h>
#include <kwin/opengl/glvertexbuffer.h>

#include <QImage>
#include <QPointF>
#include <QSize>
#include <QSizeF>

#include <memory>
#include <vector>

namespace KWin
{

class Cursor;
class CursorSource;
class GLFramebuffer;
class GLShader;
class GLTexture;

/**
 * Renders the current pointer image at every position that the pointer was
 * sampled at during the previous frame.
 *
 * Samples are taken from the input event stream (KWin::InputEventSpy), which is
 * the only way to observe them: KWin exposes the current pointer position and
 * individual input events, but keeps no history itself. The samples are stored
 * in a bounded ring buffer and consumed once per frame, so a sample is drawn in
 * the very next frame after it was produced and never delayed further.
 *
 * The effect runs in the compositor process, on the compositor's main thread:
 * both the sampling callback and the frame painting are driven by KWin's event
 * loop, which is what keeps the latency at one frame without any cross-thread
 * hand-off.
 *
 * Every output owns its own sample buffer and frame state. The pointer is a
 * single global device, but each output is painted in its own pass with its own
 * viewport, scale and damage region; sharing one buffer would let whichever
 * output happens to be painted first consume the samples of all the others.
 */
class TrailEffect : public Effect, public InputEventSpy
{
    Q_OBJECT

public:
    TrailEffect();
    ~TrailEffect() override;

    void reconfigure(ReconfigureFlags flags) override;
    void prePaintScreen(ScreenPrePaintData &data) override;
    bool paintScreen(const RenderTarget &renderTarget,
                     const RenderViewport &viewport,
                     int mask,
                     const Region &deviceRegion,
                     LogicalOutput *screen) override;
    void postPaintScreen() override;
    bool isActive() const override;
    bool blocksDirectScanout() const override;
    int requestedEffectChainPosition() const override;

    // KWin::InputEventSpy
    void pointerMotion(PointerMotionEvent *event) override;

private:
    /**
     * Per output painting state; KWin paints each output in its own pass, with
     * its own sample buffer, viewport and damage accounting.
     */
    struct OutputFrameState
    {
        OutputFrameState();

        /**
         * Samples produced since the last frame that was actually drawn by this
         * output. Per output, because one composited frame of one output
         * consumes every sample it sees.
         */
        Trail::SampleRing ring;
        /** Samples selected for the frame currently being painted. */
        std::vector<Trail::Sample> samples;
        /**
         * Cursor geometry the samples of this frame were selected and damaged
         * with. Stored per frame so that a cursor change processed between
         * prePaintScreen() and paintScreen(), or for another output, cannot
         * make the drawn geometry differ from the damaged geometry.
         */
        Trail::CursorShape cursorShape;
        /** Area covered by the cursors drawn in this frame (logical coords). */
        Region damage;
        /** Area covered by the cursors drawn in the previous frame. */
        Region previousDamage;
        /** Start of the sampling window used by the last collected frame. */
        Trail::TimeUs lastCollect = 0;
        /** Real cursor position snapshotted in prePaintScreen() for head masking. */
        QPointF headPos;
        bool hasHead = false;
        /**
         * True once pointerMotion() has already scheduled a repaint for this
         * output during the current frame interval; cleared on prePaintScreen().
         */
        bool repaintPending = false;
        /**
         * True while this output's frames are presented in adaptive sync mode,
         * in which case the trail is not drawn. Read from the frame that is
         * being prepared; see prePaintScreen().
         */
        bool vrrActive = false;
    };

    struct OutputEntry
    {
        LogicalOutput *screen = nullptr;
        std::unique_ptr<OutputFrameState> state;
    };

    /**
     * What the cached texture and m_cursorShape were built from.
     *
     * These are the cheap properties of the cursor; comparing them every frame
     * is what lets the effect notice a changed cursor without fetching its
     * image, which is expensive when a client provides the cursor contents.
     */
    struct CursorImageState
    {
        const CursorSource *source = nullptr;
        QSizeF size;
        QPointF hotspot;
        double surfaceScale = 0.0;
    };

    struct CachedTexture
    {
        qint64 key = 0;
        QSize size;
        std::unique_ptr<GLTexture> texture;
    };

    OutputFrameState *findFrame(LogicalOutput *screen);
    const OutputFrameState *findFrame(LogicalOutput *screen) const;
    OutputFrameState &findOrCreateFrame(LogicalOutput *screen);

    /** Maximum scale across all known outputs (for crisp GPU cursor rendering). */
    double maxOutputScale() const;

    /** True when the pointer's own cursor changed shape, geometry, or scale. */
    bool cursorGeometryChanged(double targetScale) const;

    /** Refresh the cached cursor shape and GPU texture without CPU readback. */
    void refreshCursorShape(RenderView *view, double targetScale);

    /** Render a Wayland SurfaceCursorSource directly into m_surfaceCursorFbo. */
    bool renderCursorItemToTexture(const Cursor *cursor, RenderView *view, double scale);

    /** Look up or upload a QImage-backed cursor texture in the small LRU cache. */
    GLTexture *getOrUploadCachedTexture(const QImage &image);

    /** Lazily compile the batched single-draw-call trail shader. */
    bool ensureTrailShader();

    /** Fallback renderer when the custom batched shader is unavailable. */
    void paintFallback(const RenderTarget &renderTarget,
                       const QMatrix4x4 &projection,
                       const OutputFrameState &frame,
                       double scale,
                       const QSizeF &deviceSize,
                       const QPointF &deviceHotspot);

    /** True when this frame can actually collect and paint cursor samples. */
    bool canDraw() const;

    /**
     * True when the render pass currently being prepared draws to an actual
     * output rather than into an offscreen capture target.
     *
     * Every capture path -- screencast, screenshot, color picker, the screen
     * transform -- renders the same scene through the same effect chain, but
     * into its own buffer and with its own RenderView. Those passes must not
     * touch the per-output frame state; see prePaintScreen().
     */
    bool isOutputRenderPass(LogicalOutput *screen) const;

    /** Emit a periodic summary of the rendering work, for diagnostics. */
    void reportFrameStats(std::size_t cursorCount);

    /**
     * Diagnostics only: snapshot the framebuffer area that is about to be
     * painted, so that selfCheckAfter() can prove the draw reached it.
     */
    void selfCheckBefore(const Region &logicalDamage, double scale, const QSize &targetSize);

    /** Diagnostics only: report how many pixels the draw changed. */
    void selfCheckAfter();

    /** State of every output, stored contiguously for fast scan on hot paths. */
    std::vector<OutputEntry> m_frames;

    /**
     * View of the render pass being prepared. paintScreen() is not told which
     * view it paints for, so prePaintScreen() records it here; the two are
     * always called as a pair for one pass. Compared against the output's own
     * view by isOutputRenderPass().
     */
    RenderView *m_currentView = nullptr;
    LogicalOutput *m_currentScreen = nullptr;

    /** LRU cache for QImage-backed cursors (theme shapes and animated sprites). */
    std::vector<CachedTexture> m_textureCache;
    /** Reusable GPU target for SurfaceCursorSource (declared before its FBO). */
    std::unique_ptr<GLTexture> m_surfaceCursorTexture;
    std::unique_ptr<GLFramebuffer> m_surfaceCursorFbo;
    /** Non-owning pointer to whichever texture is currently active. */
    GLTexture *m_activeCursorTexture = nullptr;

    Trail::CursorShape m_cursorShape;
    CursorImageState m_cursorImageState;
    /**
     * Set when the compositor reports new cursor contents. Together with
     * cursorGeometryChanged() this keeps the cached texture up to date without
     * refreshing on every frame.
     */
    bool m_cursorDirty = true;

    /** Batched trail shader (per-sample fade + real-cursor alpha masking). */
    std::unique_ptr<GLShader> m_trailShader;
    bool m_trailShaderAttempted = false;
    int m_locTextureMatrix = -1;
    int m_locCursorOrigin = -1;
    int m_locCursorInvSize = -1;
    int m_locMaskCursor = -1;

    /** Reusable vertex scratch buffers (zero steady-state heap allocation). */
    std::vector<GLVertex3D> m_vertices3D;
    std::vector<GLVertex2D> m_vertices2D;

    /**
     * Output whose frame is currently being prepared and has to composite the
     * trail, or nullptr when the frame may be a direct scanout.
     */
    LogicalOutput *m_scanoutBlockOutput = nullptr;

    /** Upper bound on cursors drawn per frame. */
    int m_maxSamples = 256;
    /**
     * Keep one out of every this many pointer samples. 1 keeps all of them;
     * larger values drop the samples in between before they are buffered.
     */
    int m_sampleStride = 1;
    /** Samples seen since the last one that was kept; see pointerMotion(). */
    int m_strideCounter = 0;
    /**
     * Whether to block direct scanout (and thus DRM cursor/overlay planes)
     * while a trail is active. Off by default to preserve the hardware cursor
     * plane and fullscreen direct scanout.
     */
    bool m_blockDirectScanout = false;
    /** Dyadic fractal opacity parameters. */
    Trail::FractalOpacityConfig m_fade;
    bool m_enabled = true;

    // Diagnostics only.
    Trail::TimeUs m_statsSince = 0;
    std::uint64_t m_statsFrames = 0;
    std::uint64_t m_statsCursors = 0;
    bool m_selfCheck = false;
    QRect m_selfCheckRect;
    std::vector<unsigned char> m_selfCheckBefore;
    std::vector<unsigned char> m_selfCheckAfter;
    Trail::TimeUs m_selfCheckReported = 0;
};

} // namespace KWin
