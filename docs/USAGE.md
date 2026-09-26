Usage
=====

Discovery, enabling and troubleshooting. Building and installing is in the README;
design and API research is in DESIGN.md.


How kwin finds the plugin
-------------------------

kwin discovers binary effects with KPluginMetaData::findPlugins("kwin/effects/plugins"),
which searches Qt's plugin paths. Qt's defaults are the system directories, so an
install under /usr is found with no environment variable:

    /usr/lib64/qt6/plugins/kwin/effects/plugins/trail.so             Fedora
    /usr/lib/x86_64-linux-gnu/qt6/plugins/kwin/effects/plugins/...   Debian/Ubuntu

This is why the README installs with -DCMAKE_INSTALL_PREFIX=/usr. Qt does not search
/usr/local, so a plugin installed there (the plain `cmake ..` default) stays invisible
even though `sudo make install` reported success.

~/.local/share/kwin/effects/ is for scripted/QML effects (KPackage). Binary plugins do
not go there.

The plugin id is its file name, trail.so. That is why the target is built without the
lib prefix and why the kwinrc switch is [Plugins] trailEnabled.


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

kwinrc persists the choice for the next session; the D-Bus calls change the running one:

    # enable
    kwriteconfig6 --file kwinrc --group Plugins --key trailEnabled true
    gdbus call --session --dest org.kde.KWin --object-path /Effects \
        --method org.kde.kwin.Effects.loadEffect trail

    # disable
    kwriteconfig6 --file kwinrc --group Plugins --key trailEnabled false
    gdbus call --session --dest org.kde.KWin --object-path /Effects \
        --method org.kde.kwin.Effects.unloadEffect trail

    # re-create the effect without touching kwinrc (same build)
    gdbus call --session --dest org.kde.KWin --object-path /Effects \
        --method org.kde.kwin.Effects.unloadEffect trail
    gdbus call --session --dest org.kde.KWin --object-path /Effects \
        --method org.kde.kwin.Effects.loadEffect trail

    # query
    gdbus call --session --dest org.kde.KWin --object-path /Effects \
        --method org.kde.kwin.Effects.isEffectSupported trail
    gdbus call --session --dest org.kde.KWin --object-path /Effects \
        --method org.kde.kwin.Effects.isEffectLoaded trail

The same switch is the "Intra-frame Trail" checkbox under System Settings -> Window
Management -> Desktop Effects, whose Apply button makes the loadEffect/unloadEffect call
for you.


Apply a rebuild
---------------

A running kwin keeps the .so it loaded for the whole session. Qt's plugin loader caches a
plugin by path, so unloadEffect + loadEffect re-creates the effect from the same mapping
rather than reading the file again; rebuilding and installing changes the file on disk but
not what the compositor runs.

    log out and back in      the only way to run a rebuilt plugin

reconfigureEffect is not a reload either: it calls Effect::reconfigure() on the already
loaded instance, for [Effect-trail] settings only. Metadata is the exception:
PluginEffectLoader::findEffect() asks for the plugin metadata on every call instead of
caching it, so a build whose embedded metadata is invalid reports isEffectSupported false,
and the next good install flips it back to true with no restart. That is discovery only;
the running effect still executes the build it started with.

`qdbus org.kde.KWin /KWin reconfigure` does not reset the plugin cache either. It reaches
Workspace::slotReconfigure(), which reparses the config, updates Options and reloads the
window rules; it never calls the effect loader. The only caller of
EffectLoader::queryAndLoadAll() is EffectsHandler::reconfigure(), which runs once, from the
EffectsHandler constructor at session start. Even that would not re-read a loaded .so:
PluginEffectLoader::loadEffect() returns early while the name is in m_loadedEffects, and
PluginEffectLoader::clear() is an empty function.

[Plugins] trailEnabled persists the choice for the next session. A plain kwriteconfig6 edit
does not load or unload a running kwin -- measured, no change after six seconds either way.
KConfig only emits the org.kde.kconfig.notify D-Bus signal for entries written with the
KConfig::Notify flag, and EffectsHandler::configChanged (the live load/unload hook, connected
to KConfigWatcher) is driven by that signal. kwriteconfig6 sets the flag only with --notify,
so either pass it:

    kwriteconfig6 --file kwinrc --group Plugins --key trailEnabled true --notify

