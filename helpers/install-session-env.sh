#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Trail KWin Effect contributors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# install-session-env.sh -- make the user-local KWin effect plugin discoverable.
#
# KWin finds binary effect plugins with
# KPluginMetaData::findPlugins("kwin/effects/plugins"), which searches Qt's
# plugin paths. Qt's defaults are only the system directories
# (e.g. /usr/lib64/qt6/plugins); a plugin in ~/.local is NOT found unless
# QT_PLUGIN_PATH is set. Plasma 6 starts KWin from a systemd user unit, so the
# right place to set it is ~/.config/environment.d/, which the systemd user
# manager reads at login.
#
# A re-login is required for the change to take effect.
#
# Usage:
#   helpers/install-session-env.sh             # install / update
#   helpers/install-session-env.sh --remove    # undo
#
set -euo pipefail

PLUGIN_PREFIX=${PLUGIN_PREFIX:-"$HOME/.local"}
ENV_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/environment.d"
ENV_FILE="$ENV_DIR/50-kwin-trail.conf"

case "${1:-install}" in
--remove | remove)
    if [[ -f "$ENV_FILE" ]]; then
        rm -f "$ENV_FILE"
        echo "removed $ENV_FILE"
        echo "log out and back in for KWin to stop looking in the plugin directory"
    else
        echo "nothing to do: $ENV_FILE does not exist"
    fi
    exit 0
    ;;
install | "") ;;
*)
    echo "usage: $0 [install|--remove]" >&2
    exit 2
    ;;
esac

# Prefer the library directory the plugin was actually installed into.
PLUGIN_DIR=""
for libdir in lib64 lib; do
    if [[ -d "$PLUGIN_PREFIX/$libdir/qt6/plugins" ]]; then
        PLUGIN_DIR="$PLUGIN_PREFIX/$libdir/qt6/plugins"
        break
    fi
done
if [[ -z "$PLUGIN_DIR" ]]; then
    echo "error: no Qt plugin directory found under $PLUGIN_PREFIX (build and install first)" >&2
    exit 1
fi
if [[ ! -f "$PLUGIN_DIR/kwin/effects/plugins/trail-capturable.so" ]]; then
    echo "warning: $PLUGIN_DIR/kwin/effects/plugins/trail-capturable.so not found;" >&2
    echo "         run helpers/build.sh first" >&2
fi

# Do not clobber a value that is already set for the session.
existing=""
if command -v systemctl >/dev/null 2>&1; then
    existing=$(systemctl --user show-environment 2>/dev/null | sed -n 's/^QT_PLUGIN_PATH=//p' || true)
fi
if [[ -z "$existing" ]]; then
    existing=${QT_PLUGIN_PATH:-}
fi

case "$existing" in
"") value="$PLUGIN_DIR" ;;
*)
    if [[ ":$existing:" == *":$PLUGIN_DIR:"* ]]; then
        value="$existing"
    else
        value="$PLUGIN_DIR:$existing"
    fi
    ;;
esac

mkdir -p "$ENV_DIR"
cat > "$ENV_FILE" <<EOF
# Installed by trail-kwin-effect (docs/USAGE.md).
# KWin discovers binary effect plugins through Qt's plugin paths, which do not
# include user directories by default.
QT_PLUGIN_PATH=$value
EOF

echo "wrote $ENV_FILE:"
sed 's/^/  /' "$ENV_FILE"
cat <<EOF

Next steps:
  1. log out and back in (the systemd user manager reads this file at login)
  2. verify:  systemctl --user show-environment | grep QT_PLUGIN_PATH
  3. enable:  helpers/enable-effect.sh enable
              (or System Settings > Window Management > Desktop Effects)

To use the effect right now without re-logging in, run it in a nested session:
  helpers/run-nested.sh
EOF
