#!/usr/bin/env bash
#
# enable-effect.sh -- switch the effect on or off for the current user.
#
# This edits the user's own kwinrc ([Plugins] trailEnabled), so it affects the
# running desktop session, not a nested test session. Use helpers/run-nested.sh
# or helpers/nested-e2e.sh if you want an isolated environment instead.
#
# Usage: helpers/enable-effect.sh [enable|disable|status]
#
set -euo pipefail

action=${1:-enable}

case "$action" in
enable)
    kwriteconfig6 --file kwinrc --group Plugins --key trailEnabled true
    echo "effect enabled (kwinrc [Plugins] trailEnabled=true)"
    ;;
disable)
    kwriteconfig6 --file kwinrc --group Plugins --key trailEnabled false
    echo "effect disabled (kwinrc [Plugins] trailEnabled=false)"
    ;;
status)
    value=$(kreadconfig6 --file kwinrc --group Plugins --key trailEnabled --default false)
    echo "trailEnabled=$value"
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
