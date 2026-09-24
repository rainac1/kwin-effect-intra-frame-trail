#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Trail KWin Effect contributors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# enable-effect.sh -- switch the effect on or off for the current user, and
# report whether the running KWin can actually see it.
#
# This edits the user's own kwinrc ([Plugins] trailEnabled), so it affects the
# running desktop session. Use helpers/run-nested.sh if you want an isolated
# environment instead.
#
# Usage: helpers/enable-effect.sh [enable|disable|status]
#
set -euo pipefail

action=${1:-enable}
PLUGIN_ID=trail

# Ask the running compositor over D-Bus. KWin exports
# org.kde.kwin.Effects on /Effects with isEffectSupported()/isEffectLoaded().
kwin_effects_call() {
    local method=$1
    command -v gdbus >/dev/null 2>&1 || return 1
    # gdbus prints a GVariant tuple such as "(false,)"; reduce it to true/false.
    gdbus call --session --dest org.kde.KWin --object-path /Effects \
        --method "org.kde.kwin.Effects.$method" "$PLUGIN_ID" 2>/dev/null \
        | tr -d '(), '
}

report_state() {
    local supported loaded
    if ! supported=$(kwin_effects_call isEffectSupported); then
        echo "  (could not reach the running KWin over D-Bus; is this a Plasma session?)"
        return
    fi
    loaded=$(kwin_effects_call isEffectLoaded)
    echo "  KWin sees the plugin : $supported"
    echo "  effect is loaded     : $loaded"
    case "$supported" in
    *false*)
        cat <<'EOF'

  KWin does not know the plugin yet. That usually means QT_PLUGIN_PATH does not
  include the user-local Qt plugin directory. Fix it with:
      helpers/install-session-env.sh
  and then log out and back in.
EOF
        ;;
    esac
}

case "$action" in
enable)
    kwriteconfig6 --file kwinrc --group Plugins --key "$PLUGIN_ID"Enabled true
    echo "effect enabled (kwinrc [Plugins] ${PLUGIN_ID}Enabled=true)"
    report_state
    ;;
disable)
    kwriteconfig6 --file kwinrc --group Plugins --key "$PLUGIN_ID"Enabled false
    echo "effect disabled (kwinrc [Plugins] ${PLUGIN_ID}Enabled=false)"
    report_state
    ;;
status)
    value=$(kreadconfig6 --file kwinrc --group Plugins --key "$PLUGIN_ID"Enabled --default false)
    echo "kwinrc ${PLUGIN_ID}Enabled = $value"
    report_state
    ;;
*)
    echo "usage: $0 [enable|disable|status]" >&2
    exit 2
    ;;
esac

if [[ "$action" != "status" ]]; then
    echo
    echo "KWin applies effect configuration changes at runtime; if the effect does"
    echo "not appear immediately, toggle it in System Settings > Window Management"
    echo "> Desktop Effects, or restart the session."
fi
