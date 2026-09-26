trail
=====

Intra-frame trail: a KWin effect that redraws the cursor at every position sampled during the previous frame. Modern mouses report far more positions per frame than the compositor draws, so a moving pointer leaves a trail of the copies it would otherwise skip; drawing them improves the pointer's visual smoothness and click accuracy. C++ and OpenGL, built against kwin's effect API. Needs an OpenGL compositor.

Not just for extreme 8000Hz mice: Even with a standard 250Hz office mouse on a 60Hz display, it quadruples the perceived cursor density, turning choppy, nauseating cursor jumping into smooth, readable motion.

The following is a demonstration video; the right side shows the view with this KWin effect enabled.

https://github.com/user-attachments/assets/b65f0f27-ce55-4336-ac58-59105d327ec2


Requirements
------------

KWin 6.7.x on Plasma 6, with an OpenGL-capable compositor: the effect draws with
OpenGL.

The source only uses the effect API as it exists throughout the 6.7 series, so any 6.7
point release builds it. CMakeLists.txt asks for KWin without a version, and the pin
comes from KWin instead: config-kwin.h embeds the full version in the plugin IID
(org.kde.kwin.EffectPluginFactory6.7.5), which KWin compares with its own before
instantiating anything. The KWin development headers must therefore be the exact same
version as the running KWin, and the effect has to be rebuilt after every KWin upgrade
-- a 6.7.0 build does not load on 6.7.5. See "Pitfalls".


Installation
------------

There is no distribution package yet, so the effect is compiled from source and
installed system-wide. KWin loads the plugin from Qt's plugin directories, which on most
distributions means the install prefix has to be /usr.


1. Install the build dependencies
---------------------------------

The effect itself only uses the KWin effect API, but KWin's CMake package requires its
own full development set: Qt 6 Core/Gui/Widgets/DBus/Quick, KF6 Config/CoreAddons/
WindowSystem, libepoxy, libdrm, Vulkan and Wayland-server. CMake refuses to configure
until every one of them is present, so install the development packages even though most
of them look unrelated to a cursor effect.

Ubuntu/Debian (Plasma 6). Use a release that ships KWin 6.7.x, such as Debian forky/sid
or KDE neon; Debian trixie ships 6.3, which this effect does not load on:

    sudo apt install cmake extra-cmake-modules build-essential \
        qt6-base-dev qt6-declarative-dev \
        libkf6config-dev libkf6coreaddons-dev libkf6windowsystem-dev \
        kwin-dev libepoxy-dev libdrm-dev libvulkan-dev libwayland-dev

Arch Linux. The KWin headers and CMake files are inside the kwin package; there is no
separate -devel package:

    sudo pacman -S --needed base-devel cmake extra-cmake-modules \
        kwin qt6-base qt6-declarative \
        kconfig kcoreaddons kwindowsystem \
        libepoxy libdrm vulkan-headers vulkan-icd-loader wayland

Fedora:

    sudo dnf install cmake gcc-c++ extra-cmake-modules \
        qt6-qtbase-devel qt6-qtdeclarative-devel \
        kf6-kconfig-devel kf6-kcoreaddons-devel kf6-kwindowsystem-devel \
        kwin-devel libepoxy-devel libdrm-devel \
        vulkan-loader-devel wayland-devel

Install the KWin development package from the same repository as the running Plasma, so
that its version matches the running KWin exactly: a 6.7.4 header set cannot build a
plugin for 6.7.5 (see "Pitfalls").


2. Download the source and build
--------------------------------

The install prefix must be /usr, the same prefix as the system Qt. KWin searches the Qt
plugin directories, which do not include /usr/local, so a plain `cmake ..` installs to a
location KWin never scans and the effect stays invisible in System Settings.

    # 1. get the source
    git clone https://github.com/rainac1/kwin-effect-intra-frame-trail.git
    cd kwin-effect-intra-frame-trail

    # 2. create and enter a build directory
    mkdir build && cd build

    # 3. configure CMake (the prefix must be /usr)
    cmake .. -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=Release

    # 4. compile
    make -j"$(nproc)"

    # 5. install into the system (needs root)
    sudo make install

Installation places the compiled .so in Qt's KWin effect directory, for example
/usr/lib64/qt6/plugins/kwin/effects/plugins/trail.so on Fedora or
/usr/lib/x86_64-linux-gnu/qt6/plugins/kwin/effects/plugins/trail.so on Debian/Ubuntu.
The plugin metadata (src/metadata.json) is embedded in the .so; no separate JSON or
desktop file is installed.

AUTOMOC cannot see the file name inside the KWIN_EFFECT_FACTORY macro, so when you edit
src/metadata.json the embedded metadata is not regenerated on its own:

    rm -rf build/src/trail_autogen


