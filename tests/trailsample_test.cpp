/*
    SPDX-FileCopyrightText: 2026 Trail KWin Effect contributors
    SPDX-License-Identifier: GPL-2.0-or-later

    Unit tests for the compositor-independent parts of the effect.
*/

#include "trailsample.h"

#include <QRectF>

#include <cstdio>

namespace
{

int g_failures = 0;

void check(bool condition, const char *expression, int line)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
        ++g_failures;
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

Trail::Sample makeSample(float x, float y, Trail::TimeUs time)
{
    return Trail::Sample{.x = x, .y = y, .time = time};
}

void testCapacity()
{
    CHECK(Trail::SampleRing(1).capacity() == 2);
    CHECK(Trail::SampleRing(3).capacity() == 4);
    CHECK(Trail::SampleRing(1024).capacity() == 1024);
}

void testWindowFiltering()
{
    Trail::SampleRing ring(8);
    ring.push(makeSample(1.0f, 1.0f, 100));
    ring.push(makeSample(2.0f, 2.0f, 200));
    ring.push(makeSample(3.0f, 3.0f, 300));

    Trail::Sample out[8];

    // Only samples inside the window are returned, in arrival order.
    CHECK(ring.collect(150, out, 8) == 2);
    CHECK(out[0].time == 200);
    CHECK(out[1].time == 300);
    CHECK(out[0].x == 2.0f);
    CHECK(out[1].x == 3.0f);

    // The window boundary is inclusive: a sample exactly at `since` is kept.
    ring.push(makeSample(4.0f, 4.0f, 400));
    CHECK(ring.collect(400, out, 8) == 1);
    CHECK(out[0].time == 400);
}

void testConsumedSamplesAreNotReturnedTwice()
{
    Trail::SampleRing ring(8);
    ring.push(makeSample(1.0f, 1.0f, 100));
    ring.push(makeSample(2.0f, 2.0f, 200));

    Trail::Sample out[8];
    CHECK(ring.collect(0, out, 8) == 2);
    CHECK(ring.collect(0, out, 8) == 0);
}

void testMaxOutKeepsNewest()
{
    Trail::SampleRing ring(16);
    for (int i = 1; i <= 10; ++i) {
        ring.push(makeSample(float(i), float(i), Trail::TimeUs(i) * 100));
    }

    Trail::Sample out[3];
    CHECK(ring.collect(0, out, 3) == 3);
    // The newest samples matter most: they are the ones right behind the pointer.
    CHECK(out[0].time == 800);
    CHECK(out[1].time == 900);
    CHECK(out[2].time == 1000);
}

void testFullBufferDropsIncoming()
{
    Trail::SampleRing ring(4);
    for (int i = 1; i <= 6; ++i) {
        ring.push(makeSample(float(i), float(i), Trail::TimeUs(i) * 100));
    }

    CHECK(ring.droppedSamples() == 2);

    Trail::Sample out[8];
    // Never overwrites slots the painter may still be reading.
    CHECK(ring.collect(0, out, 8) == 4);
    CHECK(out[0].time == 100);
    CHECK(out[3].time == 400);

    // A full buffer recovers as soon as the stale samples are collected.
    CHECK(ring.droppedSamples() == 2);
    ring.push(makeSample(7.0f, 7.0f, 700));
    CHECK(ring.collect(0, out, 8) == 1);
    CHECK(out[0].time == 700);
}

void testReset()
{
    Trail::SampleRing ring(4);
    ring.push(makeSample(1.0f, 1.0f, 100));
    ring.reset();

    Trail::Sample out[4];
    CHECK(ring.collect(0, out, 4) == 0);
}

void testCursorRectUsesHotspot()
{
    const Trail::CursorShape shape{.size = QSizeF(24.0, 24.0), .hotspot = QPointF(2.0, 3.0)};
    CHECK(shape.isValid());

    const QRectF rect = Trail::cursorRect(makeSample(100.0f, 200.0f, 1), shape);
    CHECK(rect.x() == 98.0);
    CHECK(rect.y() == 197.0);
    CHECK(rect.width() == 24.0);
    CHECK(rect.height() == 24.0);

    CHECK(!Trail::CursorShape().isValid());
}

} // namespace

int main()
{
    testCapacity();
    testWindowFiltering();
    testConsumedSamplesAreNotReturnedTwice();
    testMaxOutKeepsNewest();
    testFullBufferDropsIncoming();
    testReset();
    testCursorRectUsesHotspot();

    if (g_failures > 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
