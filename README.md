# Intra-frame Trail

Intra-frame Trail is a KWin effect that redraws the cursor at every position sampled during the previous frame. Modern mice report far more positions per frame than the compositor draws, so a moving pointer leaves a trail of the intermediate positions that would otherwise be skipped; drawing them improves the pointer's visual smoothness and click accuracy. Written in C++ and OpenGL against KWin's effect API (requires an OpenGL-capable compositor).

Not just for extreme 8000 Hz mice: even with a standard 250 Hz office mouse on a 60 Hz display, it quadruples the perceived cursor density, turning choppy cursor jumping into smooth, readable motion.

The following is a demonstration video; the right side shows the view with this KWin effect enabled:

https://github.com/user-attachments/assets/b65f0f27-ce55-4336-ac58-59105d327ec2

## Principle

The human visual system has a relatively low temporal resolution for tracking fast-moving objects; instead, spatial information strongly dominates our perception of motion. When a cursor moves rapidly across a screen, it normally renders as a series of disconnected, discrete jumps.

Therefore, even on modern displays with high refresh rates such as 120 Hz or above, embedding more spatial information about the cursor's trajectory within a single frame provides crucial visual context. By drawing the intermediate positions that the mouse hardware reports between frames, this effect bridges the visual gaps. This dramatically enhances the brain's perception of smooth, continuous motion and improves your subconscious ability to track and predict the cursor's exact location.

## Requirements

- **Plasma 6 / KWin 6.8.x**
- **OpenGL-capable compositor** (the effect renders with OpenGL)

