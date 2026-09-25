trail
=====

Intra-frame trail: a KWin effect that redraws the cursor at every position sampled during the previous frame. Modern mouses report far more positions per frame than the compositor draws, so a moving pointer leaves a trail of the copies it would otherwise skip; drawing them improves the pointer's visual smoothness and click accuracy. C++ and OpenGL, built against kwin's effect API. Needs an OpenGL compositor.

Not just for extreme 8000Hz mice: Even with a standard 250Hz office mouse on a 60Hz display, it quadruples the perceived cursor density, turning choppy, nauseating cursor jumping into smooth, readable motion.


Requirements
------------

KWin 6.7.x. The source only uses the effect API as it exists throughout the 6.7
series, so any 6.7 point release builds it. CMakeLists.txt asks for KWin without a
version, and the pin comes from KWin instead: config-kwin.h embeds the full version
in the plugin IID (org.kde.kwin.EffectPluginFactory6.7.5), which kwin compares with
its own before instantiating anything. kwin-devel must therefore be the exact same
version as the running kwin, and the effect has to be rebuilt after every kwin
upgrade -- a 6.7.0 build does not load on 6.7.5.

On Fedora:

    dnf install cmake ninja-build gcc-c++ extra-cmake-modules \
        qt6-qtbase-devel kf6-kcoreaddons-devel kwin-devel

That pulls in the rest: CMake >= 3.20, Qt6 Core/Gui/Widgets/DBus, KF6 CoreAddons.

Without root, unpack the devel packages into ./.sysroot/ instead; helpers/build.sh
finds that directory on its own.


Build
-----

    cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_INSTALL_PREFIX="$HOME/.local"
    cmake --build build
    cmake --install build

Add -DCMAKE_PREFIX_PATH="$PWD/.sysroot/usr" to build against a local sysroot.
helpers/build.sh is a wrapper around the same three commands; it takes PREFIX,
BUILD_DIR, BUILD_TYPE and SYSROOT from the environment.

Either way the plugin lands in
~/.local/lib64/qt6/plugins/kwin/effects/plugins/trail.so

AUTOMOC cannot see the file name inside the KWIN_EFFECT_FACTORY macro, so when you
edit src/metadata.json the embedded metadata is not regenerated on its own:

    rm -rf build/src/trail_autogen

helpers/build.sh does that for you.

A running kwin keeps executing the build it loaded, so a rebuild alone is invisible
until the plugin is reloaded; helpers/build.sh reloads it automatically when the
effect is loaded, and docs/USAGE.md has the manual calls ("Reload after a rebuild").


Enable
------

kwin finds effect plugins through Qt's plugin paths, which do not include ~/.local.
Put the directory into the session environment once:

    helpers/install-session-env.sh

then log out and back in, then switch the effect on:

    helpers/enable-effect.sh enable

or tick it under System Settings -> Window Management -> Desktop Effects. To ask
the running compositor what it sees:

    gdbus call --session --dest org.kde.KWin --object-path /Effects \
        --method org.kde.kwin.Effects.isEffectSupported trail

false means kwin has not picked up the environment yet; isEffectLoaded says whether
the effect is actually loaded.

To skip the re-login, run a nested compositor instead:

    helpers/run-nested.sh


Configuration
-------------

~/.config/kwinrc:

    [Plugins]
    trailEnabled=true

    [Effect-trail]
    Enabled=true
    TrailFrames=1
    MaxSamples=256

TrailFrames is how many frame intervals of samples to keep. 1 is a trail exactly one
frame long; raise it for a longer one. MaxSamples caps how many cursors are drawn per
frame. kwin re-reads kwinrc when it changes.


Files
-----

    src/trailsample.{h,cpp}   sample ring buffer, cursor geometry, sampling window
    src/traileffect.{h,cpp}   Effect + InputEventSpy, damage tracking, GL drawing
    src/main.cpp              plugin factory
    src/metadata.json         embedded plugin metadata
    helpers/                  build and session helpers
    docs/USAGE.md             discovery, configuration, troubleshooting
    docs/DESIGN.md            design and API research


Notes
-----

The plugin id is the file name, trail.so, not a field in metadata.json.

Screenshots and screencasts include the trail: a capture pass renders the scene into a
target of its own and this effect draws the same snapshot into it. TRAIL_KWIN_SELFCHECK=1
makes the effect read back its own pixels instead and log how many it changed. A window
capture is the exception, because it renders the window directly without the effect chain.

Shake Cursor magnifies the pointer on fast back-and-forth motion, which is exactly the
motion that reveals a trail. helpers/run-nested.sh switches it off.

A nested kwin started by hand needs its own XDG_CONFIG_HOME and D-Bus session.
Otherwise it rewrites your real kwinrc and kglobalshortcutsrc, and can leave the host's
kwin global shortcuts component inactive -- Meta+D, Alt+Tab and the rest stop working
until you log in again. helpers/run-nested.sh isolates both.


License
-------

GPL-2.0-or-later, inherited from KWin: the effect includes GPL-2.0-or-later KWin
headers and links libkwin. See LICENSE; full text in LICENSES/GPL-2.0-or-later.txt.
