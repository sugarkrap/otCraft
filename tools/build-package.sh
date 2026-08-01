#!/bin/sh
set -eu

# Builds an .ipk of the otCraft on-device smoke test, installable with
# piko's opkg (tools/make-ipk.sh in the sibling piko repo -- see
# ../piko/docs/HOWTO-PACKAGES.md for the format and why the architecture
# is "piko", not "arm").
#
# Usage:
#   tools/build-package.sh [--version VER] [--out DIR]
#
# This packages the SMOKE TEST, not a game: the port has no playable
# binary yet (main.c is still on OpenGL). What ships here is the
# rasterizer plus the framebuffer/evdev platform layer and the assets
# they need, which is exactly the code that has never run on hardware.

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
PIKO="$(CDPATH= cd -- "$REPO/../piko" 2>/dev/null && pwd)" || {
    echo "FAILED: sibling piko repo not found at $REPO/../piko" >&2
    echo "make-ipk.sh (package builder) and the cross-toolchain both live there." >&2
    exit 1
}
MAKEIPK="$PIKO/tools/make-ipk.sh"
[ -x "$MAKEIPK" ] || { echo "FAILED: $MAKEIPK not found or not executable" >&2; exit 1; }

VERSION=""
OUTDIR="."

while [ $# -gt 0 ]; do
    case "$1" in
        --version) VERSION="${2:?--version needs a value}"; shift 2 ;;
        --out)     OUTDIR="${2:?--out needs a value}"; shift 2 ;;
        -h|--help) sed -n '3,16p' "$0"; exit 0 ;;
        *) echo "FAILED: unknown option: $1" >&2; exit 1 ;;
    esac
done

[ -n "$VERSION" ] || VERSION="$(cd "$REPO" && git describe --always --dirty 2>/dev/null)"
[ -n "$VERSION" ] || VERSION="0.0.0-unknown"

cd "$REPO"

[ -f otcraft-smoke ] || { echo "FAILED: otcraft-smoke not built" >&2; exit 1; }
for f in palette.bin colormap.bin blocks.raw font.raw sky.raw; do
    [ -f "assets/$f" ] || { echo "FAILED: assets/$f missing (run mkassets.py)" >&2; exit 1; }
done

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT INT TERM

# mktemp -d makes this 0700, and make-ipk.sh tars it as the package's
# "./" entry -- opkg would then apply that mode to the already-existing
# shared destination root, locking it down to root only. 0755 matches
# what a plain `mkdir -p` would have given it.
chmod 755 "$STAGE"

mkdir -p "$STAGE/usr/bin" "$STAGE/usr/share/otcraft"
cp -p otcraft-smoke "$STAGE/usr/bin/"
cp -p assets/palette.bin assets/colormap.bin assets/blocks.raw \
      assets/font.raw assets/sky.raw "$STAGE/usr/share/otcraft/"

# No usr/share/applications: this is a diagnostic run from a shell, and
# a desktop icon for it would be misleading. make-ipk.sh notes the
# absence rather than leaving it to be discovered.
"$MAKEIPK" \
    --name otcraft-smoke \
    --version "$VERSION" \
    --root "$STAGE" \
    --arch piko \
    --desc "otCraft software rasterizer smoke test" \
    --out "$OUTDIR"
