#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Trail KWin Effect contributors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# run-nested.sh -- run an isolated, nested KWin session to test the effect.
#
# This never touches the host session: it starts a separate kwin_wayland that
# renders into a window on the current desktop, with its own D-Bus session, its
# own configuration and its own socket. The host compositor keeps running.
#
# Environment:
#   SOCKET          nested Wayland socket name      (default: wayland-dev)
#   RUNTIME_DIR     scratch dir for config/cache   (default: <repo>/.runtime)
#   PLUGIN_PREFIX   prefix the effect is installed (default: ~/.local)
#   CLIENT          client to run inside; set to "" for none (default: konsole)
#   DURATION        stop after N seconds, 0 = run until the client exits
#   WIDTH, HEIGHT   nested output size
#   TRAIL_FRAMES    value for the effect's TrailFrames setting (default: 1)
#
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
SOCKET=${SOCKET:-wayland-dev}
RUNTIME_DIR=${RUNTIME_DIR:-"$ROOT_DIR/.runtime"}
CONFIG_HOME="$RUNTIME_DIR/config"
CACHE_HOME="$RUNTIME_DIR/cache"
PLUGIN_PREFIX=${PLUGIN_PREFIX:-"$HOME/.local"}
# Note: ${CLIENT-...} and not ${CLIENT:-...}, so that CLIENT="" means "no client".
CLIENT=${CLIENT-konsole}
DURATION=${DURATION:-0}
WIDTH=${WIDTH:-1280}
HEIGHT=${HEIGHT:-720}
TRAIL_FRAMES=${TRAIL_FRAMES:-1}

mkdir -p "$CONFIG_HOME" "$CACHE_HOME"

# Isolated configuration: the effect has to be switched on in [Plugins] using
# the plugin id, and its own settings live in [Effect-<id>].
cat > "$CONFIG_HOME/kwinrc" <<EOF
[Plugins]
trail-capturableEnabled=true
# Shake Cursor magnifies the pointer when it is moved quickly back and forth,
# which is exactly what one does when looking at the trail; the magnified
# pointer would obscure it. It is on by default, so switch it off here.
shakecursorEnabled=false

[Effect-trail-capturable]
Enabled=true
TrailFrames=$TRAIL_FRAMES
MaxSamples=256
EOF

export QT_PLUGIN_PATH="$PLUGIN_PREFIX/lib64/qt6/plugins:$PLUGIN_PREFIX/lib/qt6/plugins${QT_PLUGIN_PATH:+:$QT_PLUGIN_PATH}"
export XDG_DATA_DIRS="$PLUGIN_PREFIX/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
export XDG_CONFIG_HOME="$CONFIG_HOME"
export XDG_CACHE_HOME="$CACHE_HOME"
# Without this, KWin sends its logging to the journal instead of stderr.
export QT_FORCE_STDERR_LOGGING=1
export QT_LOGGING_RULES="kwin_effect_trail_capturable.debug=true${QT_LOGGING_RULES:+;$QT_LOGGING_RULES}"

args=(--socket "$SOCKET" --width "$WIDTH" --height "$HEIGHT")
if [[ -n "$CLIENT" ]]; then
    args+=("$CLIENT")
fi

echo "nested KWin:"
echo "  socket      : $SOCKET"
echo "  config      : $CONFIG_HOME (TrailFrames=$TRAIL_FRAMES)"
echo "  plugins     : $QT_PLUGIN_PATH"
echo "  client      : ${CLIENT:-<none>}"
echo "  attach with : WAYLAND_DISPLAY=$SOCKET <command>"
echo

if [[ "$DURATION" != "0" ]]; then
    exec timeout --signal=TERM "$DURATION" dbus-run-session kwin_wayland "${args[@]}"
else
    exec dbus-run-session kwin_wayland "${args[@]}"
fi
