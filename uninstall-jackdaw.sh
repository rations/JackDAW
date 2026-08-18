#!/bin/sh
#
# uninstall-jackdaw.sh — remove a JackDAW install placed by install-jackdaw.sh.
#
# Removes the binary, the LV2 UI helpers, the icon set and the .desktop
# launcher, then refreshes the icon/desktop caches. Distro packages are never
# touched.
#
# Usage:
#   ./uninstall-jackdaw.sh [-h|--help]
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

for arg in "$@"; do
    case "$arg" in
        -h|--help) sed -n '2,/^set -eu/{/^set -eu/d;s/^# \{0,1\}//;p}' "$0"; exit 0 ;;
        *) die "unknown argument: $arg (try --help)" ;;
    esac
done

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)

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
# Leftovers under the OTHER prefix. This run only touched $PREFIX, so a copy
# installed under a different prefix survives and keeps answering to `jackdaw`
# and to the application menu — which looks exactly like a failed uninstall.
# Report it; never remove it, since it was not the prefix we were asked for.
# --------------------------------------------------------------------------- #
dir_on_path() {
    _dop_dir="$1"
    _dop_ifs="$IFS"
    IFS=:
    for _dop_p in $PATH; do
        if [ "$_dop_p" = "$_dop_dir" ]; then IFS="$_dop_ifs"; return 0; fi
    done
    IFS="$_dop_ifs"
    return 1
}

check_remaining_install() {
    for _other in /usr/local "$HOME/.local"; do
        [ "${_other%/}" = "${PREFIX%/}" ] && continue
        _obin="$_other/bin/jackdaw"
        _odesk="$_other/share/applications/jackdaw.desktop"
        [ -e "$_obin" ] || [ -e "$_odesk" ] || continue
        warn "JackDAW is STILL installed under $_other (this run only touched $PREFIX):"
        [ -e "$_obin"  ] && warn "    $_obin"
        [ -e "$_odesk" ] && warn "    $_odesk"
        if [ -e "$_obin" ] && dir_on_path "$_other/bin"; then
            warn "  '$_other/bin' is on PATH — 'jackdaw' still starts that copy."
        fi
        [ -e "$_odesk" ] && warn "  its application-menu entry is still in place."
        warn "  Remove it too with:  PREFIX=$_other $SCRIPT_DIR/uninstall-jackdaw.sh"
    done
    return 0
}

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
# Per-user data in ~/.jackdaw. Reported, never removed: alongside the settings
# and the (regenerable) plugin scan caches it holds recordings/, which is
# captured audio the user may not have copied anywhere else. Deleting that on an
# uninstall would be destroying work, so print the path and let them decide.
# --------------------------------------------------------------------------- #
report_user_data() {
    _ud="$HOME/.jackdaw"
    [ -d "$_ud" ] || return 0
    log "Per-user data left in place: $_ud"
    [ -f "$_ud/config" ]      && log "    config       settings (theme, ports, preferences)"
    [ -f "$_ud/plugincache" ] && log "    plugincache  plugin scan cache (regenerated on demand)"
    [ -f "$_ud/pluginindex" ] && log "    pluginindex  plugin scan index (regenerated on demand)"
    if [ -d "$_ud/recordings" ]; then
        _n=$(find "$_ud/recordings" -type f 2>/dev/null | wc -l)
        warn "    recordings/  $_n recorded file(s) — YOUR AUDIO, not removed"
    fi
    log "  Remove it yourself if you want a clean slate:  rm -rf $_ud"
    return 0
}

log "JackDAW uninstalled from $PREFIX."

report_user_data

# Last, so a surviving copy under the other prefix is the final thing seen.
check_remaining_install