or write kwinrc for the next login and use loadEffect/unloadEffect for the running session,
which is also what the Desktop Effects Apply button does.

That live call needs the running kwin to have the plugin on Qt's plugin path. A system-wide
install under /usr is already there. A process cannot gain a plugin path at runtime, so a
plugin installed under /usr/local only helps after it is reinstalled into /usr (or tried in
a nested session, below).


Configuration
-------------

~/.config/kwinrc, group [Effect-trail]:

    Enabled       default true    runtime switch; does not unload the plugin
    TrailFrames   default 1       frame intervals of samples to keep
    MaxSamples    default 256     per-frame draw budget

    kwriteconfig6 --file kwinrc --group Effect-trail --key TrailFrames 6

If a change does not take effect:

    gdbus call --session --dest org.kde.KWin --object-path /Effects \
        --method org.kde.kwin.Effects.reconfigureEffect trail

TrailFrames=1 gives a trail exactly one frame long: flick the pointer to see it.


Uninstall
---------

    kwriteconfig6 --file kwinrc --group Plugins --key trailEnabled false
    sudo rm /usr/lib64/qt6/plugins/kwin/effects/plugins/trail.so

Adjust the path for the distribution (see "How kwin finds the plugin"), and log out and
back in so the removed plugin is not loaded from a stale session. The effect's settings
stay in ~/.config/kwinrc under [Effect-trail]; remove that group with kwriteconfig6 if
you want them gone.


Nested session
--------------

An isolated kwin with its own D-Bus, config, cache and socket, drawn into a window on
the current desktop, is the safe way to try a build whose ABI may not match: a bad plugin
can crash the compositor process, and this keeps that out of the real session.

    RUNTIME_DIR=${XDG_RUNTIME_DIR:-/tmp}/trail-nested
    mkdir -p "$RUNTIME_DIR/config" "$RUNTIME_DIR/cache"
    cat > "$RUNTIME_DIR/config/kwinrc" <<'EOF'
    [Plugins]
    trailEnabled=true
    shakecursorEnabled=false

    [Effect-trail]
    Enabled=true
    TrailFrames=1
    MaxSamples=256
    EOF
    XDG_CONFIG_HOME="$RUNTIME_DIR/config" \
    XDG_CACHE_HOME="$RUNTIME_DIR/cache" \
    QT_LOGGING_RULES="kwin_effect_trail.debug=true" \
        dbus-run-session kwin_wayland \
            --socket wayland-dev --width 1280 --height 720 konsole

    WAYLAND_DISPLAY=wayland-dev dolphin          # attach another client

Never run kwin_wayland --replace or restart the display manager in the host session;
that is how the desktop goes black and windows are lost. The nested instance picks up the
system-wide plugin directly, without touching the real session and without a re-login.


Logs
----

    journalctl --user -u plasma-kwin_wayland -f | grep kwin_effect_trail
    QT_LOGGING_RULES="kwin_effect_trail.debug=true" dbus-run-session kwin_wayland ...


Problems
--------

Effect missing from Desktop Effects, isEffectSupported false:
    the plugin is not on Qt's plugin path. Reinstall with
    -DCMAKE_INSTALL_PREFIX=/usr; Qt does not search /usr/local.

isEffectSupported true, but nothing is drawn:
    [Effect-trail] Enabled is false, or the session is not compositing with OpenGL.

Listed but greyed out, log says "has not matching plugin version":
    built against a different kwin. Rebuild.

Rebuilt, but the running compositor still behaves the old way:
    it keeps the .so it loaded for the whole session. Log out and back in; see
    "Apply a rebuild".

No kwin_effect_trail output at all:
    the plugin is not loaded; see the first two entries.

Trail does not show up in a screenshot or screencast:
    expected. Use TRAIL_KWIN_SELFCHECK=1 to verify the pixels on screen.

Pointer gets bigger while shaking it:
    Shake Cursor.
    kwriteconfig6 --file kwinrc --group Plugins --key shakecursorEnabled false


Terms
-----

effect chain      effects run in sequence each frame; this one draws above windows
damage            the region that has to be repainted; less damage, less power
output / scale    a display; logical coordinates * scale = device pixels
KPluginMetaData   KDE plugin discovery metadata, embedded in trail.so
InputEventSpy     internal interface for observing input events without consuming them
