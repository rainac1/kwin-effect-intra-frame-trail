/*
    SPDX-FileCopyrightText: 2026 Trail KWin Effect contributors
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "trailsample.h"

#include <algorithm>

namespace Trail
{

namespace
{
std::size_t nextPowerOfTwo(std::size_t value)
{
    std::size_t result = 1;
    while (result < value) {
        result <<= 1;
    }
    return result;
}
}

SampleRing::SampleRing(std::size_t capacity)
    : m_data(nextPowerOfTwo(std::max<std::size_t>(capacity, 2)))
    , m_mask(m_data.size() - 1)
{
}

void SampleRing::push(const Sample &sample) noexcept
{
    const std::uint64_t write = m_write.load(std::memory_order_relaxed);
    if (write - m_read.load(std::memory_order_acquire) >= m_data.size()) {
        // Full: drop the sample instead of overwriting data the painter may
        // still be reading.
        ++m_dropped;
        return;
    }
    m_data[write & m_mask] = sample;
    m_write.store(write + 1, std::memory_order_release);
}

std::size_t SampleRing::collect(TimeUs since, Sample *out, std::size_t maxOut) noexcept
{
    const std::uint64_t write = m_write.load(std::memory_order_acquire);
    std::uint64_t read = m_read.load(std::memory_order_relaxed);
    // Never reach further back than the buffer can still hold.
    if (write - read > m_data.size()) {
        read = write - m_data.size();
    }

    // Samples are ordered by arrival, so the oldest qualifying sample is found
    // with a forward scan and the newest maxOut samples can be kept by moving
    // the start of the copy window forward.
    std::uint64_t first = write;
    for (std::uint64_t seq = read; seq < write; ++seq) {
        if (m_data[seq & m_mask].time >= since) {
            first = seq;
            break;
        }
    }
    if (write - first > maxOut) {
        first = write - maxOut;
    }

    std::size_t count = 0;
    for (std::uint64_t seq = first; seq < write && count < maxOut; ++seq) {
        const Sample &sample = m_data[seq & m_mask];
        if (sample.time >= since) {
            out[count++] = sample;
        }
    }

    m_read.store(write, std::memory_order_release);
    return count;
}

void SampleRing::reset() noexcept
{
    m_read.store(m_write.load(std::memory_order_acquire), std::memory_order_release);
    m_dropped = 0;
}

QRectF cursorRect(const Sample &sample, const CursorShape &shape)
{
    const QPointF topLeft(sample.x - shape.hotspot.x(), sample.y - shape.hotspot.y());
    return QRectF(topLeft, shape.size);
}

TimeUs samplingWindowStart(TimeUs anchor, TimeUs interval, int trailFrames) noexcept
{
    if (trailFrames <= 1) {
        return anchor;
    }
    const TimeUs back = TimeUs(trailFrames - 1) * interval;
    return anchor > back ? anchor - back : 0;
}

} // namespace Trail
