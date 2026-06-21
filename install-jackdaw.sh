#!/usr/bin/env bash
#
# install-jackdaw.sh — distro-agnostic, systemd-free installer for JackDAW.
#
# Installs runtime dependencies, then installs JackDAW. When run interactively
# it asks how to install — the PREBUILT binary is the default — and falls back
# to building from source if the prebuilt binary is not usable on this machine.
# Places the binary, the optional LV2 UI helpers, the icon set and a launcher.
#
# Usage:
#   ./install-jackdaw.sh [options]
#
# Options:
#   --prebuilt     Use the prebuilt binary, no prompt; fail if it is unusable.
#                  (Prebuilt is also the default choice in the prompt.)
#   --build        Force building from source (ignore any prebuilt binary).
#   --no-deps      Do not install any distro packages.
#   -h, --help     Show this help.
#
# Environment:
#   PREFIX=/path   Install prefix (skips the interactive prefix prompt).
#                  Common values: /usr/local (system) or "$HOME/.local" (user).
#
set -euo pipefail

# --------------------------------------------------------------------------- #
# Messaging
# --------------------------------------------------------------------------- #
if [ -t 2 ]; then
    C_RED=$'\033[31m'; C_YEL=$'\033[33m'; C_GRN=$'\033[32m'; C_OFF=$'\033[0m'
else
    C_RED=''; C_YEL=''; C_GRN=''; C_OFF=''
fi
log()  { printf '%s==>%s %s\n'  "$C_GRN" "$C_OFF" "$*" >&2; }
warn() { printf '%swarning:%s %s\n' "$C_YEL" "$C_OFF" "$*" >&2; }
die()  { printf '%serror:%s %s\n'   "$C_RED" "$C_OFF" "$*" >&2; exit 1; }

usage() { sed -n '2,/^set -euo/{/^set -euo/d;s/^# \{0,1\}//;p}' "$0"; exit "${1:-0}"; }

# --------------------------------------------------------------------------- #
# Locate the source tree (the directory this script lives in)
# --------------------------------------------------------------------------- #
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
cd "$SCRIPT_DIR"

# --------------------------------------------------------------------------- #
# Arguments
# --------------------------------------------------------------------------- #
FORCE_BUILD=0
FORCE_PREBUILT=0
NO_DEPS=0
for arg in "$@"; do
    case "$arg" in
        --build)    FORCE_BUILD=1 ;;
        --prebuilt) FORCE_PREBUILT=1 ;;
        --no-deps)  NO_DEPS=1 ;;
        -h|--help)  usage 0 ;;
        *)          die "unknown argument: $arg (try --help)" ;;
    esac
done
[ "$FORCE_BUILD" -eq 1 ] && [ "$FORCE_PREBUILT" -eq 1 ] && \
    die "--build and --prebuilt are mutually exclusive"

# --------------------------------------------------------------------------- #
# Install prefix (interactive unless PREFIX is set in the environment)
# --------------------------------------------------------------------------- #
if [ -n "${PREFIX:-}" ]; then
    log "Using PREFIX from environment: $PREFIX"
else
    if [ ! -t 0 ]; then
        die "no terminal for the prefix prompt; set PREFIX= explicitly"
    fi
    printf '%s\n' "Where should JackDAW be installed?" >&2
    printf '  %s\n' "[1] system-wide  (/usr/local) — needs root/sudo" >&2
    printf '  %s\n' "[2] this user    ($HOME/.local)" >&2
    printf '%s' "Choice [1]: " >&2
    read -r _choice || _choice=1
    case "${_choice:-1}" in
        2) PREFIX="$HOME/.local" ;;
        ""|1) PREFIX="/usr/local" ;;
        *) die "invalid choice: $_choice" ;;
    esac
fi

BINDIR="$PREFIX/bin"
DATADIR="$PREFIX/share"
APPDIR="$DATADIR/applications"
ICONROOT="$DATADIR/icons/hicolor"
PIXMAPS="$DATADIR/pixmaps"

