#!/usr/bin/env bash
#
# build.sh -- configure, build and install the effect.
#
# Defaults to installing into ~/.local (per the project conventions: never into
# /usr). If a local development sysroot exists it is used automatically, so the
# build works on machines without kwin-devel installed system wide.
#
# Environment:
#   BUILD_DIR   build directory          (default: <repo>/build)
#   BUILD_TYPE  CMake build type         (default: RelWithDebInfo)
#   PREFIX      install prefix           (default: ~/.local)
#   SYSROOT     local dev sysroot        (default: <repo>/.sysroot)
#
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-"$ROOT_DIR/build"}
BUILD_TYPE=${BUILD_TYPE:-RelWithDebInfo}
PREFIX=${PREFIX:-"$HOME/.local"}
SYSROOT=${SYSROOT:-"$ROOT_DIR/.sysroot"}

cmake_args=(
    -S "$ROOT_DIR"
    -B "$BUILD_DIR"
    -G Ninja
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
    -DCMAKE_INSTALL_PREFIX="$PREFIX"
)

if [[ -d "$SYSROOT/usr/include/kwin" ]]; then
    echo "using local development sysroot: $SYSROOT"
    cmake_args+=(-DCMAKE_PREFIX_PATH="$SYSROOT/usr")
fi

cmake "${cmake_args[@]}"

# AUTOMOC cannot see the JSON file name inside the KWIN_EFFECT_FACTORY macro, so
# it does not know that the embedded plugin metadata changed. Drop the autogen
# output for the plugin when metadata.json is newer than the built plugin, so the
# metadata is regenerated instead of silently keeping the old values.
plugin_file="$BUILD_DIR/plugins/kwin/effects/plugins/trail.so"
if [[ -f "$ROOT_DIR/src/metadata.json" && -f "$plugin_file" ]] \
    && [[ "$ROOT_DIR/src/metadata.json" -nt "$plugin_file" ]]; then
    echo "metadata.json changed: regenerating plugin metadata"
    rm -rf "$BUILD_DIR/src/trail_autogen"
fi

cmake --build "$BUILD_DIR" --parallel "$(nproc)"
ctest --test-dir "$BUILD_DIR" --output-on-failure
cmake --install "$BUILD_DIR"

echo
echo "installed plugin:"
for libdir in lib64 lib; do
    installed="$PREFIX/$libdir/qt6/plugins/kwin/effects/plugins/trail.so"
    if [[ -f "$installed" ]]; then
        ls -l "$installed" | sed 's/^/  /'
    fi
done
echo
echo "enable it with:"
echo "  helpers/enable-effect.sh enable   # for the current user's session"
echo "  helpers/nested-e2e.sh             # isolated nested compositor + screenshot"
