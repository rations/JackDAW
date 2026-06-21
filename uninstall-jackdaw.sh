#!/bin/sh
#
# uninstall-jackdaw.sh — remove a JackDAW install placed by install-jackdaw.sh.
#
# Removes the binary, the LV2 UI helpers, the icon set and the .desktop
# launcher, then refreshes the icon/desktop caches. Distro packages are NOT
# removed unless --purge is given.
#
# Usage:
#   ./uninstall-jackdaw.sh [--purge] [-h|--help]
#
# Use the SAME prefix you installed with: it prompts interactively, or set
# PREFIX= in the environment (e.g. PREFIX=$HOME/.local ./uninstall-jackdaw.sh).
#
set -eu

if [ -t 2 ]; then
    C_RED=$(printf '\033[31m'); C_YEL=$(printf '\033[33m')
    C_GRN=$(printf '\033[32m'); C_OFF=$(printf '\033[0m')
else
    C_RED=''; C_YEL=''; C_GRN=''; C_OFF=''
fi
log()  { printf '%s==>%s %s\n'  "$C_GRN" "$C_OFF" "$*" >&2; }
warn() { printf '%swarning:%s %s\n' "$C_YEL" "$C_OFF" "$*" >&2; }
die()  { printf '%serror:%s %s\n'   "$C_RED" "$C_OFF" "$*" >&2; exit 1; }

PURGE=0
for arg in "$@"; do
    case "$arg" in
        --purge) PURGE=1 ;;
        -h|--help) sed -n '2,/^set -eu/{/^set -eu/d;s/^# \{0,1\}//;p}' "$0"; exit 0 ;;
        *) die "unknown argument: $arg (try --help)" ;;
    esac
done

# --------------------------------------------------------------------------- #
# Prefix selection — must match what install-jackdaw.sh used.
# --------------------------------------------------------------------------- #
if [ -z "${PREFIX:-}" ]; then
    if [ ! -t 0 ]; then
        die "no terminal for the prefix prompt; set PREFIX= explicitly"
    fi
    printf '%s\n' "Which JackDAW install should be removed?" >&2
    printf '  %s\n' "[1] system-wide  (/usr/local)" >&2
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
# Privilege escalation (same policy as install).
# --------------------------------------------------------------------------- #
NEED_SUDO=0
if [ "$(id -u)" -ne 0 ]; then
    if [ ! -w "$PREFIX" ] && [ -e "$PREFIX" ]; then
        if command -v sudo >/dev/null 2>&1; then NEED_SUDO=1
        else die "no write access to $PREFIX and sudo not found; re-run as root"; fi
    fi
fi
run_priv() { if [ "$NEED_SUDO" -eq 1 ]; then sudo "$@"; else "$@"; fi; }

# --------------------------------------------------------------------------- #
# Remove files (idempotent). Auditable, fixed list that mirrors the installer.
# --------------------------------------------------------------------------- #
removed=0
rm_file() {
    if [ -e "$1" ]; then
        run_priv rm -f "$1" && { log "removed $1"; removed=$((removed+1)); }
    fi
}

rm_file "$BINDIR/jackdaw"
rm_file "$BINDIR/jackdaw-lv2ui-gtk2"
rm_file "$BINDIR/jackdaw-lv2ui-x11"
rm_file "$APPDIR/jackdaw.desktop"
rm_file "$PIXMAPS/jackdaw.png"
for s in 16 22 24 32 48 64 128 256 512; do
    rm_file "$ICONROOT/${s}x${s}/apps/jackdaw.png"
done

[ "$removed" -eq 0 ] && warn "nothing found to remove under $PREFIX"

# --------------------------------------------------------------------------- #
# Refresh caches (guarded, same as install).
# --------------------------------------------------------------------------- #
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    run_priv gtk-update-icon-cache -f -t "$ICONROOT" >/dev/null 2>&1 || true
elif command -v gtk-update-icon-cache-3.0 >/dev/null 2>&1; then
    run_priv gtk-update-icon-cache-3.0 -f -t "$ICONROOT" >/dev/null 2>&1 || true
fi
if command -v update-desktop-database >/dev/null 2>&1; then
    run_priv update-desktop-database "$APPDIR" >/dev/null 2>&1 || true
fi

# --------------------------------------------------------------------------- #
# Optional package purge (off by default).
# --------------------------------------------------------------------------- #
if [ "$PURGE" -eq 1 ]; then
    warn "JackDAW's runtime libraries are shared with other applications."
    warn "Removing them may break other software. Listing only — review carefully."
    if   command -v apt-get >/dev/null 2>&1; then
        echo "  sudo apt-get remove libgtk-3-0 libjack-jackd2-0 libsndfile1 libsamplerate0 liblilv-0-0 libsuil-0-0" >&2
    elif command -v dnf >/dev/null 2>&1; then
        echo "  sudo dnf remove gtk3 jack-audio-connection-kit libsndfile libsamplerate lilv suil" >&2
    elif command -v pacman >/dev/null 2>&1; then
        echo "  sudo pacman -Rs gtk3 jack2 libsndfile libsamplerate lilv suil" >&2
    elif command -v zypper >/dev/null 2>&1; then
        echo "  sudo zypper remove libgtk-3-0 libjack0 libsndfile1 libsamplerate0 liblilv-0-0 libsuil-0-0" >&2
    elif command -v apk >/dev/null 2>&1; then
        echo "  sudo apk del gtk+3.0 jack libsndfile libsamplerate lilv suil" >&2
    fi
    warn "Run the command above manually if you really want to remove the libraries."
fi

log "JackDAW uninstalled."