# --------------------------------------------------------------------------- #
# Privilege escalation: route every write to PREFIX through run_priv()
# --------------------------------------------------------------------------- #
NEED_SUDO=0
if [ "$(id -u)" -ne 0 ]; then
    # We need elevation only when the prefix root is not writable by us.
    _probe="$PREFIX"
    while [ -n "$_probe" ] && [ ! -e "$_probe" ]; do _probe=$(dirname "$_probe"); done
    if [ ! -w "${_probe:-/}" ]; then
        if command -v sudo >/dev/null 2>&1; then
            NEED_SUDO=1
        else
            die "no write access to $PREFIX and sudo not found; re-run as root or use PREFIX=\$HOME/.local"
        fi
    fi
fi
run_priv() { if [ "$NEED_SUDO" -eq 1 ]; then sudo "$@"; else "$@"; fi; }

# --------------------------------------------------------------------------- #
# Package manager detection + per-distro package-name map
#   NOTE: package names are best-effort and may need refinement per distro/
#   release. A failed package install is only a warning — the real gate on a
#   working install is the ldd check (prebuilt) or make (source).
# --------------------------------------------------------------------------- #
PM=unknown
detect_pm() {
    if   command -v apt-get >/dev/null 2>&1; then PM=apt
    elif command -v dnf     >/dev/null 2>&1; then PM=dnf
    elif command -v yum     >/dev/null 2>&1; then PM=yum
    elif command -v pacman  >/dev/null 2>&1; then PM=pacman
    elif command -v zypper  >/dev/null 2>&1; then PM=zypper
    elif command -v apk     >/dev/null 2>&1; then PM=apk
    else PM=unknown
    fi
}

# Echo the package list for the given mode (runtime|build) on the detected PM.
pkg_list() {
    local mode="$1"
    case "$PM" in
        apt)
            if [ "$mode" = build ]; then
                echo "libgtk-3-dev libjack-jackd2-dev libsndfile1-dev libasound2-dev libsamplerate0-dev liblilv-dev libsuil-dev build-essential pkg-config"
            else
                echo "libgtk-3-0 libjack-jackd2-0 libsndfile1 libasound2 libsamplerate0 liblilv-0-0 libsuil-0-0"
            fi ;;
        dnf|yum)
            if [ "$mode" = build ]; then
                echo "gtk3-devel jack-audio-connection-kit-devel libsndfile-devel alsa-lib-devel libsamplerate-devel lilv-devel suil-devel gcc gcc-c++ make pkgconf-pkg-config"
            else
                echo "gtk3 jack-audio-connection-kit libsndfile alsa-lib libsamplerate lilv suil"
            fi ;;
        pacman)
            # Arch dev headers ship inside the main packages; base-devel for toolchain.
            if [ "$mode" = build ]; then
                echo "gtk3 jack2 libsndfile alsa-lib libsamplerate lilv suil base-devel pkgconf"
            else
                echo "gtk3 jack2 libsndfile alsa-lib libsamplerate lilv suil"
            fi ;;
        zypper)
            if [ "$mode" = build ]; then
                echo "gtk3-devel libjack-devel libsndfile-devel alsa-devel libsamplerate-devel lilv-devel suil-devel gcc gcc-c++ make pkg-config"
            else
                echo "libgtk-3-0 libjack0 libsndfile1 libasound2 libsamplerate0 liblilv-0-0 libsuil-0-0"
            fi ;;
        apk)
            if [ "$mode" = build ]; then
                echo "gtk+3.0-dev jack-dev libsndfile-dev alsa-lib-dev libsamplerate-dev lilv-dev suil-dev build-base pkgconf"
            else
                echo "gtk+3.0 jack libsndfile alsa-lib libsamplerate lilv suil"
            fi ;;
        *) echo "" ;;
    esac
}