The source uses the effect API of the 6.8 series, so any 6.8 point release builds it. `CMakeLists.txt` requires `KWin 6.8`, and the exact version pin comes from KWin itself: `config-kwin.h` embeds the full version in the plugin IID (e.g., `org.kde.kwin.EffectPluginFactory6.8.3`), which KWin compares against its own version before instantiating anything. The KWin development headers must therefore match the running KWin **exactly**, and the effect must be rebuilt after every KWin upgrade—for example, a 6.8.2 build will not load on 6.8.3 (see [Pitfalls](#pitfalls)).

## Installation

There is no distribution package yet, so the effect is compiled from source and installed system-wide. KWin loads the plugin from Qt's plugin directories, which on most distributions requires an install prefix of `/usr`.

### Install the build dependencies

The effect itself only uses the KWin effect API, but KWin's CMake package requires its own full development set: Qt 6 (`Core`, `Gui`, `Widgets`, `DBus`, `Quick`), KF6 (`Config`, `CoreAddons`, `WindowSystem`), `libepoxy`, `libdrm`, Vulkan, and `wayland-server`. CMake refuses to configure until every one of them is present, so install the full set even though most of them look unrelated to a cursor effect.

**Ubuntu / Debian (Plasma 6)** — use a release that ships KWin 6.8.x, such as Debian forky/sid or KDE neon (Debian trixie ships 6.3, which this effect does not load on):

```bash
sudo apt install cmake extra-cmake-modules build-essential \
    qt6-base-dev qt6-declarative-dev \
    libkf6config-dev libkf6coreaddons-dev libkf6windowsystem-dev \
    kwin-dev libepoxy-dev libdrm-dev libvulkan-dev libwayland-dev
```

**Arch Linux** — the KWin headers and CMake files are inside the `kwin` package; there is no separate `-devel` package:

```bash
sudo pacman -S --needed base-devel cmake extra-cmake-modules \
    kwin qt6-base qt6-declarative \
    kconfig kcoreaddons kwindowsystem \
    libepoxy libdrm vulkan-headers vulkan-icd-loader wayland
```

**Fedora**:

```bash
sudo dnf install cmake gcc-c++ extra-cmake-modules \
    qt6-qtbase-devel qt6-qtdeclarative-devel \
    kf6-kconfig-devel kf6-kcoreaddons-devel kf6-kwindowsystem-devel \
    kwin-devel libepoxy-devel libdrm-devel \
    vulkan-loader-devel wayland-devel
```

Install the KWin development package from the same repository as the running Plasma so that its version matches the running KWin exactly: a 6.8.2 header set cannot build a plugin for 6.8.3 (see [Pitfalls](#pitfalls)).

### Download the source and build

The install prefix must be `/usr`, the same prefix as the system Qt. KWin searches the Qt plugin directories, which do not include `/usr/local`, so a build configured without a prefix installs to a location KWin never scans and the effect stays invisible in System Settings.

```bash
# 1. Get the source
git clone https://github.com/rainac1/kwin-effect-intra-frame-trail.git
cd kwin-effect-intra-frame-trail

# 2. Configure (the prefix must be /usr)
cmake -B build -S . -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=Release

# 3. Build
cmake --build build -j"$(nproc)"

# 4. Install into the system (requires root)
sudo cmake --install build
```

Installation places the compiled `.so` in Qt's KWin effect directory, for example:
- Fedora: `/usr/lib64/qt6/plugins/kwin/effects/plugins/trail.so`
- Debian / Ubuntu: `/usr/lib/x86_64-linux-gnu/qt6/plugins/kwin/effects/plugins/trail.so`

The plugin metadata (`src/metadata.json`) is embedded directly into the `.so`; no separate JSON or `.desktop` file is installed.

> **Note:** AUTOMOC cannot see the file name inside the `KWIN_EFFECT_FACTORY` macro, so if you edit `src/metadata.json`, the embedded metadata is not regenerated automatically. Clear the autogen directory before rebuilding:
> ```bash
> rm -rf build/src/trail_autogen
> ```

### Enable the effect

Installing does not enable the effect. Switch it on in System Settings:

1. Open **System Settings**.
2. Go to **Window Management → Desktop Effects**.
3. Find **Intra-frame Trail** in the list and tick its checkbox.
4. Click **Apply**.

Clicking **Apply** loads the effect into the running compositor via D-Bus (`loadEffect`), so no logout is needed for a first-time install. The same can be done from the command line:

```bash
kwriteconfig6 --file kwinrc --group Plugins --key trailEnabled true
gdbus call --session --dest org.kde.KWin --object-path /Effects \
    --method org.kde.kwin.Effects.loadEffect trail
```

`qdbus org.kde.KWin /KWin reconfigure` is not part of this and does not help: it re-reads KWin's own settings (options and window rules) but never calls the effect loader, so it neither rescans plugin directories nor loads the effect.

To query what the running compositor sees:

```bash
gdbus call --session --dest org.kde.KWin --object-path /Effects \
    --method org.kde.kwin.Effects.isEffectSupported trail
gdbus call --session --dest org.kde.KWin --object-path /Effects \
    --method org.kde.kwin.Effects.isEffectLoaded trail
```

## Apply a rebuild

Installing a new build does not change what a running KWin executes. Qt's plugin loader caches a plugin by path, so `unloadEffect` + `loadEffect` re-creates the effect from the same mapping instead of reading the file again. A rebuilt `.so` therefore only runs in a new compositor process: **log out and back in** after `sudo cmake --install build`. `qdbus org.kde.KWin /KWin reconfigure` does not reset that cache and does not reload the plugin.

## Pitfalls

1. **Plasma 5 and Plasma 6 are incompatible.**
   Plasma 6 moved KWin to Qt 6 and rebuilt its internals, so a C++ effect written for Plasma 5 does not load on Plasma 6, and vice versa. Build the branch that matches your Plasma major version.

2. **KWin has no stable C++ plugin ABI.**
   Even within Plasma 6, a point release can change an internal API and break an existing effect or crash KWin. On top of the ABI, this effect carries a hard version guard: the factory IID embeds KWin's full version (e.g., `org.kde.kwin.EffectPluginFactory6.8.3`), and KWin refuses to instantiate a plugin built against a different one. After a KWin update, the symptom is usually just "the effect disappeared" because the mismatch is logged at debug level. Rebuild against the matching KWin development package (`kwin-devel` on Fedora, `kwin-dev` on Debian/Ubuntu, `kwin` on Arch).

3. **The install prefix trap.**
   A build configured without `-DCMAKE_INSTALL_PREFIX=/usr` defaults to `/usr/local`, and KWin's plugin search covers the system Qt plugin directories under `/usr`, not `/usr/local`. `sudo cmake --install build` succeeds, but the effect never appears in Desktop Effects. Always pass `-DCMAKE_INSTALL_PREFIX=/usr`.

4. **The compositor keeps the `.so` it started with.**
   Rebuilding and installing updates the file on disk, not what the running KWin executes. Log out and back in (see [Apply a rebuild](#apply-a-rebuild)). `qdbus org.kde.KWin /KWin reconfigure` does not help—it never reaches the effect loader.

## Configuration

`~/.config/kwinrc`:

```ini
[Plugins]
trailEnabled=true

[Effect-trail]
Enabled=true
MaxSamples=256
SampleStride=1
```

The trail is always exactly one frame long: the effect draws every sample the pointer produced since the previous frame.
- `MaxSamples` caps how many cursors are drawn per frame.
- `SampleStride` keeps one out of every `N` samples and drops the rest before they are buffered, so the drawn cursors end up `N` times further apart while the trail still covers that same frame (`1` keeps every sample).

KWin re-reads the `[Effect-trail]` settings when `kwinrc` changes; the `[Plugins] trailEnabled` switch is only read at session start (see [docs/USAGE.md](docs/USAGE.md)).

## Files

| Path | Purpose |
| :--- | :--- |
| `src/trailsample.{h,cpp}` | Sample ring buffer, cursor geometry, sampling window |
| `src/traileffect.{h,cpp}` | `Effect` + `InputEventSpy`, damage tracking, OpenGL drawing |
| `src/main.cpp` | Plugin factory |
| `src/metadata.json` | Embedded plugin metadata |
| [docs/USAGE.md](docs/USAGE.md) | Discovery, configuration, troubleshooting |
| [docs/DESIGN.md](docs/DESIGN.md) | Design and API research |

## Notes

- **Plugin ID:** The plugin ID is the file name (`trail.so`), not a field in `metadata.json`.
- **Color management:** The trail is converted from sRGB into the output's color description, so it follows display brightness, Night Light, and HDR settings the same way the composited pointer does, in both SDR and HDR modes.
- **Erasing frame:** The effect keeps one repaint scheduled while a trail is on screen. Because the trail is drawn on top of the scene, it only disappears when a later frame repaints the region it was drawn into; while the pointer moves, motion events schedule those frames anyway, but once it stops nothing else produces damage and the last copy would stay behind as a bright edge around the pointer.
- **Screenshots and screencasts:** Captures will not show the trail. Set `TRAIL_KWIN_SELFCHECK=1` to make the effect read back its own pixels and log how many it changed.
- **Multi-monitor & mixed scaling:** Every output gets the trail, including on multi-monitor and mixed-scaling setups—each output has its own sample buffer and damage, so one monitor being painted first does not consume the samples of the others.
- **Shake Cursor:** Shake Cursor magnifies the pointer on fast back-and-forth motion, which is the exact motion that reveals a trail. To disable it:
  ```bash
  kwriteconfig6 --file kwinrc --group Plugins --key shakecursorEnabled false
  ```
- **Adaptive Sync (VRR):** Set Adaptive Sync to **Automatic** or **Off** rather than **Always**. The trail is not drawn while a frame is presented in adaptive sync mode: on **Always**, that is every frame; on **Automatic**, it is only the windows that request it, so the trail remains active everywhere else. VRR drops the display's refresh rate to the content's rate (e.g., a 60 fps video on a 120 Hz display drops it to 60 Hz). Drawing the trail at that lowered rate makes the stepping obvious, so the effect stands down until the display returns to its native rate. (The pointer stutters in that situation with the effect disabled as well, so the effect is not the cause.)
- **Never run `kwin_wayland --replace`** or restart the display manager in your live session; that is how the desktop goes black and windows are lost. To test a freshly built plugin safely, start a nested KWin instead (see [docs/USAGE.md](docs/USAGE.md#nested-session)).

## Author

`rainac1` `<rainandcap@gmail.com>` — design, implementation, and packaging.

The KWin effect API and the headers this plugin builds against are the work of the KDE community and are used here, not claimed. Third-party reference checkouts (KWin sources and headers, kept out of the repository) retain their own copyright notices.

## License

Copyright (C) 2026 rainac1 `<rainandcap@gmail.com>`

`GPL-3.0-only`. The effect includes KWin headers and links `libkwin`; both are `GPL-2.0-or-later`, whose "or later" clause allows the combination to be used under `GPL-3.0`. Qt 6 is available under `GPL-3.0` as well, so the whole work is licensed as `GPL-3.0-only`. The full text is in [LICENSE](LICENSE). The grant is version 3 only, not "or any later version": the "or later" wording inside `LICENSE` belongs to the FSF's sample notice for applying the license, not to this project.
