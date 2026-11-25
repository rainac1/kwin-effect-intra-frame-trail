# Usage

Discovery, enabling, and troubleshooting. For building and installing, see [README.md](../README.md); for design and API research, see [DESIGN.md](DESIGN.md).

## How KWin finds the plugin

KWin discovers binary effects with `KPluginMetaData::findPlugins("kwin/effects/plugins")`, which searches Qt's plugin paths. Qt's defaults are the system directories, so an install under `/usr` is found without any environment variable:

- **Fedora:** `/usr/lib64/qt6/plugins/kwin/effects/plugins/trail.so`
- **Debian / Ubuntu:** `/usr/lib/x86_64-linux-gnu/qt6/plugins/kwin/effects/plugins/trail.so`

This is why the README configures with `-DCMAKE_INSTALL_PREFIX=/usr`. Qt does not search `/usr/local`, so a plugin installed there (the default of a build configured without a prefix) stays invisible even though `sudo cmake --install build` reports success.

`~/.local/share/kwin/effects/` is for scripted/QML effects (`KPackage`). Binary plugins do not go there.

The plugin ID is its file name (`trail.so`), which is why the target is built without the `lib` prefix and why the `kwinrc` switch is `[Plugins] trailEnabled`.

## Version guard

The factory IID carries KWin's full version (`config-kwin.h`):

```text
org.kde.kwin.EffectPluginFactory6.8.3
```

`PluginEffectLoader::factory()` compares it against the plugin's embedded metadata before instantiating anything. On a mismatch, it logs one `qCDebug(KWIN_CORE)` line and returns `nullptr`:

```text
"trail" has not matching plugin version, expected ...6.8.3 got ...6.8.2
Couldn't get an EffectPluginFactory for: "trail"
```

No crash occurs, other effects load, and the compositor keeps running. Discovery is metadata-only and version-independent, so the effect is still listed in Desktop Effects, but `isEffectSupported` returns `false` (listed, greyed out). Because the log is debug-level, the symptom after a KWin upgrade is usually just "the effect disappeared". Rebuild against the matching `kwin-devel` / `kwin-dev`.

