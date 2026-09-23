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
        /** Smoothed interval between two paint passes for this output. */
        Trail::TimeUs interval = 0;
    };

    /** Refresh the cached cursor texture and its logical geometry. */
    void updateCursorShape();

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

    std::unique_ptr<GLTexture> m_cursorTexture;
    qint64 m_cursorImageKey = 0;
    Trail::CursorShape m_cursorShape;

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
