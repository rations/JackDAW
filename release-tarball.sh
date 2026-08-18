#!/bin/sh
#
# release-tarball.sh — build a JackDAW release tarball.
#
# Produces  jackdaw-<VERSION>.tar.gz  which unpacks into a top-level directory
# named  JackDAW/  containing the source, bundled headers, icons, packaging
# scripts and (by default) the prebuilt binary.
#
# Usage:
#   ./release-tarball.sh [VERSION] [--no-binary]
#
#   VERSION       Override the version (e.g. 0.2.0). If omitted, it is read
#                 from src/config.h (#define VERSION "...").
#   --no-binary   Source-only tarball (install will always build from source).
#
# Edit the VERSION default below, or pass it on the command line each release.
#
set -eu

# --------------------------------------------------------------------------- #
# Default version — edit here, or override with the first argument.
# Empty means "read from src/config.h".
# --------------------------------------------------------------------------- #
VERSION=""

INCLUDE_BINARY=1
for arg in "$@"; do
    case "$arg" in
        --no-binary) INCLUDE_BINARY=0 ;;
        -h|--help) sed -n '2,/^set -eu/{/^set -eu/d;s/^# \{0,1\}//;p}' "$0"; exit 0 ;;
        --*) printf 'error: unknown option: %s\n' "$arg" >&2; exit 1 ;;
        *) VERSION="$arg" ;;
    esac
done

# Operate from the repo root (this script's directory).
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
cd "$ROOT"

# Resolve version from config.h if not provided.
if [ -z "$VERSION" ]; then
    VERSION=$(sed -n 's/^#define VERSION[[:space:]]*"\(.*\)"/\1/p' src/config.h)
    [ -n "$VERSION" ] || { echo "error: could not read VERSION from src/config.h" >&2; exit 1; }
fi
VERSION=${VERSION#v}   # tolerate a leading 'v'

NAME="jackdaw-$VERSION"
TARBALL="$ROOT/$NAME.tar.gz"

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
DEST="$STAGE/JackDAW"
mkdir -p "$DEST"

echo "==> Staging JackDAW $VERSION" >&2

# --------------------------------------------------------------------------- #
# Top-level files
# --------------------------------------------------------------------------- #
for f in Makefile LICENSE README.md jackdawicon.png jackdaw.desktop.in \
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
