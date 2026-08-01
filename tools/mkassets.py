#!/usr/bin/env python3
"""
mkassets.py -- convert Craft's PNG assets into the flat binary form the
software renderer loads on the device.

Runs on the build host only. The Zaurus never decodes a PNG: lodepng is
dropped from the device build entirely, and with it the RGBA buffers that
would cost ~350 KB of a 64 MB machine's RAM.

Everything downstream of here is 8-bit palette indices. See
docs/PORTING.md for why (short version: half the fill bandwidth, and
lighting collapses into a table lookup instead of a soft-float multiply
on a CPU with no FPU).

Outputs, into assets/:

  palette.bin   768 B      256 RGB triples. Index 255 is the TRANSPARENCY
                           KEY, reserved to match Craft's fragment shader
                           discarding magenta (1,0,1).

  colormap.bin  64 x 256   colormap[light][index] -> index. light 0 is
                           black, 63 is full bright. Row 255 of every
                           light level stays 255 so shading can never
                           turn a transparent texel opaque.

  blocks.raw    256 x 256  the block atlas, 16x16 grid of 16x16 tiles,
                           STORED FLIPPED -- see below.

  font.raw      128 x 48   16x6 grid of 8x8 glyphs for chars 32..127.
                           Stored as a COVERAGE MASK (0..255 alpha), not
                           palette indices -- text is drawn as a solid
                           colour through this mask, so it needs no
                           palette entries of its own.

  sky.raw       64 x 64    the sky gradient, sampled by (time_of_day,
                           fog_height), also flipped. The original
                           1024x1024 is a smooth gradient; at 320x240
                           nothing is lost by storing 1/256th of it.

The vertical flip matters and is easy to get wrong. GL's v=0 is the
BOTTOM of a texture, so Craft's tile 0 is the bottom-left 16x16 of
texture.png, not the top-left. Flipping once here means the renderer can
index tiles the obvious way (row = tile / 16, counted downwards from the
stored image's first row) with no per-pixel correction. Skipping the flip
does not look subtly wrong, it looks like every block is transparent,
because the tiles Craft actually references land on the empty top rows.

The font needs a different treatment again: its cells are 32x64 (glyphs
are one cell wide and TWO tall, per make_character in cube.c) and the
glyph ink sits inside a wide margin. Rescaling the raw cells would waste
most of the output on padding, so each glyph is cropped to the font's
common ink bounding box before being packed into a square 8x8.

Usage:  python3 tools/mkassets.py [--out assets]
"""

import argparse
import os
import sys

try:
    import numpy as np
    from PIL import Image
except ImportError as exc:  # pragma: no cover
    sys.exit("mkassets.py needs Pillow and numpy on the build host: %s" % exc)


# Craft's shader treats exactly this colour as "discard".
KEY_RGB = (255, 0, 255)
KEY_INDEX = 255

# 255 usable colours; the 256th is the key.
PALETTE_COLOURS = 255

LIGHT_LEVELS = 64

# Source font layout, from make_character() in cube.c: 16 columns x 8
# rows of cells, each one glyph wide and two tall, and character `c`
# lives in cell `c - 32`.
FONT_SRC_COLS = 16
FONT_SRC_ROWS = 8
FONT_FIRST = 32

# Output: 8x8 glyphs for chars 32..127, packed 16 across.
FONT_GLYPH = 8
FONT_COLS = 16
FONT_ROWS = 6

SKY_OUT = 64


def load_rgba(path):
    return np.asarray(Image.open(path).convert("RGBA"), dtype=np.uint8)


def nearest_indices(rgb, palette, chunk=4096):
    """Map an (N,3) uint8 array to nearest entries in an (P,3) palette.

    Deduplicates first: these atlases have a few hundred distinct colours
    at most, so the brute-force distance matrix stays small. Doing it on
    all 65,536 pixels directly would allocate ~200 MB.
    """
    flat = rgb.reshape(-1, 3)
    uniq, inverse = np.unique(flat, axis=0, return_inverse=True)

    pal = palette.astype(np.int32)
    out = np.empty(len(uniq), dtype=np.uint8)
    for start in range(0, len(uniq), chunk):
        block = uniq[start:start + chunk].astype(np.int32)
        d = ((block[:, None, :] - pal[None, :, :]) ** 2).sum(axis=2)
        out[start:start + chunk] = d.argmin(axis=1).astype(np.uint8)

    return out[inverse].reshape(rgb.shape[:2])


def build_palette(sources):
    """Median-cut a shared palette across every image that needs one.

    A shared palette is the point: the rasterizer blends and shades
    across atlases (a block face fogged toward the sky colour), so they
    have to live in the same index space.
    """
    rows = []
    for rgba in sources:
        rgb = rgba[..., :3].reshape(-1, 3)
        alpha = rgba[..., 3].reshape(-1)
        # Drop fully transparent pixels and the magenta key -- neither
        # should waste one of the 255 slots.
        keep = alpha > 0
        keep &= ~np.all(rgb == np.array(KEY_RGB, dtype=np.uint8), axis=1)
        rows.append(rgb[keep])

    pixels = np.concatenate(rows, axis=0)

    # PIL's adaptive quantiser is a median cut. Feed it the pixels as a
    # 1-pixel-tall image so no spatial dithering is involved.
    img = Image.fromarray(pixels.reshape(1, -1, 3).astype(np.uint8), "RGB")
    quant = img.quantize(colors=PALETTE_COLOURS, method=Image.MEDIANCUT)

    raw = quant.getpalette()[: PALETTE_COLOURS * 3]
    palette = np.array(raw, dtype=np.uint8).reshape(PALETTE_COLOURS, 3)

    full = np.zeros((256, 3), dtype=np.uint8)
    full[:PALETTE_COLOURS] = palette
    full[KEY_INDEX] = KEY_RGB
    return full