If the version matches but the ABI does not, the guard passes and the plugin is instantiated inside the compositor process, unsandboxed. Test in a [nested session](#nested-session) first.

## Enable, disable, query

`kwinrc` persists the choice for the next session; D-Bus calls change the running one:

```bash
# Enable
kwriteconfig6 --file kwinrc --group Plugins --key trailEnabled true
gdbus call --session --dest org.kde.KWin --object-path /Effects \
    --method org.kde.kwin.Effects.loadEffect trail

# Disable
kwriteconfig6 --file kwinrc --group Plugins --key trailEnabled false
gdbus call --session --dest org.kde.KWin --object-path /Effects \
    --method org.kde.kwin.Effects.unloadEffect trail

# Re-create the effect without touching kwinrc (same build)
gdbus call --session --dest org.kde.KWin --object-path /Effects \
    --method org.kde.kwin.Effects.unloadEffect trail
gdbus call --session --dest org.kde.KWin --object-path /Effects \
    --method org.kde.kwin.Effects.loadEffect trail

# Query
gdbus call --session --dest org.kde.KWin --object-path /Effects \
    --method org.kde.kwin.Effects.isEffectSupported trail
gdbus call --session --dest org.kde.KWin --object-path /Effects \
    --method org.kde.kwin.Effects.isEffectLoaded trail
```

The same switch is the **Intra-frame Trail** checkbox under **System Settings → Window Management → Desktop Effects**, whose **Apply** button makes the `loadEffect` / `unloadEffect` call for you.

## Apply a rebuild

A running KWin keeps the `.so` it loaded for the whole session. Qt's plugin loader caches a plugin by path, so `unloadEffect` + `loadEffect` re-creates the effect from the same mapping rather than reading the file again; rebuilding and installing changes the file on disk, not what the compositor runs.

- **Log out and back in** — the only way to run a rebuilt plugin.

`reconfigureEffect` is not a reload either: it calls `Effect::reconfigure()` on the already loaded instance, for `[Effect-trail]` settings only. Metadata is the exception: `PluginEffectLoader::findEffect()` asks for the plugin metadata on every call instead of caching it, so a build whose embedded metadata is invalid reports `isEffectSupported` as `false`, and the next good install flips it back to `true` with no restart. That affects discovery only; the running effect still executes the build it started with.

`qdbus org.kde.KWin /KWin reconfigure` does not reset the plugin cache either. It reaches `Workspace::slotReconfigure()`, which reparses the config, updates `Options`, and reloads window rules; it never calls the effect loader. The only caller of `EffectLoader::queryAndLoadAll()` is `EffectsHandler::reconfigure()`, which runs once from the `EffectsHandler` constructor at session start. Even that would not re-read a loaded `.so`: `PluginEffectLoader::loadEffect()` returns early while the name is in `m_loadedEffects`, and `PluginEffectLoader::clear()` is an empty function.

`[Plugins] trailEnabled` persists the choice for the next session. A plain `kwriteconfig6` edit does not load or unload a running KWin (measured: no change after six seconds either way). `KConfig` only emits the `org.kde.kconfig.notify` D-Bus signal for entries written with the `KConfig::Notify` flag, and `EffectsHandler::configChanged` (the live load/unload hook, connected to `KConfigWatcher`) is driven by that signal. `kwriteconfig6` sets the flag only with `--notify`, so either pass it:

```bash
kwriteconfig6 --file kwinrc --group Plugins --key trailEnabled true --notify
```

or write `kwinrc` for the next login and use `loadEffect` / `unloadEffect` for the running session, which is also what the Desktop Effects **Apply** button does.

That live call requires the running KWin to have the plugin on Qt's plugin path. A system-wide install under `/usr` is already there. A running process cannot gain a new plugin path at runtime, so a plugin installed under `/usr/local` only helps after it is reinstalled into `/usr` (or tested in a [nested session](#nested-session)).

## Configuration

`~/.config/kwinrc`, group `[Effect-trail]`:

| Key | Default | Description |
| :--- | :--- | :--- |
| `Enabled` | `true` | Runtime switch; does not unload the plugin |
| `MaxSamples` | `256` | Per-frame draw budget |
| `SampleStride` | `1` | Keep one out of every `N` samples |
| `BlockDirectScanout` | `false` | Force compositing over fullscreen direct-scanout windows (also disables hardware cursor plane while active) |
| `Fade` | `true` | Enable width-of-zero hierarchical opacity (`L = m - ctz(k)`) |
| `FadeMode` | `geometric` | Attenuation curve per level: `geometric` (`pow(FadeBase, -L)`) or `beer-lambert` (`1 - exp(-FadeOpticalDepth * 2^-L)`) |
| `FadeBase` | `1.618` | Base `b` (`>= 1.0`) for `geometric` mode (`1.0` is full opacity) |
| `FadeOpticalDepth` | `2.0` | Reference optical thickness `τ0` (`>= 0.0`) for `beer-lambert` mode (`τ(L) = τ0 * 2^-L`) |
| `FadeGammaCompensation` | `true` | Apply sRGB inverse-gamma compensation `pow(p, 1 / FadeGamma)` to final opacity |
| `FadeGamma` | `2.2` | Gamma exponent (`0.1`–`10.0`) for `FadeGammaCompensation` |

```bash
kwriteconfig6 --file kwinrc --group Effect-trail --key MaxSamples 128
kwriteconfig6 --file kwinrc --group Effect-trail --key SampleStride 4
kwriteconfig6 --file kwinrc --group Effect-trail --key FadeBase 1.5
```

If a change does not take effect immediately:

```bash
gdbus call --session --dest org.kde.KWin --object-path /Effects \
    --method org.kde.kwin.Effects.reconfigureEffect trail
```

The trail is always exactly one frame long: the effect draws every pointer sample produced since the previous frame, so flick the pointer to see it.

`SampleStride` thins that trail out: it keeps one sample out of every `N` and drops the others as they arrive, so the drawn cursors are `N` times further apart while the trail still covers the same frame. Because the newest samples close to the real pointer can be dropped with them, raise it only as far as needed when the trail is too dense to read. Values below `1` fall back to `1`, which keeps every sample and is the default.

## Uninstall

```bash
kwriteconfig6 --file kwinrc --group Plugins --key trailEnabled false
sudo rm /usr/lib64/qt6/plugins/kwin/effects/plugins/trail.so
```

Adjust the path for your distribution (see [How KWin finds the plugin](#how-kwin-finds-the-plugin)), and log out and back in so the removed plugin is not loaded from a stale session. The effect's settings remain in `~/.config/kwinrc` under `[Effect-trail]`; remove that group with `kwriteconfig6` if you want them gone.

## Nested session

An isolated KWin with its own D-Bus, config, cache, and socket—drawn into a window on the current desktop—is the safe way to test a build whose ABI may not match: a bad plugin can crash the compositor process, and this keeps that out of your real session.

```bash
RUNTIME_DIR=${XDG_RUNTIME_DIR:-/tmp}/trail-nested
mkdir -p "$RUNTIME_DIR/config" "$RUNTIME_DIR/cache"
cat > "$RUNTIME_DIR/config/kwinrc" <<'EOF'
[Plugins]
trailEnabled=true
shakecursorEnabled=false

[Effect-trail]
Enabled=true
MaxSamples=256
EOF
XDG_CONFIG_HOME="$RUNTIME_DIR/config" \
XDG_CACHE_HOME="$RUNTIME_DIR/cache" \
QT_LOGGING_RULES="kwin_effect_trail.debug=true" \
    dbus-run-session kwin_wayland \
        --socket wayland-dev --width 1280 --height 720 konsole

WAYLAND_DISPLAY=wayland-dev dolphin          # attach another client
```

Never run `kwin_wayland --replace` or restart the display manager in your host session; that is how the desktop goes black and windows are lost. The nested instance picks up the system-wide plugin directly, without touching the real session and without a re-login.

## Logs

```bash
journalctl --user -u plasma-kwin_wayland -f | grep kwin_effect_trail
QT_LOGGING_RULES="kwin_effect_trail.debug=true" dbus-run-session kwin_wayland ...
```

## Problems

- **Effect missing from Desktop Effects, `isEffectSupported` is `false`:**
  The plugin is not on Qt's plugin path. Reinstall with `-DCMAKE_INSTALL_PREFIX=/usr`; Qt does not search `/usr/local`.
- **`isEffectSupported` is `true`, but nothing is drawn:**
  `[Effect-trail] Enabled` is `false`, or the session is not compositing with OpenGL.
- **Listed but greyed out, log says `"has not matching plugin version"`:**
  Built against a different KWin version. Rebuild against the matching `kwin-devel` / `kwin-dev`.
- **Rebuilt, but the running compositor still behaves the old way:**
  It keeps the `.so` it loaded for the whole session. Log out and back in (see [Apply a rebuild](#apply-a-rebuild)).
- **No `kwin_effect_trail` output at all:**
  The plugin is not loaded; see the first two entries above.
- **Trail does not show up in a screenshot or screencast:**
  Expected. Set `TRAIL_KWIN_SELFCHECK=1` to verify the pixels drawn on screen.
- **Pointer gets bigger while shaking it:**
  That is KWin's Shake Cursor effect. Disable it with:
  ```bash
  kwriteconfig6 --file kwinrc --group Plugins --key shakecursorEnabled false
  ```

## Terms

| Term | Meaning |
| :--- | :--- |
| `effect chain` | Effects run in sequence each frame; this one draws above windows |
| `damage` | The region that has to be repainted; less damage, less power |
| `output` / `scale` | A display; `logical coordinates * scale = device pixels` |
| `KPluginMetaData` | KDE plugin discovery metadata, embedded in `trail.so` |
| `InputEventSpy` | Internal interface for observing input events without consuming them |
