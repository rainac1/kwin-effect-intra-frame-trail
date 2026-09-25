/*
    SPDX-FileCopyrightText: 2026 Trail KWin Effect contributors
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "trailsample.h"

#include <kwin/effect/effect.h>
#include <kwin/input_event_spy.h>

#include <QHash>
#include <QImage>
#include <QSizeF>

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
    /** Per output painting state; KWin paints each output in its own pass. */
    struct OutputFrameState
    {
        /** Samples selected for the frame currently being painted. */
        std::vector<Trail::Sample> samples;
        /** Area covered by the cursors drawn in this frame (logical coords). */
        Region damage;
        /** Area covered by the cursors drawn in the previous frame. */
        Region previousDamage;
        /** Timestamp of the previous paint pass for this output. */
        Trail::TimeUs lastPaint = 0;
        /** Start of the sampling window used by the last collected frame. */
        Trail::TimeUs lastCollect = 0;
        /** Smoothed interval between two paint passes for this output. */
        Trail::TimeUs interval = 0;
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

    /**
     * Add the trail's damage for a capture pass and remember it for that view,
     * so that the next pass into the same target repairs what this one drew.
     *
     * The samples themselves are not touched here: they were collected by the
     * output pass and are only read again.
     */
    void prepareCaptureDamage(ScreenPrePaintData &data);

    /** Emit a periodic summary of the rendering work, for diagnostics. */
    void reportFrameStats(std::size_t cursorCount);

    /**
     * Diagnostics only: snapshot the framebuffer area that is about to be
     * painted, so that selfCheckAfter() can prove the draw reached it.
     */
    void selfCheckBefore(const Region &logicalDamage, double scale, const QSize &targetSize);

    /** Diagnostics only: report how many pixels the draw changed. */
    void selfCheckAfter();

    Trail::SampleRing m_ring;
    QHash<LogicalOutput *, OutputFrameState> m_frames;

    /**
     * View of the render pass being prepared. paintScreen() is not told which
     * view it paints for, so prePaintScreen() records it here; the two are
     * always called as a pair for one pass. Compared against the output's own
     * view by isOutputRenderPass().
     */
    RenderView *m_currentView = nullptr;

    /**
     * Damage the trail left in a capture target last time, per view. A capture
     * target is not the output, so its own previous trail has to be tracked and
     * repaired separately. Entries are dropped when their view is destroyed.
     */
    QHash<RenderView *, Region> m_captureDamage;

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

    /** Timestamp of the most recent sample, used for direct scanout decisions. */
    Trail::TimeUs m_lastSampleUs = 0;
    /** Most recent frame interval, used for direct scanout decisions. */
    Trail::TimeUs m_intervalUs = 0;

    /** Number of frame intervals a sample stays visible. 1 = exactly one frame. */
    int m_trailFrames = 1;
    /** Upper bound on cursors drawn per frame. */
    int m_maxSamples = 256;
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