def build_colormap(palette):
    """colormap[light][index] -> index, for light 0 (black) .. 63 (full).

    This is the whole lighting model. Craft's fragment shader computes
    `color * (ambient + light_color * diffuse) * ao` and clamps; here
    that collapses to one multiplier per pixel, quantised to 64 steps and
    resolved back into the palette ahead of time. On a CPU with no FPU
    the difference between this and the shader is roughly a byte load
    versus a few hundred cycles.
    """
    usable = palette[:PALETTE_COLOURS].astype(np.float32)

    levels = np.arange(LIGHT_LEVELS, dtype=np.float32) / (LIGHT_LEVELS - 1)
    # (light, colour, 3)
    shaded = usable[None, :, :] * levels[:, None, None]
    shaded = np.clip(shaded, 0, 255).astype(np.uint8)

    cmap = np.empty((LIGHT_LEVELS, 256), dtype=np.uint8)
    for light in range(LIGHT_LEVELS):
        cmap[light, :PALETTE_COLOURS] = nearest_indices(
            shaded[light].reshape(1, -1, 3), palette[:PALETTE_COLOURS]
        ).reshape(-1)
    # Transparent stays transparent at every light level.
    cmap[:, KEY_INDEX] = KEY_INDEX
    return cmap


def quantise_atlas(rgba, palette):
    """Block atlas -> indices, with alpha and magenta both becoming the key."""
    idx = nearest_indices(rgba[..., :3], palette[:PALETTE_COLOURS])
    transparent = rgba[..., 3] < 128
    transparent |= np.all(rgba[..., :3] == np.array(KEY_RGB, np.uint8), axis=2)
    idx[transparent] = KEY_INDEX
    return idx


def build_font(rgba):
    """Font -> a tight 16x6 grid of 8x8 coverage masks for chars 32..127.

    Only the alpha channel carries the glyph. The source cells are 32x64
    with the ink floating in a large margin, so a common ink bounding box
    is measured across every printable glyph and each cell is cropped to
    it before being squared off. Rescaling whole cells instead would
    spend most of the 8x8 on empty padding and leave the text unreadable.
    """
    alpha = rgba[..., 3]
    h, w = alpha.shape
    cw = w // FONT_SRC_COLS
    ch = h // FONT_SRC_ROWS

    count = FONT_COLS * FONT_ROWS

    # Union ink bounding box, in cell-local coordinates.
    x0, y0, x1, y1 = cw, ch, 0, 0
    for cell in range(count):
        cx = (cell % FONT_SRC_COLS) * cw
        cy = (cell // FONT_SRC_COLS) * ch
        ink = alpha[cy:cy + ch, cx:cx + cw] > 127
        if not ink.any():
            continue
        rows = np.where(ink.any(axis=1))[0]
        cols = np.where(ink.any(axis=0))[0]
        x0 = min(x0, cols[0]); x1 = max(x1, cols[-1] + 1)
        y0 = min(y0, rows[0]); y1 = max(y1, rows[-1] + 1)

    if x1 <= x0 or y1 <= y0:          # no ink at all: bail out visibly
        raise SystemExit("font.png has no glyph coverage -- wrong file?")

    out = np.zeros((FONT_ROWS * FONT_GLYPH, FONT_COLS * FONT_GLYPH),
                   dtype=np.uint8)

    for cell in range(count):
        cx = (cell % FONT_SRC_COLS) * cw
        cy = (cell // FONT_SRC_COLS) * ch
        crop = alpha[cy + y0:cy + y1, cx + x0:cx + x1]
        glyph = Image.fromarray(crop, "L").resize(
            (FONT_GLYPH, FONT_GLYPH), Image.BILINEAR)
        dx = (cell % FONT_COLS) * FONT_GLYPH
        dy = (cell // FONT_COLS) * FONT_GLYPH
        out[dy:dy + FONT_GLYPH, dx:dx + FONT_GLYPH] = np.asarray(glyph)

    return out


def downscale_indexed(rgba, size, palette):
    img = Image.fromarray(rgba[..., :3], "RGB").resize((size, size), Image.BOX)
    return nearest_indices(np.asarray(img, dtype=np.uint8),
                           palette[:PALETTE_COLOURS])


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--textures", default="textures")
    ap.add_argument("--out", default="assets")
    args = ap.parse_args()

    tex = lambda n: os.path.join(args.textures, n)
    os.makedirs(args.out, exist_ok=True)

    blocks = load_rgba(tex("texture.png"))
    font = load_rgba(tex("font.png"))
    sky = load_rgba(tex("sky.png"))

    # The font contributes no colours -- it is drawn as a mask.
    palette = build_palette([blocks, sky])
    colormap = build_colormap(palette)

    written = []

    def emit(name, array):
        path = os.path.join(args.out, name)
        data = np.ascontiguousarray(array).tobytes()
        with open(path, "wb") as fh:
            fh.write(data)
        written.append((name, len(data)))

    # np.flipud puts GL's v=0 row first, so the renderer indexes tiles
    # top-down without a per-sample correction. See the note above.
    emit("palette.bin", palette)
    emit("colormap.bin", colormap)
    emit("blocks.raw", np.flipud(quantise_atlas(blocks, palette)))
    emit("font.raw", build_font(font))
    emit("sky.raw", np.flipud(downscale_indexed(sky, SKY_OUT, palette)))

    total = sum(size for _, size in written)
    for name, size in written:
        print("  %-14s %7d B" % (name, size))
    print("  %-14s %7d B total" % ("", total))


if __name__ == "__main__":
    main()
