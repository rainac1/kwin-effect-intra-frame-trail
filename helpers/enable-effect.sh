#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Trail KWin Effect contributors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# enable-effect.sh -- switch the effect on or off for the current user and report whether
# the running KWin can actually see it.
#
# The kwinrc value ([Plugins] trailEnabled) is what persists the choice for the next
# login; it is only read at session start, so enable/disable/reload also act on the
# running compositor over D-Bus. Use helpers/run-nested.sh for an isolated
# environment instead.
#
# reload re-creates the effect, but a running kwin keeps the .so it started with, so it
# does not switch to a rebuilt plugin: that needs a new login (docs/USAGE.md).
#
# Usage: helpers/enable-effect.sh [enable|disable|reload|status]
#
set -euo pipefail

action=${1:-enable}
PLUGIN_ID=trail

# Ask the running compositor over D-Bus. KWin exports
# org.kde.kwin.Effects on /Effects with isEffectSupported(), isEffectLoaded(),
# loadEffect() and unloadEffect().
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
    if loaded=$(kwin_effects_call loadEffect); then
        case "$loaded" in
        true) echo "  loaded into the running session (D-Bus loadEffect)" ;;
        *) echo "  the running kwin did not load it; see the state below" ;;
        esac
    else
        echo "  (could not reach the running KWin over D-Bus; kwinrc applies at next login)"
    fi
    report_state
    ;;
disable)
    kwriteconfig6 --file kwinrc --group Plugins --key "$PLUGIN_ID"Enabled false
    echo "effect disabled (kwinrc [Plugins] ${PLUGIN_ID}Enabled=false)"
    if kwin_effects_call unloadEffect >/dev/null; then
        echo "  unloaded from the running session (D-Bus unloadEffect)"
    else
        echo "  (could not reach the running KWin over D-Bus; kwinrc applies at next login)"
    fi
    report_state
    ;;
reload)
    # Re-creates the effect from the mapping kwin already has. It does not read the
    # .so again, so a rebuilt plugin needs a new login instead.
    if ! kwin_effects_call unloadEffect >/dev/null; then
        echo "error: could not reach the running KWin over D-Bus; nothing to reload" >&2
        exit 1
    fi
    if loaded=$(kwin_effects_call loadEffect); then
        echo "plugin '$PLUGIN_ID' re-created for the running session (unloadEffect + loadEffect)"
        echo "  note: this is the build kwin loaded at login, not a rebuilt .so"
        if [[ "$loaded" != "true" ]]; then
            echo "  warning: KWin reported loadEffect=$loaded" >&2
        fi
    fi
    report_state
    ;;
status)
    value=$(kreadconfig6 --file kwinrc --group Plugins --key "$PLUGIN_ID"Enabled --default false)
    echo "kwinrc ${PLUGIN_ID}Enabled = $value"
    report_state
    ;;
*)
    echo "usage: $0 [enable|disable|reload|status]" >&2
    exit 2
    ;;
esac

if [[ "$action" != "status" ]]; then
    echo
    echo "kwinrc is only read when the session starts, so the kwinrc value is what"
    echo "persists the choice for the next login, while the D-Bus call is what changes"
    echo "this session. A rebuilt plugin needs a new login; see docs/USAGE.md."
fi
