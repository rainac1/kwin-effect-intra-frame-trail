#!/usr/bin/env bash
#
# nested-e2e.sh -- end-to-end test of the effect in an isolated nested KWin.
#
# Starts a nested compositor with the effect enabled, injects a pointer path
# through org_kde_kwin_fake_input, and captures a screenshot while the pointer is
# still moving. Nothing here touches the host session or its pointer.
#
# Environment:
#   SHOT          screenshot path        (default: <repo>/.runtime/trail-e2e.png)
#   TRAIL_FRAMES  effect TrailFrames     (default: 4, so the trail is easy to see)
#   INJECT_MS     duration of motion     (default: 6000)
#   DURATION      nested session timeout (default: 20)
#
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
INJECTOR="$ROOT_DIR/.tools/injector/fake-input-injector"
SHOT=${SHOT:-"$ROOT_DIR/.runtime/trail-e2e.png"}
LOG=${LOG:-"$ROOT_DIR/.runtime/nested-e2e.log"}

if [[ ! -x "$INJECTOR" ]]; then
    bash "$ROOT_DIR/tools/build-fake-input-injector.sh"
fi

export INJECTOR
export SHOT
export INJECT_MS=${INJECT_MS:-6000}
export SPEED=${SPEED:-20}

# org_kde_kwin_fake_input is on KWin's interface blacklist and is normally only
# exposed to allow-listed clients. KWin offers this switch for testing; it only
# affects the throwaway nested session started below.
export KWIN_WAYLAND_NO_PERMISSION_CHECKS=1
export TRAIL_KWIN_SELFCHECK=1

mkdir -p "$(dirname "$SHOT")" "$(dirname "$LOG")"
rm -f "$SHOT"

CLIENT="$ROOT_DIR/helpers/nested-scenario.sh" \
DURATION=${DURATION:-20} \
TRAIL_FRAMES=${TRAIL_FRAMES:-4} \
SOCKET=${SOCKET:-wayland-dev} \
    bash "$ROOT_DIR/helpers/run-nested.sh" > "$LOG" 2>&1 || true

echo "=== nested session log (effect lines) ==="
grep -E "kwin_effect_trail|explicitly states|Couldn't create|fake-input" "$LOG" || echo "(no effect lines)"
echo
if [[ -f "$SHOT" ]]; then
    echo "screenshot written to $SHOT"
else
    echo "no screenshot was produced; see $LOG" >&2
    exit 1
fi
