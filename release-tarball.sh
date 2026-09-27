#!/bin/sh
#
# release-tarball.sh — build a JackDAW release tarball.
#
# Produces  jackdaw-<VERSION>-linux-<ARCH>.tar.gz  (or  jackdaw-<VERSION>.tar.gz
# for a source-only tarball) which unpacks into a top-level directory named
# JackDAW/  containing the source, bundled headers, icons, packaging scripts
# and (by default) the prebuilt binary. <ARCH> is read from the binary itself
# (x86_64, aarch64), so the name says what it runs on, not where it was packed.
#
# Usage:
#   ./release-tarball.sh [--no-binary]
#
#   --no-binary   Source-only tarball (install will always build from source).
#
# The version is the VERSION file at the repo root, the same file the Makefile
# compiles into the binary, so every architecture's tarball carries one number.
# To release: edit VERSION, rebuild (make rebuilds everything when it changes),
# then run this. A binary built from a different VERSION is refused.
#
set -eu

INCLUDE_BINARY=1
for arg in "$@"; do
    case "$arg" in
        --no-binary) INCLUDE_BINARY=0 ;;
        -h|--help) sed -n '2,/^set -eu/{/^set -eu/d;s/^# \{0,1\}//;p}' "$0"; exit 0 ;;
        *) printf 'error: unknown argument: %s (the version is set in ./VERSION)\n' "$arg" >&2; exit 1 ;;
    esac
done

# Operate from the repo root (this script's directory).
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
cd "$ROOT"

VERSION=$(tr -d ' \t\r\n' < VERSION 2>/dev/null) || VERSION=""
[ -n "$VERSION" ] || { echo "error: VERSION file missing or empty" >&2; exit 1; }

# Refuse a binary built before VERSION was bumped: its tarball would be named
# for one version and report another. The version is compiled into the window
# title format string ("%s — JackDAW " VERSION in mainwindow.c), so it is
# visible in the binary without running it.
if [ "$INCLUDE_BINARY" -eq 1 ] && [ -f src/jackdaw ] &&
   ! grep -aqF "JackDAW $VERSION" src/jackdaw; then
    echo "error: src/jackdaw was not built from VERSION $VERSION; run make first" >&2
    exit 1
fi

# A binary tarball only runs on one architecture, so it says which in its
# name. Taken from the ELF header rather than uname -m, so a binary built
# elsewhere is still named for what it is.
ARCH=""
if [ "$INCLUDE_BINARY" -eq 1 ] && [ -f src/jackdaw ]; then
    if command -v readelf >/dev/null 2>&1; then
        case $(readelf -h src/jackdaw 2>/dev/null) in
            *X86-64*)  ARCH=x86_64 ;;
            *AArch64*) ARCH=aarch64 ;;
        esac
    fi
    [ -n "$ARCH" ] || ARCH=$(uname -m)
fi

NAME="jackdaw-$VERSION${ARCH:+-linux-$ARCH}"
TARBALL="$ROOT/$NAME.tar.gz"

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
DEST="$STAGE/JackDAW"
mkdir -p "$DEST"

echo "==> Staging JackDAW $VERSION" >&2

# --------------------------------------------------------------------------- #
# Top-level files
# --------------------------------------------------------------------------- #
for f in Makefile VERSION LICENSE README.md jackdawicon.png jackdaw.desktop.in \
         install-jackdaw.sh uninstall-jackdaw.sh release-tarball.sh; do
    [ -e "$f" ] && cp -p "$f" "$DEST/" || echo "  (skip missing $f)" >&2
done

# --------------------------------------------------------------------------- #
# Bundled headers, the VST3 SDK, and pre-generated icons.
#
# ext/ carries the vendored vestige/CLAP/LADSPA headers AND ext/vst3sdk, which
# is a git submodule (Steinberg VST3 SDK, MIT licensed — its LICENSE.txt files
# are copied along with it, which is what the licence requires). A tarball is
# not a git checkout, so the submodule's .git pointer files must be stripped:
# each contains a "gitdir: ../../.git/modules/..." path that does not exist once
# unpacked, and git tooling run inside the unpacked tree trips over them.
# --------------------------------------------------------------------------- #
if [ -d ext ]; then
    cp -a ext "$DEST/"
    find "$DEST/ext" -name '.git' -exec rm -rf {} + 2>/dev/null || true
    if [ ! -f "$DEST/ext/vst3sdk/pluginterfaces/base/funknown.cpp" ]; then
        echo "  warning: ext/vst3sdk is empty — the tarball will only build with VST3=0" >&2
        echo "           run: git submodule update --init ext/vst3sdk" >&2
        echo "                git -C ext/vst3sdk submodule update --init base pluginterfaces public.sdk" >&2
    fi
fi
[ -d icons ] && cp -a icons "$DEST/"

# --------------------------------------------------------------------------- #
# Source: *.c *.cpp *.h plus config.h (config.h is gitignored but required).
# Exclude build artifacts (*.o *.d).
# --------------------------------------------------------------------------- #
mkdir -p "$DEST/src"
find src -maxdepth 1 -type f \
        \( -name '*.c' -o -name '*.cpp' -o -name '*.h' \) \
        -exec cp -p {} "$DEST/src/" \;
[ -f src/config.h ] && cp -p src/config.h "$DEST/src/"

# --------------------------------------------------------------------------- #
# Prebuilt binary + helpers (preserve exec bit) unless --no-binary.
# --------------------------------------------------------------------------- #
if [ "$INCLUDE_BINARY" -eq 1 ]; then
    if [ -f src/jackdaw ]; then
        cp -p src/jackdaw "$DEST/src/"
        for h in jackdaw-lv2ui-gtk2 jackdaw-lv2ui-x11; do
            [ -f "src/$h" ] && cp -p "src/$h" "$DEST/src/"
        done
        echo "  included prebuilt binary" >&2
    else
        echo "  warning: src/jackdaw not found — producing source-only tarball" >&2
    fi
else
    echo "  --no-binary: source-only tarball" >&2
fi

# --------------------------------------------------------------------------- #
# Pack. Tarball root is JackDAW/.
# --------------------------------------------------------------------------- #
echo "==> Creating $TARBALL" >&2
tar -czf "$TARBALL" -C "$STAGE" JackDAW

# --------------------------------------------------------------------------- #
# Report
# --------------------------------------------------------------------------- #
SIZE=$(du -h "$TARBALL" | cut -f1)
echo "==> Done: $TARBALL ($SIZE)" >&2
if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$TARBALL"
fi
