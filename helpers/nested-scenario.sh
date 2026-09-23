#!/usr/bin/env bash
#
# nested-scenario.sh -- the test scenario executed *inside* the nested KWin
# session. KWin starts this as its client, so WAYLAND_DISPLAY points at the
# nested compositor and the nested D-Bus session is inherited.
#
# It injects a pointer path and captures a screenshot while the pointer is still
# moving, because with TrailFrames=1 the trail only exists while samples keep
# arriving.
#
set -u

INJECTOR=${INJECTOR:?INJECTOR must point at the fake-input-injector binary}
SHOT=${SHOT:-/tmp/trail-e2e.png}
INJECT_MS=${INJECT_MS:-6000}
SPEED=${SPEED:-20}

echo "[scenario] waiting for the compositor to settle"
sleep 2

echo "[scenario] injecting pointer motion for ${INJECT_MS} ms"
"$INJECTOR" --duration "$INJECT_MS" --interval-ms 4 --speed "$SPEED" \
    --x0 80 --y0 80 --x1 1180 --y1 620 &
injector_pid=$!

# Let the trail build up, then capture while motion is still happening.
sleep 2
echo "[scenario] capturing screenshot to $SHOT"
if command -v spectacle >/dev/null 2>&1; then
    spectacle -b -n -f -o "$SHOT" || echo "[scenario] spectacle failed"
else
    echo "[scenario] spectacle is not available"
fi

wait "$injector_pid" 2>/dev/null || true
echo "[scenario] done"
