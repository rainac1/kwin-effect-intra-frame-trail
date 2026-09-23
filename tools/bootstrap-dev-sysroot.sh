#!/usr/bin/env bash
#
# bootstrap-dev-sysroot.sh -- build a local, root-free development sysroot for
# KWin effect development on Fedora/Plasma 6.
#
# Why this exists: KWin effect plugins need the KWin development headers
# (kwin-devel), Qt 6 development files (headers, CMake configs, moc/rcc/uic)
# and a few KF6 development packages. On a machine where `dnf install` is not
# available (no root, read-only system, sandboxed CI, container, ...) we can
# still get a fully working toolchain by downloading the matching RPMs and
# unpacking them into a private prefix. CMake is then pointed at that prefix
# via CMAKE_PREFIX_PATH.
#
# Nothing here touches the system: every file lands under <repo>/.sysroot.
#
# Usage:
#   tools/bootstrap-dev-sysroot.sh [-f]
#
#   -f   force: re-download and re-extract everything
#
# Environment:
#   FEDORA_MIRROR  mirror base URL (default: TUNA)
#   FEDORA_RELEASE releasever (default: 44)
#
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
CACHE_DIR="$ROOT_DIR/.tools/rpm"
SYSROOT="$ROOT_DIR/.sysroot"
MIRROR="${FEDORA_MIRROR:-https://mirrors.tuna.tsinghua.edu.cn/fedora}"
RELEASEVER="${FEDORA_RELEASE:-44}"
FORCE=0
[[ "${1:-}" == "-f" || "${1:-}" == "--force" ]] && FORCE=1

# Repositories to search, in priority order. The newest package wins, so
# updates/ must be searched before the release/ GA repo.
REPO_PATHS=(
    "updates/$RELEASEVER/Everything/x86_64"
    "releases/$RELEASEVER/Everything/x86_64/os"
)

# Packages we need. Names only; the newest matching build in the repos above is
# picked, so this keeps working when Fedora bumps versions.
PACKAGES=(
    extra-cmake-modules
    qt6-qtbase-devel
    # Qt6CoreToolsTargets.cmake references every binary of the Qt installation,
    # including ones shipped by the runtime package (e.g. qtpaths), and CMake
    # refuses to configure if an imported target points at a missing file.
    qt6-qtbase
    # find_package(KWin) hard-requires Qt6Quick, the Wayland server libraries,
    # libepoxy, libdrm and Vulkan even though a simple effect never touches them.
    qt6-qtdeclarative-devel
    wayland-devel
    libepoxy-devel
    libdrm-devel
    vulkan-loader-devel
    vulkan-headers
    kf6-kcoreaddons-devel
    kf6-kconfig-devel
    kf6-ki18n-devel
    kf6-kwindowsystem-devel
    kwin-devel
)

# NOTE: all logging goes to stderr. stdout is reserved for function return
# values (download_rpm prints the RPM path).
log()  { printf '\033[1;34m==>\033[0m %s\n' "$*" >&2; }
warn() { printf '\033[1;33mwarning:\033[0m %s\n' "$*" >&2; }

need() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "error: required tool '$1' is missing" >&2
        exit 1
    }
}
need curl
need rpm2cpio
need cpio
need python3

mkdir -p "$CACHE_DIR" "$SYSROOT"

# Pick the newest version-release.arch for a package name from one repo path.
# Prints "<version>-<release>.<arch>" or nothing.
resolve_version() {
    local name=$1 repo=$2
    local letter=${name:0:1}
    local listing
    listing=$(curl -fsS --retry 3 --max-time 60 "$MIRROR/$repo/Packages/$letter/" 2>/dev/null) || return 0
    printf '%s\n' "$listing" \
        | grep -oE "href=\"${name}-[0-9][^\"]*\.(x86_64|noarch)\.rpm\"" \
        | sed -e 's/^href="//' -e 's/"$//' -e "s/^${name}-//" -e 's/\.rpm$//' \
        | sort -V \
        | tail -1
}