pkg_install() {
    local pkgs="$1"
    [ -n "$pkgs" ] || return 0
    log "Installing packages: $pkgs"
    case "$PM" in
        apt)    run_priv apt-get update -qq || warn "apt-get update failed"
                run_priv apt-get install -y $pkgs || warn "some packages failed to install (may already be present)" ;;
        dnf)    run_priv dnf install -y $pkgs    || warn "some packages failed to install (may already be present)" ;;
        yum)    run_priv yum install -y $pkgs    || warn "some packages failed to install (may already be present)" ;;
        pacman) run_priv pacman -S --needed --noconfirm $pkgs || warn "some packages failed to install (may already be present)" ;;
        zypper) run_priv zypper install -y $pkgs || warn "some packages failed to install (may already be present)" ;;
        apk)    run_priv apk add $pkgs           || warn "some packages failed to install (may already be present)" ;;
        *)      warn "unknown package manager; skipping dependency install" ;;
    esac
}

# --------------------------------------------------------------------------- #
# Prebuilt binary usability — checked WITHOUT executing it (it calls gtk_init).
# --------------------------------------------------------------------------- #
PREBUILT=""
find_prebuilt() {
    if   [ -f "src/jackdaw" ]; then PREBUILT="src/jackdaw"
    elif [ -f "jackdaw"      ]; then PREBUILT="jackdaw"
    else PREBUILT=""
    fi
}

prebuilt_usable() {
    local bin="$1"
    [ -n "$bin" ] && [ -x "$bin" ] || return 1
    # Architecture must match this machine.
    if command -v readelf >/dev/null 2>&1; then
        local elf_machine host
        elf_machine=$(readelf -h "$bin" 2>/dev/null | awk -F: '/Machine/{gsub(/^ +/,"",$2);print $2;exit}')
        host=$(uname -m)
        case "$host:$elf_machine" in
            x86_64:*X86-64*|amd64:*X86-64*) : ;;
            aarch64:*AArch64*|arm64:*AArch64*) : ;;
            *) warn "prebuilt arch ($elf_machine) does not match host ($host)"; return 1 ;;
        esac
    fi
    # All NEEDED shared libraries must resolve.
    if command -v ldd >/dev/null 2>&1; then
        if ldd "$bin" 2>/dev/null | grep -q "not found"; then
            return 1
        fi
    fi
    return 0
}

build_from_source() {
    log "Building JackDAW from source"
    command -v make >/dev/null 2>&1 || die "make not found and build deps could not be installed"
    [ -f Makefile ] || die "no Makefile in $SCRIPT_DIR — cannot build from source"
    local jobs; jobs=$(nproc 2>/dev/null || echo 1)
    make -j"$jobs" || die "build failed"
    [ -f src/jackdaw ] || die "build finished but src/jackdaw is missing"
    PREBUILT="src/jackdaw"
}

# --------------------------------------------------------------------------- #
# Install steps
# --------------------------------------------------------------------------- #
install_binary() {
    log "Installing binary to $BINDIR/jackdaw"
    run_priv install -Dm755 "$PREBUILT" "$BINDIR/jackdaw"
    # Optional LV2 UI helpers MUST live next to the main binary (find_helper()
    # in src/lv2ui_bridge.c looks in the binary's own directory first).
    local h
    for h in jackdaw-lv2ui-gtk2 jackdaw-lv2ui-x11; do
        if [ -f "src/$h" ]; then
            run_priv install -Dm755 "src/$h" "$BINDIR/$h"
            log "Installed helper $h"
        fi
    done
}

install_icons() {
    local s src dst found=0
    for s in 16 22 24 32 48 64 128 256 512; do
        src="icons/hicolor/${s}x${s}/apps/jackdaw.png"
        if [ -f "$src" ]; then
            dst="$ICONROOT/${s}x${s}/apps/jackdaw.png"
            run_priv install -Dm644 "$src" "$dst"
            found=1
        fi
    done
    if [ "$found" -eq 0 ] && [ -f jackdawicon.png ]; then
        run_priv install -Dm644 jackdawicon.png "$ICONROOT/512x512/apps/jackdaw.png"
        found=1
    fi
    # pixmaps fallback for environments that do not consult the hicolor theme.
    if [ -f jackdawicon.png ]; then
        run_priv install -Dm644 jackdawicon.png "$PIXMAPS/jackdaw.png"
    fi
    [ "$found" -eq 1 ] || warn "no icons found to install"
    log "Installed icons under $ICONROOT"
}

