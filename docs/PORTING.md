# Porting Craft to the Zaurus (piko)

How a modern-OpenGL Minecraft clone becomes a software-rendered game on a
2003 handheld. Written while doing it; the estimates are marked as
estimates and the measurements are marked as measured.

---

## The target

From the sibling `piko` repo — mainline Linux on a Sharp Zaurus SL-C760/C860:

| | |
|---|---|
| CPU | PXA255, XScale **ARMv5TE**, ~400 MHz, **no FPU** (EABI soft-float) |
| RAM | **64 MB**, shared with kernel, X and Matchbox |
| Display | 640×480 landscape, `w100fb`, **16 bpp RGB565** |
| libc | uClibc-ng 1.0.54, cross-built via `piko/toolchain/` |
| Storage | jffs2 NAND root (~68 MiB) + SD card; `/tmp` is **on flash, not tmpfs** |
| GPU | none. There is no GL, no GLES, no blitter we can reach |

Two consequences drive every decision below:

1. **No FPU.** Every `float` op is a libgcc soft-float call — tens of
   cycles. The inner loops must be integer fixed-point.
2. **No GPU.** Every pixel is written by the CPU, and the framebuffer is
   slow memory. Pixels are the budget.

## What Craft is

Fogleman's Craft: ~5,700 lines of C99. The renderer is small and
unusually clean, which is what makes this port tractable:

- **5** `glDrawArrays` call sites, all `GL_TRIANGLES`/`GL_LINES` over
  interleaved float VBOs. No index buffers, no instancing.
- **4** GLSL 1.20 shader pairs: block, line, text, sky.
- Mesh generation (`compute_chunk` in `main.c`, `cube.c`) already does
  **exposed-face culling** and bakes **ambient occlusion + light** per
  vertex, on worker threads. That is all reusable as-is.
- Per-chunk **frustum culling** already exists (`chunk_visible`).

So the seam is clean: keep the geometry pipeline, replace everything
below `draw_triangles_*`.

## The core decision: 8-bit paletted, not RGB565

The renderer works in **8-bit palette indices** with a Quake-style
colormap, and only converts to RGB565 in the final blit.

This is not nostalgia. It is three separate wins on this hardware:

1. **Half the fill bandwidth.** The rasterizer writes 1 byte per pixel
   instead of 2. SDRAM bandwidth, not ALU, is the binding constraint on
   a PXA255.
2. **Lighting becomes a table lookup.** `colormap[light][index] → index`
   replaces the fragment shader's multiply-and-clamp. One byte load
   instead of a soft-float pipeline. This is exactly how Quake shaded
   pixels on hardware of this class, and why it ran at all.
3. **The blit already exists.** otQuake's `vid_fb.c` blits an 8-bit
   source through a `palette16[256]` LUT with a 2× pixel-double
   (320×240 → 640×480), writing pairs of RGB565 shorts as single 32-bit
   stores. That path is already tuned and proven on this exact panel.

