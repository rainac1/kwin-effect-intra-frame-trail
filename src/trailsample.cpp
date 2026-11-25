#include "trailsample.h"

#include <algorithm>
#include <bit>
#include <cmath>

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

int fractalSampleLevel(std::size_t k, std::size_t totalCount) noexcept
{
    if (k == 0 || totalCount <= 1) {
        return 0;
    }

    // m = floor(log2(n - 1)) + 1, so 2^(m - 1) <= n - 1 < 2^m.
    const int m = int(std::bit_width(totalCount - 1));
    // v = ctz(k), so 2^v = lowbit(k); level L = m - v ranges from 1 (coarsest) to m (finest).
    const int v = std::countr_zero(k);
    return std::max(1, m - v);
}

double levelPerceivedBrightness(int level, const FractalOpacityConfig &config) noexcept
{
    if (!config.enabled || level <= 0) {
        return 1.0;
    }

    switch (config.mode) {
    case FadeMode::Geometric:
        if (config.base <= 1.0) {
            return 1.0;
        }
        return std::clamp(std::pow(config.base, -double(level)), 0.0, 1.0);
    case FadeMode::BeerLambert: {
        if (config.opticalDepth <= 0.0) {
            return 0.0;
        }
        const double tau = config.opticalDepth * std::exp2(-double(level));
        return std::clamp(1.0 - std::exp(-tau), 0.0, 1.0);
    }
    }
    return 1.0;
}

double applyGammaCompensation(double brightness, const FractalOpacityConfig &config) noexcept
{
    const double clamped = std::clamp(brightness, 0.0, 1.0);
    if (!config.gammaCompensation || config.gamma <= 0.0 || clamped <= 0.0 || clamped >= 1.0) {
        return clamped;
    }
    return std::clamp(std::pow(clamped, 1.0 / config.gamma), 0.0, 1.0);
}

LevelOpacityTable buildLevelOpacities(const FractalOpacityConfig &config) noexcept
{
    LevelOpacityTable table{};
    table[0] = 1.0f;
    for (int level = 1; level < int(kMaxFadeLevels); ++level) {
        const double brightness = levelPerceivedBrightness(level, config);
        table[std::size_t(level)] = float(applyGammaCompensation(brightness, config));
    }
    return table;
}

float fractalSampleOpacity(std::size_t k, std::size_t totalCount, const LevelOpacityTable &levelOpacities) noexcept
{
    const int level = fractalSampleLevel(k, totalCount);
    return levelOpacities[std::size_t(std::clamp(level, 0, int(kMaxFadeLevels - 1)))];
}

float fractalSampleOpacity(std::size_t k, std::size_t totalCount, const FractalOpacityConfig &config) noexcept
{
    const int level = fractalSampleLevel(k, totalCount);
    const double brightness = levelPerceivedBrightness(level, config);
    return float(applyGammaCompensation(brightness, config));
}

QRectF cursorRect(const Sample &sample, const CursorShape &shape)
{
    const QPointF topLeft(sample.x - shape.hotspot.x(), sample.y - shape.hotspot.y());
    return QRectF(topLeft, shape.size);
}

} // namespace Trail
