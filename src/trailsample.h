/*
    SPDX-FileCopyrightText: 2026 Trail KWin Effect contributors
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <QPointF>
#include <QRectF>
#include <QSizeF>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

/**
 * Pointer sampling and geometry, deliberately free of any KWin or OpenGL
 * dependency so that it can be unit tested without a compositor.
 */
namespace Trail
{

/**
 * Monotonic timestamp in microseconds. Samples carry the timestamp reported by
 * the input backend when it is usable; see TrailEffect::pointerMotion().
 */
using TimeUs = std::uint64_t;

/** A single pointer position sampled from the input stream. */
struct Sample
{
    float x = 0.0f;
    float y = 0.0f;
    TimeUs time = 0;
};

/**
 * Fixed capacity ring buffer holding the most recent pointer samples.
 *
 * KWin does not provide any pointer history (it only broadcasts individual
 * input events), so the effect has to keep its own. This buffer is sized once,
 * never allocates while running, and pushes in O(1), which keeps the sampling
 * path free of dynamically allocating containers at rates of several kHz.
 *
 * Indices are published with acquire/release atomics. Producer and consumer
 * currently both live on the compositor's main thread, so this is not required
 * for correctness today; it costs two atomic operations per event and keeps the
 * buffer correct if input ever moves off the main thread again, and avoids a
 * mutex on the input path altogether.
 *
 * When the buffer is full, incoming samples are dropped instead of overwriting
 * slots the painter may still be reading. Samples that are older than the
 * requested time window are discarded on the next collect(), so a full buffer
 * recovers immediately; droppedSamples() makes the situation observable.
 * Dropping is also the right policy for the frame budget: the number of cursors
 * that can be drawn per frame is bounded anyway.
 */
class SampleRing
{
public:
    explicit SampleRing(std::size_t capacity);

    /** Producer side: append a sample. Never blocks, never allocates. */
    void push(const Sample &sample) noexcept;

    /**
     * Consumer side: copy every buffered sample with time >= @p since into
     * @p out, oldest first, and mark the buffer as consumed.
     *
     * At most @p maxOut samples are returned. If more samples qualify, the
     * newest ones are kept, because those are the ones closest to the real
     * pointer and therefore the most visible.
     */
    std::size_t collect(TimeUs since, Sample *out, std::size_t maxOut) noexcept;

    /** Discard all buffered samples. */
    void reset() noexcept;

    std::size_t capacity() const noexcept
    {
        return m_data.size();
    }

    /** Number of samples dropped because the buffer was full. */
    std::uint64_t droppedSamples() const noexcept
    {
        return m_dropped;
    }

private:
    std::vector<Sample> m_data;
    std::size_t m_mask = 0;
    // Sequence number of the next slot to write / of the oldest unconsumed slot.
    std::atomic<std::uint64_t> m_write{0};
    std::atomic<std::uint64_t> m_read{0};
    // Producer only, exposed for diagnostics.
    std::uint64_t m_dropped = 0;
};

/** Size and hotspot of the cursor image, in logical coordinates. */
struct CursorShape
{
    QSizeF size;
    QPointF hotspot;

    bool isValid() const
    {
        return size.width() > 0.0 && size.height() > 0.0;
    }
};

/**
 * Rectangle covered by the cursor image drawn for @p sample.
 *
 * The hotspot is the point of the image that sits exactly on the pointer
 * position, so the image is offset by -hotspot. This mirrors what KWin's own
 * CursorItem does when it renders the real pointer.
 */
QRectF cursorRect(const Sample &sample, const CursorShape &shape);

} // namespace Trail
