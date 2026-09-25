#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Trail KWin Effect contributors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# build.sh -- configure, build and install the effect.
#
# After installing, an effect that is loaded in the running session is reloaded over
# D-Bus (unloadEffect + loadEffect), so no kwin restart is needed to run the new build.
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

# A running kwin keeps executing the mapping of the plugin it loaded, so installing
# a new build changes nothing until the effect is unloaded and loaded again. Do that
# here, but only when the effect is loaded: a disabled effect should stay unloaded.
# Never fatal -- without a session bus, or with the plugin unknown to kwin, the
# kwinrc value still applies at the next login.
reload_running_effect() {
    local plugin_id=trail-capturable reply
    command -v gdbus >/dev/null 2>&1 || return 0
    if ! reply=$(gdbus call --session --dest org.kde.KWin --object-path /Effects \
        --method org.kde.kwin.Effects.isEffectLoaded "$plugin_id" 2>/dev/null); then
        return 0 # no running kwin on this session bus
    fi
    if [[ "$reply" != *true* ]]; then
        return 0 # not loaded, nothing to switch over
    fi
    gdbus call --session --dest org.kde.KWin --object-path /Effects \
        --method org.kde.kwin.Effects.unloadEffect "$plugin_id" >/dev/null 2>&1 || true
    if gdbus call --session --dest org.kde.KWin --object-path /Effects \
        --method org.kde.kwin.Effects.loadEffect "$plugin_id" >/dev/null 2>&1; then
        echo "reloaded '$plugin_id' in the running kwin (unloadEffect + loadEffect)"
    else
        echo "warning: unloaded '$plugin_id' but could not load it again;" >&2
        echo "         it will be back at the next login" >&2
    fi
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
plugin_file="$BUILD_DIR/plugins/kwin/effects/plugins/trail-capturable.so"
if [[ -f "$ROOT_DIR/src/metadata.json" && -f "$plugin_file" ]] \
    && [[ "$ROOT_DIR/src/metadata.json" -nt "$plugin_file" ]]; then
    echo "metadata.json changed: regenerating plugin metadata"
    rm -rf "$BUILD_DIR/src/trail-capturable_autogen"
fi

cmake --build "$BUILD_DIR" --parallel "$(nproc)"
cmake --install "$BUILD_DIR"

reload_running_effect

echo
echo "installed plugin:"
for libdir in lib64 lib; do
    installed="$PREFIX/$libdir/qt6/plugins/kwin/effects/plugins/trail-capturable.so"
    if [[ -f "$installed" ]]; then
        ls -l "$installed" | sed 's/^/  /'
    fi
done
echo
echo "next steps:"
echo "  helpers/install-session-env.sh    # once: let KWin find plugins in ~/.local,"
echo "                                    # then log out and back in"
echo "  helpers/enable-effect.sh enable   # enable and load it in the current session"
echo "  helpers/enable-effect.sh reload   # reload by hand after an install"
echo "  helpers/run-nested.sh             # or try it in an isolated nested compositor"
echo
echo "details: docs/USAGE.md"
