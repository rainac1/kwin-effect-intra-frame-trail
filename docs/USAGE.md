Usage
=====

Discovery, enabling and troubleshooting. Building is in the README; design and API
research is in DESIGN.md.


How kwin finds the plugin
-------------------------

kwin discovers binary effects with KPluginMetaData::findPlugins("kwin/effects/plugins"),
which searches Qt's plugin paths. Qt's defaults are the system directories only
(/usr/lib64/qt6/plugins on Fedora), so a plugin under ~/.local stays invisible until
QT_PLUGIN_PATH includes it.

    /usr/lib64/qt6/plugins/...          found by default; distro packaging, needs root
    ~/.local/lib64/qt6/plugins/...      needs QT_PLUGIN_PATH

~/.local/share/kwin/effects/ is for scripted/QML effects (KPackage). Binary plugins do
not go there.

Plasma 6 starts kwin from a systemd user unit, so the variable belongs in
~/.config/environment.d/ and is read at login:

    helpers/install-session-env.sh
    systemctl --user show-environment | grep QT_PLUGIN_PATH

The plugin id is its file name, trail-capturable.so. That is why the target is built without the
lib prefix and why the kwinrc switch is [Plugins] trail-capturableEnabled.


Version guard
-------------

The factory IID carries kwin's full version (config-kwin.h):

    org.kde.kwin.EffectPluginFactory6.7.5

PluginEffectLoader::factory() compares it against the plugin's embedded metadata before
instantiating anything. On a mismatch it logs one qCDebug(KWIN_CORE) line and returns
nullptr:

    "trail" has not matching plugin version, expected ...6.7.5 got ...6.7.4
    Couldn't get an EffectPluginFactory for: "trail"

No crash, other effects load, the compositor keeps running. Discovery is metadata-only
and version-independent, so the effect is still listed but isEffectSupported is false:
listed, greyed out. The log is debug level, so after a kwin upgrade the symptom is
usually just "the effect disappeared". Rebuild against the matching kwin-devel.

If the version matches but the ABI does not, the guard passes and the plugin is
instantiated inside the compositor process, unsandboxed. Test in a nested session.


Enable, disable, query
----------------------

    helpers/enable-effect.sh enable     # [Plugins] trail-capturableEnabled=true, then loadEffect
    helpers/enable-effect.sh disable    # [Plugins] trail-capturableEnabled=false, then unloadEffect
    helpers/enable-effect.sh reload     # unload + load the effect, same build, kwinrc untouched
    helpers/enable-effect.sh status     # kwinrc value plus live D-Bus state

    gdbus call --session --dest org.kde.KWin --object-path /Effects \
        --method org.kde.kwin.Effects.isEffectSupported trail-capturable
    gdbus call --session --dest org.kde.KWin --object-path /Effects \
        --method org.kde.kwin.Effects.isEffectLoaded trail-capturable

Loading without touching kwinrc:

    gdbus call --session --dest org.kde.KWin --object-path /Effects \
        --method org.kde.kwin.Effects.loadEffect trail-capturable


Apply a rebuild
---------------

A running kwin keeps the .so it loaded for the whole session. Qt's plugin loader caches a
plugin by path, so unloadEffect + loadEffect re-creates the effect from the same mapping
rather than reading the file again; rebuilding and installing changes the file on disk but
not what the compositor runs.

    log out and back in      the only way to run a rebuilt plugin
    helpers/build.sh         installs, then says the same

reconfigureEffect is not a reload either: it calls Effect::reconfigure() on the already
loaded instance, for [Effect-trail-capturable] settings only. Metadata is the exception:
PluginEffectLoader::findEffect() asks for the plugin metadata on every call instead of
caching it, so a build whose embedded metadata is invalid reports isEffectSupported false,
and the next good install flips it back to true with no restart. That is discovery only;
the running effect still executes the build it started with.

[Plugins] trail-capturableEnabled is read when the session starts and not before: editing it,
with kwriteconfig6 or anything else, does not load or unload a running kwin -- measured,
no change after six seconds either way. helpers/enable-effect.sh therefore writes it
for the next login and then makes the change happen now with loadEffect/unloadEffect.

This needs the running kwin to already have the user plugin directory on Qt's path
(helpers/install-session-env.sh, then one login). A process cannot gain a plugin path
at runtime; if isEffectSupported is false for that reason, only a re-login or
helpers/run-nested.sh helps.


Configuration
-------------

~/.config/kwinrc, group [Effect-trail-capturable]:

    Enabled       default true    runtime switch; does not unload the plugin
    TrailFrames   default 1       frame intervals of samples to keep
    MaxSamples    default 256     per-frame draw budget

    kwriteconfig6 --file kwinrc --group Effect-trail-capturable --key TrailFrames 6

If a change does not take effect:

    gdbus call --session --dest org.kde.KWin --object-path /Effects \
        --method org.kde.kwin.Effects.reconfigureEffect trail-capturable

TrailFrames=1 gives a trail exactly one frame long: flick the pointer to see it.


Uninstall
---------

    helpers/enable-effect.sh disable
    helpers/install-session-env.sh --remove      # applies at next login
    rm -f ~/.local/lib64/qt6/plugins/kwin/effects/plugins/trail-capturable.so


Nested session
--------------

An isolated kwin with its own D-Bus, config, cache and socket, drawn into a window on
the current desktop.

    helpers/run-nested.sh
    WAYLAND_DISPLAY=wayland-dev dolphin          # attach another client

Never run kwin_wayland --replace or restart the display manager in the host session;
that is how the desktop goes black and windows are lost. The nested instance picks up
the plugin immediately because it exports QT_PLUGIN_PATH itself, which a real session
only gets from a re-login.

Environment: TRAIL_FRAMES, CLIENT (empty for none), WIDTH, HEIGHT, DURATION, SOCKET,
RUNTIME_DIR, PLUGIN_PREFIX.


Logs
----

    journalctl --user -u plasma-kwin_wayland -f | grep kwin_effect_trail_capturable
    QT_LOGGING_RULES="kwin_effect_trail_capturable.debug=true" CLIENT=konsole helpers/run-nested.sh


Problems
--------

Effect missing from Desktop Effects, isEffectSupported false:
    Qt's plugin paths do not include ~/.local. Run helpers/install-session-env.sh and
    log back in.

isEffectSupported true, but nothing is drawn:
    [Effect-trail-capturable] Enabled is false, or the session is not compositing with OpenGL.

Listed but greyed out, log says "has not matching plugin version":
    built against a different kwin. Rebuild.

Rebuilt, but the running compositor still behaves the old way:
    it keeps the .so it loaded for the whole session. Log out and back in; see
    "Apply a rebuild".

No kwin_effect_trail_capturable output at all:
    the plugin is not loaded; see the first two entries.

Trail does not show up in a screenshot or screencast:
    a screen or area capture draws it, because those run the effect chain. A window
    capture does not: it renders the window item directly. Use TRAIL_KWIN_SELFCHECK=1
    to check the pixels on screen.

Pointer gets bigger while shaking it:
    Shake Cursor.
    kwriteconfig6 --file kwinrc --group Plugins --key shakecursorEnabled false


Terms
-----

effect chain      effects run in sequence each frame; this one draws above windows
damage            the region that has to be repainted; less damage, less power
output / scale    a display; logical coordinates * scale = device pixels
KPluginMetaData   KDE plugin discovery metadata, embedded in trail-capturable.so
InputEventSpy     internal interface for observing input events without consuming them