3. Enable the effect
--------------------

Installing does not enable the effect. Switch it on in System Settings:

1. Open System Settings.
2. Go to Window Management -> Desktop Effects.
3. Find "Intra-frame Trail" in the list and tick its checkbox.
4. (optional) click the gear icon to change TrailFrames and MaxSamples.
5. Click Apply.

Apply loads the effect into the running compositor (it calls loadEffect over D-Bus), so no
logout is needed. The same can be done without the GUI:

    kwriteconfig6 --file kwinrc --group Plugins --key trailEnabled true
    gdbus call --session --dest org.kde.KWin --object-path /Effects \
        --method org.kde.kwin.Effects.loadEffect trail

`qdbus org.kde.KWin /KWin reconfigure` is not part of this and does not help: it re-reads
KWin's own settings (options and window rules) but never calls the effect loader, so it
neither rescans the plugin directories nor loads the effect. Enabling is the Apply button
or the loadEffect call above.

To ask the running compositor what it sees:

    gdbus call --session --dest org.kde.KWin --object-path /Effects \
        --method org.kde.kwin.Effects.isEffectSupported trail
    gdbus call --session --dest org.kde.KWin --object-path /Effects \
        --method org.kde.kwin.Effects.isEffectLoaded trail


Apply a rebuild
---------------

Installing a new build does not change what a running KWin executes. Qt's plugin loader
caches a plugin by path, so unloadEffect + loadEffect re-creates the effect from the
same mapping instead of reading the file again. A rebuilt .so therefore only runs in a
new compositor process: log out and back in after `sudo make install`.
`qdbus org.kde.KWin /KWin reconfigure` does not reset that cache and does not reload the
plugin; it never calls the effect loader at all.


Pitfalls
--------

1. Plasma 5 and Plasma 6 are incompatible.

   Plasma 6 moved KWin to Qt 6 and rebuilt its internals, so a C++ effect written for
   Plasma 5 does not load on Plasma 6, and the other way round. Build the branch that
   matches your Plasma major version.

2. KWin has no stable C++ plugin ABI.

   KWin ships no long-term stable C++ plugin ABI. Even within Plasma 6 a point release
   can change an internal API and break an existing effect, or crash KWin. On top of the
   ABI, this effect carries a hard version guard: the factory IID contains KWin's full
   version (org.kde.kwin.EffectPluginFactory6.7.5), and KWin refuses to instantiate a
   plugin built against a different one. After a KWin update the symptom is usually just
   "the effect disappeared", because the mismatch is logged at debug level. Rebuild
   against the matching KWin development package (kwin-devel, kwin-dev on
   Debian/Ubuntu). This is also why C++ effects normally have to be rebuilt after each
   system update.

3. The install prefix trap.

   A plain `cmake ..` defaults to /usr/local, and KWin's plugin search covers the system
   Qt plugin directories under /usr, not /usr/local. `sudo make install` succeeds but the
   effect never appears in Desktop Effects. Always pass
   -DCMAKE_INSTALL_PREFIX=/usr.

4. The compositor keeps the .so it started with.

   Rebuilding and installing changes the file on disk, not what the running KWin
   executes. Log out and back in; see "Apply a rebuild". `qdbus org.kde.KWin /KWin
   reconfigure` does not help -- it never reaches the effect loader.


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
frame. KWin re-reads the [Effect-trail] settings when kwinrc changes; the
[Plugins] trailEnabled switch is only read at session start (docs/USAGE.md).


Files
-----

    src/trailsample.{h,cpp}   sample ring buffer, cursor geometry, sampling window
    src/traileffect.{h,cpp}   Effect + InputEventSpy, damage tracking, GL drawing
    src/main.cpp              plugin factory
    src/metadata.json         embedded plugin metadata
    docs/USAGE.md             discovery, configuration, troubleshooting
    docs/DESIGN.md            design and API research


Notes
-----

The plugin id is the file name, trail.so, not a field in metadata.json.

Screenshots and screencasts will not show the trail. TRAIL_KWIN_SELFCHECK=1 makes the
effect read back its own pixels instead and log how many it changed.

Shake Cursor magnifies the pointer on fast back-and-forth motion, which is exactly the
motion that reveals a trail:

    kwriteconfig6 --file kwinrc --group Plugins --key shakecursorEnabled false

Never run kwin_wayland --replace or restart the display manager in your session; that is
how the desktop goes black and windows are lost. To try a freshly built plugin safely,
start a nested KWin instead (docs/USAGE.md, "Nested session").


License
-------

GPL-2.0-or-later, inherited from KWin: the effect includes GPL-2.0-or-later KWin
headers and links libkwin. See LICENSE; full text in LICENSES/GPL-2.0-or-later.txt.
