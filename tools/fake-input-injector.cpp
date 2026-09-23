/*
    SPDX-FileCopyrightText: 2026 Trail KWin Effect contributors
    SPDX-License-Identifier: GPL-2.0-or-later

    Test helper: injects a pointer path into a KWin session through the
    org_kde_kwin_fake_input Wayland protocol.

    This exists so the effect can be exercised deterministically in a nested
    compositor without touching the host pointer. It is a test tool and is not
    part of the effect.
*/

#include "fake-input-client.h"

#include <wayland-client.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace
{

struct Options
{
    double x0 = 80.0;
    double y0 = 80.0;
    double x1 = 1200.0;
    double y1 = 640.0;
    int durationMs = 4000;
    int intervalMs = 4;
    double speed = 28.0; // pixels per step
};

void sleepMs(int ms)
{
    timespec ts{};
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = long(ms % 1000) * 1'000'000L;
    nanosleep(&ts, nullptr);
}

void printUsage(const char *program)
{
    std::fprintf(stderr,
                 "usage: %s [--x0 N] [--y0 N] [--x1 N] [--y1 N]\n"
                 "          [--duration MS] [--interval-ms MS] [--speed PX]\n",
                 program);
}

org_kde_kwin_fake_input *g_fakeInput = nullptr;

void handleGlobal(void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
    (void)data;
    if (std::strcmp(interface, org_kde_kwin_fake_input_interface.name) != 0) {
        return;
    }
    // pointer_motion_absolute requires version 3 or newer.
    g_fakeInput = static_cast<org_kde_kwin_fake_input *>(
        wl_registry_bind(registry, name, &org_kde_kwin_fake_input_interface, std::min<uint32_t>(version, 3)));
}

void handleGlobalRemove(void *data, wl_registry *registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

const wl_registry_listener kRegistryListener = {
    .global = handleGlobal,
    .global_remove = handleGlobalRemove,
};

} // namespace

int main(int argc, char **argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        const auto next = [&]() -> const char * {
            return i + 1 < argc ? argv[++i] : nullptr;
        };
        if (!std::strcmp(arg, "--x0")) {
            options.x0 = std::atof(next());
        } else if (!std::strcmp(arg, "--y0")) {
            options.y0 = std::atof(next());
        } else if (!std::strcmp(arg, "--x1")) {
            options.x1 = std::atof(next());
        } else if (!std::strcmp(arg, "--y1")) {
            options.y1 = std::atof(next());
        } else if (!std::strcmp(arg, "--duration")) {
            options.durationMs = std::atoi(next());
        } else if (!std::strcmp(arg, "--interval-ms")) {
            options.intervalMs = std::max(1, std::atoi(next()));
        } else if (!std::strcmp(arg, "--speed")) {
            options.speed = std::atof(next());
        } else {
            printUsage(argv[0]);
            return 2;
        }
    }

    wl_display *display = wl_display_connect(nullptr);
    if (!display) {
        std::fprintf(stderr, "fake-input-injector: cannot connect to the Wayland display\n");
        return 1;
    }

    wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &kRegistryListener, nullptr);
    wl_display_roundtrip(display);

    if (!g_fakeInput) {
        std::fprintf(stderr, "fake-input-injector: the compositor does not provide org_kde_kwin_fake_input\n");
        return 1;
    }

    // The compositor ignores every request until the client has authenticated.
    org_kde_kwin_fake_input_authenticate(g_fakeInput, "trail-e2e", "pointer trail end-to-end test");
    wl_display_flush(display);

    // Bounce between the two corners so that the pointer keeps moving while the
    // test inspects the screen.
    const double spanX = options.x1 - options.x0;
    const double spanY = options.y1 - options.y0;
    const double length = std::max(1.0, std::hypot(spanX, spanY));
    double dx = options.speed * spanX / length;
    double dy = options.speed * spanY / length;

    double x = options.x0;
    double y = options.y0;
    int steps = 0;

    timespec start{};
    clock_gettime(CLOCK_MONOTONIC, &start);
    const auto elapsedMs = [&start]() {
        timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        return int((now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1'000'000);
    };

    while (elapsedMs() < options.durationMs) {
        x += dx;
        y += dy;
        if (x < options.x0 || x > options.x1) {
            dx = -dx;
            x = std::clamp(x, options.x0, options.x1);
        }
        if (y < options.y0 || y > options.y1) {
            dy = -dy;
            y = std::clamp(y, options.y0, options.y1);
        }

        org_kde_kwin_fake_input_pointer_motion_absolute(g_fakeInput,
                                                        wl_fixed_from_double(x),
                                                        wl_fixed_from_double(y));
        wl_display_flush(display);

        if (++steps % 64 == 0) {
            // Keep the connection drained.
            wl_display_dispatch_pending(display);
        }
        sleepMs(options.intervalMs);
    }

    std::fprintf(stderr, "fake-input-injector: injected %d motion events\n", steps);

    org_kde_kwin_fake_input_destroy(g_fakeInput);
    wl_display_flush(display);
    wl_display_disconnect(display);
    return 0;
}