The cost is colour fidelity: Craft's texture atlas gets quantised to 255
colours (index 255 reserved as the transparency key, matching the
shader's `discard` on magenta). For 16×16 block textures this is barely
visible.

## Render pipeline

```
compute_chunk (unchanged)      exposed faces, baked AO + light
        |
        v
compact vertex buffer          fixed-point, ~8 bytes/vertex (was 40)
        |
        v
frustum cull per chunk         chunk_visible (unchanged)
        |
        v
transform + near-plane clip    fixed-point 4x4, integer
        |
        v
raster: z-buffered affine      1 byte/px into a 320x240 index buffer
        textured spans          colour = colormap[light][texel]
        |
        v
vid_fb 2x blit through         320x240 indices -> 640x480 RGB565
        palette16[]             page-flipped with FBIOPAN_DISPLAY
```

**Resolution.** Render at **320×240**, blit 2×. 76,800 shaded pixels per
frame instead of 307,200 — a 4× saving that costs almost nothing
visually on a 3.7" panel, and it is the path `vid_fb.c` is already fast
at.

**Perspective correction.** Affine per span, subdivided every 16 pixels
— the Quake approach. Block faces are small on screen; full
per-pixel divide is not affordable.

**Budget (estimate, not yet measured).** 76,800 px at ~2× overdraw is
~150k shaded px/frame. At a target 10–15 fps that is 1.5–2.3M px/s. On
400 MHz that allows ~170–260 cycles/px, against an expected inner loop
of ~10–15. The headroom is real but it is spent on the 640×480 blit
(153,600 32-bit stores/frame to slow framebuffer memory) and on vertex
transform, so the triangle count is what actually needs watching —
target a few thousand per frame, not tens of thousands.

## Input

The device has a thumb matrix keyboard and a resistive touchscreen
(ads7846 on SPI1). It has no mouse, and holding a stylus down while
walking is not something a thumb keyboard layout can fight.

**Look: the touchscreen as a DS-style relative stylus.**

- Pen-down **latches an origin** and emits no motion. This is the whole
  point: an absolute panel used absolutely would snap the view to
  wherever the stylus landed. Latching makes the first contact free.
- Subsequent `ABS_X`/`ABS_Y` while down are turned into **deltas from
  the previous sample**, fed to the same code path GLFW's relative
  cursor used to drive.
- Pen-up is **not** a click. Tapping to look must not also break a
  block, which is the failure mode that makes stylus FPS controls
  unplayable.

**Clicks move to the keyboard**, per the ROM's conventions:

| Action | Key |
|---|---|
| Break block (left click) | **Ctrl** |
| Place block (right click) | **AltGr** |

**Movement and the rest** stay on the matrix keyboard. Note the standing
constraint from `piko/AGENTS.md`: this keyboard **cannot type** `/`, `:`,
`[`, `]` or `|`. Craft binds `/` to the command prompt and `` ` `` to
sign entry — both are unreachable and are dropped rather than rebound
into something equally awkward.

## What gets dropped

Removed outright, for RAM, code size, or because the input method cannot
reach them:

- **curl + `auth.c` + `client.c`** — online multiplayer. This is a
  single-player build; it also removes the largest external dependency.
- **GLFW, GLEW** — replaced by the framebuffer/evdev platform layer.
- **lodepng** — textures are converted to flat 8-bit on the host by
  `tools/mkassets.py`; the device never decodes a PNG.
- **Clouds, trees-as-decoration, player models, signs** — the sky-sphere
  and sign text paths cost triangles for things nobody will read at
  320×240.
- **The `/` command prompt and `` ` `` sign entry** — untypeable.

Under evaluation (see task list): replacing **sqlite3** (5.3 MB of
amalgamation source) with a flat chunk store. sqlite is genuinely an
embedded database and may well earn its place; the deciding number is
the static binary size and its peak RSS, which is not measured yet.

Scaled down rather than removed: `CREATE_CHUNK_RADIUS`/
`RENDER_CHUNK_RADIUS` (10 → ~3), `CHUNK_SIZE` (32 → 16),
`MAX_CHUNKS` (8192 → a few hundred), `WORKERS` (4 → 1–2; there is one
core).

## What comes from otQuake

The sibling `otQuake` repo already solved the hardware-facing half of
this on the same board. Reused as **technique**, reimplemented for
Craft's needs rather than copied wholesale:

- `/dev/fb0` mmap, `FBIOGET_VSCREENINFO`, and the VT `KD_GRAPHICS`
  handover so fbcon stops drawing over us.
- **Page flipping** via a doubled virtual framebuffer and
  `FBIOPAN_DISPLAY`. This matters more than it sounds: writing directly
  into the scanned-out buffer produces *several* drifting tear lines
  here, because a full software frame takes longer than one scanout, so
  waiting for vsync only fixes where the first tear starts. w100fb's pan
  handler waits for a clean vblank edge, making the flip atomic.
- The 2× palette blit inner loop.
- The evdev key mapping for the SL-C860's hardware buttons — the corgi
  keypad driver wires them to F-keys (F1 Calendar, F2 Address, F3 Fn,
  F4 Cancel, F10 Mail, F11 OK, F12 Menu, F7/F8 jog dial), and **Cancel
  must be Escape** because this keyboard has no Escape key.
- The cross-compile Makefile shape and the `build-package.sh` →
  `piko/tools/make-ipk.sh` packaging flow.

otQuake does **not** provide a reusable rasterizer: Quake's is a
BSP-span renderer bound to static world geometry, and Craft's world is
dynamic voxels. The triangle rasterizer here is new code.

> **Licensing.** otQuake is GPL (Quake's engine source). Craft is MIT.
> The platform layer here is written fresh against the kernel interfaces
> rather than lifted from `vid_fb.c`, which keeps this tree MIT — the
> reusable part is knowledge about the hardware (which ioctls, which
> quirks, which key codes), not Quake's code.

## Packaging

Mirrors otQuake: `build-package.sh` cross-builds, stages into the card
overlay layout, and calls `piko/tools/make-ipk.sh`.

- `Architecture: **piko**`, never `arm` — `arm` means Sharp-era OABI
  against glibc 2.2.2, and piko's opkg deliberately refuses it.
- Stages to `/mnt/card/.zaurus/usr/bin` and
  `/mnt/card/.zaurus/usr/share/applications`, installed with
  `pkgadd N card`.
- `postinst` runs `deskscan` so matchbox-desktop picks up the `.desktop`
  file the first time this creates the applications directory on a card.