download_rpm() {
    local name=$1
    local repo ver url dest
    local found=""
    for repo in "${REPO_PATHS[@]}"; do
        ver=$(resolve_version "$name" "$repo")
        if [[ -n "$ver" ]]; then
            found="$repo|$ver"
            break
        fi
    done
    if [[ -z "$found" ]]; then
        warn "could not find package '$name' in any known Fedora repo"
        return 1
    fi
    repo=${found%%|*}
    ver=${found##*|}
    local letter=${name:0:1}
    local rpm="$name-$ver.rpm"
    dest="$CACHE_DIR/$rpm"
    url="$MIRROR/$repo/Packages/$letter/$rpm"
    if [[ -f "$dest" && $FORCE -eq 0 ]]; then
        log "cached  $rpm"
    else
        log "fetch   $rpm"
        curl -fS --no-progress-meter --retry 3 --retry-delay 2 -o "$dest.part" "$url"
        mv "$dest.part" "$dest"
    fi
    if [[ ! -f "$dest" ]]; then
        warn "download of $rpm failed"
        return 1
    fi
    printf '%s\n' "$dest"
}

# Unpack an RPM into the sysroot. cpio extraction is idempotent: files that are
# already present at the same or newer version are skipped, so a retry after a
# transient pipe/write hiccup simply completes whatever is still missing.
extract_rpm() {
    local rpm=$1 attempt
    for attempt in 1 2 3 4; do
        if ( cd "$SYSROOT" && rpm2cpio "$rpm" | cpio -idm --quiet --no-absolute-filenames ); then
            return 0
        fi
        log "retrying extraction of $(basename "$rpm") (attempt $attempt)"
        sleep 1
    done
    warn "extraction of $(basename "$rpm") reported errors"
    return 1
}

log "sysroot   : $SYSROOT"
log "mirror    : $MIRROR (Fedora $RELEASEVER)"

for pkg in "${PACKAGES[@]}"; do
    if ! rpm_path=$(download_rpm "$pkg"); then
        echo "error: failed to obtain $pkg" >&2
        exit 1
    fi
    stamp="$SYSROOT/.stamp-$pkg"
    if [[ -f "$stamp" && $FORCE -eq 0 ]]; then
        log "extract $pkg (already unpacked)"
    else
        log "extract $pkg"
        # A failure here is not fatal on its own: the final verification step
        # below checks for the files we actually need.
        extract_rpm "$rpm_path" || true
        : > "$stamp"
    fi
done

# Verify that everything the build needs actually landed in the sysroot.
log "verifying sysroot"
missing=0
for marker in \
    "usr/include/kwin/effect/effect.h" \
    "usr/include/kwin/effect/effecthandler.h" \
    "usr/lib64/cmake/KWin/KWinConfig.cmake" \
    "usr/lib64/cmake/Qt6/Qt6Config.cmake" \
    "usr/lib64/cmake/Qt6Core/Qt6CoreConfig.cmake" \
    "usr/share/ECM/cmake/ECMConfig.cmake" \
    "usr/lib64/cmake/KF6CoreAddons/KF6CoreAddonsConfig.cmake" \
    "usr/lib64/cmake/KF6Config/KF6ConfigConfig.cmake"
do
    if [[ ! -e "$SYSROOT/$marker" ]]; then
        warn "missing: $marker"
        missing=1
    fi
done
if [[ $missing -ne 0 ]]; then
    echo "error: sysroot is incomplete; re-run with -f" >&2
    exit 1
fi

# RPMs ship versioned runtime libraries in their runtime packages, which we do
# not unpack (the system already provides them). The devel packages only ship
# the `libfoo.so` linker symlink, which now dangles. Re-point every dangling
# symlink at the system copy so the linker can resolve it while the running
# compositor still uses the system libraries at runtime -- guaranteeing ABI
# compatibility with the host KWin.
log "relinking dangling sysroot symlinks to /usr/lib64"
while IFS= read -r -d '' link; do
    if [[ ! -e "$link" ]]; then
        target=$(basename "$(readlink "$link")")
        for candidate in "/usr/lib64/$target" "/usr/lib/$target"; do
            if [[ -e "$candidate" ]]; then
                ln -sfn "$candidate" "$link"
                break
            fi
        done
    fi
done < <(find "$SYSROOT" -type l -print0)

# Installed CMake package configs also point at the *versioned* runtime library
# (e.g. .../libQt6Gui.so.6.11.2), which only the runtime packages ship and which
# we deliberately do not unpack. Provide those from the system too, so that the
# linker uses the exact libraries the running compositor uses.
log "providing missing versioned libraries from /usr/lib64"
while IFS= read -r lib; do
    [[ -e "$SYSROOT/usr/lib64/$lib" ]] && continue
    for candidate in "/usr/lib64/$lib" "/usr/lib/$lib"; do
        if [[ -e "$candidate" ]]; then
            ln -sfn "$candidate" "$SYSROOT/usr/lib64/$lib"
            break
        fi
    done
done < <(grep -rhoE 'lib[A-Za-z0-9_+.-]+\.so(\.[0-9]+)*' \
             "$SYSROOT/usr/lib64/cmake" "$SYSROOT/usr/share/ECM" 2>/dev/null | sort -u)

# Qt's CMake packages enumerate every plugin and tool of the installation and
# verify that the files exist, e.g. .../lib64/qt6/plugins/.../libdmabuf-server.so.
# Those belong to runtime packages we do not unpack, so mirror any referenced
# path that is missing in the sysroot from the system installation.
log "mirroring other missing runtime files from the system"
while IFS= read -r rel; do
    target="$SYSROOT/usr/$rel"
    [[ -e "$target" ]] && continue
    if [[ -e "/usr/$rel" ]]; then
        mkdir -p "$(dirname "$target")"
        ln -sfn "/usr/$rel" "$target"
    fi
done < <(grep -rhoE '\$\{_IMPORT_PREFIX\}/(lib64|lib|share|bin)/[A-Za-z0-9_+./-]+' \
             "$SYSROOT/usr/lib64/cmake" 2>/dev/null \
         | sed -e 's|\${_IMPORT_PREFIX}/||' | sort -u)

cat <<EOF

Done. Development sysroot ready at:
  $SYSROOT

Configure the build with it, e.g.:
  cmake -B build -S . -G Ninja \\
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \\
      -DCMAKE_PREFIX_PATH="$SYSROOT/usr" \\
      -DCMAKE_INSTALL_PREFIX="$ROOT_DIR/.local"

(helpers/build.sh does this for you.)
EOF
