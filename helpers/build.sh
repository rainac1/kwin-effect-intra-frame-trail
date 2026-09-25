#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Trail KWin Effect contributors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# build.sh -- configure, build and install the effect.
#
# Installing does not change what a running kwin executes: it keeps the .so it loaded
# for the whole session (Qt's plugin loader caches a plugin by path, so unloadEffect +
# loadEffect re-creates the effect from the same mapping). A rebuilt plugin only runs
# after logging out and back in; this script says so when the effect is loaded.
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

# A running kwin keeps the .so it loaded for the whole session, so installing a new
# build changes nothing until the compositor is restarted. Say so when the effect is
# loaded. Never fatal -- without a session bus, or with the plugin unknown to kwin, the
# kwinrc value still applies at the next login.
warn_about_running_effect() {
    local plugin_id=trail reply
    command -v gdbus >/dev/null 2>&1 || return 0
    if ! reply=$(gdbus call --session --dest org.kde.KWin --object-path /Effects \
        --method org.kde.kwin.Effects.isEffectLoaded "$plugin_id" 2>/dev/null); then
        return 0 # no running kwin on this session bus
    fi
    if [[ "$reply" != *true* ]]; then
        return 0 # not loaded, nothing to say
    fi
    echo
    echo "note: '$plugin_id' is loaded in the running kwin, which keeps the .so it"
    echo "      started with. Log out and back in to run this build."
}

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
cmake --install "$BUILD_DIR"

warn_about_running_effect

echo
echo "installed plugin:"
for libdir in lib64 lib; do
    installed="$PREFIX/$libdir/qt6/plugins/kwin/effects/plugins/trail.so"
    if [[ -f "$installed" ]]; then
        ls -l "$installed" | sed 's/^/  /'
    fi
done
echo
echo "next steps:"
echo "  helpers/install-session-env.sh    # once: let KWin find plugins in ~/.local,"
echo "                                    # then log out and back in"
echo "  helpers/enable-effect.sh enable   # enable it for the next session and load it now"
echo "  helpers/run-nested.sh             # or try it in an isolated nested compositor"
echo
echo "a rebuilt plugin runs after the next login; see docs/USAGE.md (\"Apply a rebuild\")"
