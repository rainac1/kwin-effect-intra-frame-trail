#pragma once

#include "trailsample.h"

#include <kwin/effect/effect.h>
#include <kwin/input_event_spy.h>

#include <QImage>
#include <QSizeF>

#include <map>
#include <memory>
#include <vector>

namespace KWin
{

class CursorSource;
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
    void paintScreen(const RenderTarget &renderTarget,
                     const RenderViewport &viewport,
                     int mask,
                     const Region &deviceRegion,
                     LogicalOutput *screen) override;
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
        /**
         * True while this output's frames are presented in adaptive sync mode,
         * in which case the trail is not drawn. Read from the frame that is
         * being prepared; see prePaintScreen().
         */
        bool vrrActive = false;
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
    };

    /** True when the pointer's own cursor changed shape or geometry. */
    bool cursorGeometryChanged() const;

    /** Fetch the cursor image and rebuild the cached shape and texture. */
    void refreshCursorShape();

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

    /** State of every output, created on demand and on screenAdded. */
    std::map<LogicalOutput *, OutputFrameState> m_frames;

    /**
     * View of the render pass being prepared. paintScreen() is not told which
     * view it paints for, so prePaintScreen() records it here; the two are
     * always called as a pair for one pass. Compared against the output's own
     * view by isOutputRenderPass().
     */
    RenderView *m_currentView = nullptr;

    std::unique_ptr<GLTexture> m_cursorTexture;
    qint64 m_cursorImageKey = 0;
    Trail::CursorShape m_cursorShape;
    CursorImageState m_cursorImageState;
    /**
     * Set when the compositor reports new cursor contents. Together with
     * cursorGeometryChanged() this keeps the cached image up to date without
     * fetching it on every frame.
     */
    bool m_cursorDirty = true;

    /**
     * Output whose frame is currently being prepared and has to composite the
     * trail, or nullptr when the frame may be a direct scanout.
     *
     * KWin asks blocksDirectScanout() for one output immediately after that
     * output's prePaintScreen(), so this records the decision of the most
     * recent output pass; a capture pass does not touch it. See
     * prePaintScreen().
     */
    LogicalOutput *m_scanoutBlockOutput = nullptr;

    /** Upper bound on cursors drawn per frame. */
    int m_maxSamples = 256;
    /**
     * Keep one out of every this many pointer samples. 1 keeps all of them,
     * which is the effect's original behaviour; larger values drop the samples
     * in between before they are buffered, so the drawn cursors end up further
     * apart inside the same one-frame window.
     */
    int m_sampleStride = 1;
    /** Samples seen since the last one that was kept; see pointerMotion(). */
    int m_strideCounter = 0;
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