install_desktop() {
    [ -f jackdaw.desktop.in ] || { warn "jackdaw.desktop.in missing; skipping launcher"; return 0; }
    local tmp; tmp=$(mktemp)
    sed "s|@bindir@|$BINDIR|g" jackdaw.desktop.in > "$tmp"
    if command -v desktop-file-validate >/dev/null 2>&1; then
        desktop-file-validate "$tmp" || warn "desktop file failed validation (continuing)"
    fi
    run_priv install -Dm644 "$tmp" "$APPDIR/jackdaw.desktop"
    rm -f "$tmp"
    log "Installed launcher to $APPDIR/jackdaw.desktop"
}

refresh_caches() {
    if command -v gtk-update-icon-cache >/dev/null 2>&1; then
        run_priv gtk-update-icon-cache -f -t "$ICONROOT" >/dev/null 2>&1 || true
    elif command -v gtk-update-icon-cache-3.0 >/dev/null 2>&1; then
        run_priv gtk-update-icon-cache-3.0 -f -t "$ICONROOT" >/dev/null 2>&1 || true
    fi
    if command -v update-desktop-database >/dev/null 2>&1; then
        run_priv update-desktop-database "$APPDIR" >/dev/null 2>&1 || true
    fi
}

# --------------------------------------------------------------------------- #
# Main
# --------------------------------------------------------------------------- #
detect_pm
log "Package manager: $PM    Prefix: $PREFIX"

# 1. Runtime dependencies (always, best-effort).
if [ "$NO_DEPS" -eq 0 ]; then
    pkg_install "$(pkg_list runtime)"
else
    warn "--no-deps: skipping dependency installation"
fi

# 2. Decide install method. Explicit flags win; otherwise ask interactively
#    with the prebuilt binary as the default. A chosen/defaulted "prebuilt"
#    still falls back to a source build if the binary is not usable here.
find_prebuilt
METHOD=""
if   [ "$FORCE_BUILD"    -eq 1 ]; then METHOD=build
elif [ "$FORCE_PREBUILT" -eq 1 ]; then METHOD=prebuilt
elif [ -t 0 ]; then
    printf '%s\n' "How should JackDAW be installed?" >&2
    printf '  %s\n' "[1] prebuilt binary (default — fastest, no compiler needed)" >&2
    printf '  %s\n' "[2] build from source" >&2
    printf '%s' "Choice [1]: " >&2
    read -r _m || _m=1
    case "${_m:-1}" in
        2) METHOD=build ;;
        ""|1) METHOD=prebuilt ;;
        *) die "invalid choice: $_m" ;;
    esac
else
    METHOD=prebuilt   # non-interactive default; falls back to build if unusable
fi

case "$METHOD" in
    build)
        [ "$NO_DEPS" -eq 0 ] && pkg_install "$(pkg_list build)"
        build_from_source ;;
    prebuilt)
        if prebuilt_usable "$PREBUILT"; then
            log "Using prebuilt binary: $PREBUILT"
        elif [ "$FORCE_PREBUILT" -eq 1 ]; then
            die "--prebuilt requested but no usable prebuilt binary found"
        else
            warn "no usable prebuilt binary — falling back to building from source"
            [ "$NO_DEPS" -eq 0 ] && pkg_install "$(pkg_list build)"
            build_from_source
        fi ;;
esac

# 3. Install everything.
install_binary
install_icons
install_desktop
refresh_caches

log "JackDAW installed successfully."
cat >&2 <<EOF

${C_GRN}Done.${C_OFF} Launch from your application menu or run: jackdaw
$( [ "$PREFIX" = "$HOME/.local" ] && echo "Ensure $BINDIR is on your PATH." )

For low-latency audio, make sure your user can lock memory and is in the
'audio' group, e.g.:
    sudo usermod -aG audio "$USER"
    # and add to /etc/security/limits.d/audio.conf:
    #   @audio   -  rtprio     95
    #   @audio   -  memlock    unlimited
(Then log out and back in.) JackDAW uses no systemd services.
EOF
