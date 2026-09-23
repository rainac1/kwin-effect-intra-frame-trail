#!/usr/bin/env bash
#
# build-fake-input-injector.sh -- build the test-only pointer injector.
#
# The injector uses KWin's org_kde_kwin_fake_input Wayland protocol, which is
# defined by the plasma-wayland-protocols package (a KWin build dependency that
# is not necessarily installed). The protocol XML is fetched on demand; it is
# only needed to build this helper, never to build or run the effect.
#
# Environment:
#   SYSROOT        local dev sysroot    (default: <repo>/.sysroot)
#   FAKE_INPUT_XML override the protocol XML path
#
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
SYSROOT=${SYSROOT:-"$ROOT_DIR/.sysroot"}
OUT_DIR="$ROOT_DIR/.tools/injector"

command -v wayland-scanner >/dev/null || {
    echo "error: wayland-scanner is required (install wayland-devel)" >&2
    exit 1
}

XML=${FAKE_INPUT_XML:-}
if [[ -z "$XML" ]]; then
    for candidate in \
        /usr/share/plasma-wayland-protocols/fake-input.xml \
        "$ROOT_DIR/.tools/fake-input.xml"
    do
        if [[ -f "$candidate" ]]; then
            XML="$candidate"
            break
        fi
    done
fi
if [[ -z "$XML" ]]; then
    XML="$ROOT_DIR/.tools/fake-input.xml"
    echo "fetching fake-input.xml"
    curl -fS --no-progress-meter --retry 3 -o "$XML" \
        "https://invent.kde.org/libraries/plasma-wayland-protocols/-/raw/master/src/protocols/fake-input.xml"
fi

if [[ ! -d "$SYSROOT/usr/include" ]]; then
    echo "error: no development sysroot at $SYSROOT (run tools/bootstrap-dev-sysroot.sh)" >&2
    exit 1
fi

mkdir -p "$OUT_DIR"
wayland-scanner client-header "$XML" "$OUT_DIR/fake-input-client.h"
wayland-scanner private-code "$XML" "$OUT_DIR/fake-input-protocol.c"

# wayland-scanner emits C, so it has to be compiled as C; the generated header
# carries extern "C" guards, so the C++ side links against it fine.
gcc -std=gnu11 -O2 -Wall -c "$OUT_DIR/fake-input-protocol.c" \
    -o "$OUT_DIR/fake-input-protocol.o" -I"$OUT_DIR"

g++ -std=c++20 -O2 -Wall -o "$OUT_DIR/fake-input-injector" \
    "$ROOT_DIR/tools/fake-input-injector.cpp" \
    "$OUT_DIR/fake-input-protocol.o" \
    -I"$OUT_DIR" \
    -I"$SYSROOT/usr/include" \
    -L"$SYSROOT/usr/lib64" \
    -lwayland-client

echo "built $OUT_DIR/fake-input-injector"
