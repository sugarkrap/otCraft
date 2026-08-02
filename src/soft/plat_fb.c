/*
 * plat_fb.c -- Linux framebuffer + evdev backend for the Zaurus.
 *
 * Replaces GLFW entirely. Takes over /dev/fb0, blits the renderer's
 * 320x240 index buffer to the 640x480 panel through an RGB565 lookup,
 * and reads the built-in keyboard and the ads7846 touchscreen straight
 * from evdev.
 *
 * The hardware-facing details here (which ioctls, the page-flip dance,
 * the corgi keypad's F-key wiring) were established by the sibling
 * otQuake port on this same board; see docs/PORTING.md. The code is
 * written fresh against the kernel interfaces rather than lifted, which
 * also keeps this tree MIT.
 *
 * NOT YET RUN ON HARDWARE -- see the note at the bottom of the file.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kd.h>

#include "plat.h"
#include "soft.h"

/* ── framebuffer ────────────────────────────────────────────────────── */

static int    fb_fd = -1;
static void  *fb_mem = MAP_FAILED;
static size_t fb_mem_size;
static int    fb_width, fb_height, fb_line_len, fb_bpp;

/*
 * Page flipping.
 *
 * Writing straight into the buffer the panel is scanning out does not
 * merely tear once: a full software frame here takes longer than one
 * scanout, so the scan position laps the write position and produces
 * several drifting diagonal seams. Waiting for vsync first only fixes
 * where the FIRST seam starts.
 *
 * So when the driver reports it can pan (ypanstep non-zero), we ask for
 * a doubled virtual framebuffer, draw entirely into the invisible page
 * and flip with FBIOPAN_DISPLAY. w100fb's pan handler waits for a clean
 * vblank edge itself, making the flip atomic no matter how long the blit
 * took. Older kernels fall back to writing directly.
 */
static int page_flip = 0;
static int back_page = 1;
static int page_bytes;

/*
 * The framebuffer's geometry exactly as we found it.
 *
 * Page flipping leaves the panel showing whichever page we panned to
 * last, and asks the driver for a doubled yres_virtual. Neither is ours
 * to keep: exiting without putting both back leaves X drawing into page
 * 0 while the panel displays page 1, which looks like a frozen or
 * corrupted device rather than a program that has quit. Observed for
 * real -- /sys/class/graphics/fb0/pan read "0,480" after the first
 * on-device run.
 */
static struct fb_var_screeninfo orig_var;
static int orig_var_valid = 0;

static uint16_t palette16[256];

/* VT ownership, so fbcon stops drawing over us. */
static int tty_fd = -1;
static int tty_graphics = 0;

/* ── input ──────────────────────────────────────────────────────────── */

#define MAX_INPUT_FDS 6
static int input_fds[MAX_INPUT_FDS];
static int input_count;

static uint8_t key_state[PLAT_MAX_KEYS];

#define EVENT_QUEUE 64
static plat_event queue[EVENT_QUEUE];
static int queue_head, queue_tail;

/* Accumulated stylus motion, drained by plat_get_look_delta(). */
static int look_dx, look_dy;

/*
 * Stylus tracking.
 *
 * The panel is absolute, but using it absolutely would snap the view to
 * wherever the pen lands. So the first sample after pen-down only
 * LATCHES an origin and emits nothing; every later sample contributes
 * its difference from the previous one. That is the DS stylus feel:
 * where you touch does not matter, only how you drag.
 *
 * Pen-up deliberately emits no click. Tapping to look must not also
 * break a block -- that failure mode is what makes stylus FPS controls
 * unplayable, and it is why Ctrl and AltGr carry the two clicks instead.
 */
static int pen_down;
static int pen_have_origin;
static int pen_last_x, pen_last_y;
static int pen_x, pen_y, pen_pending;

/*
 * Raw ads7846 counts per look unit. The panel reports ADC values, not
 * pixels, and its full span is a few thousand counts, so this scales a
 * comfortable drag into a useful turn. Overridable because taste and
 * calibration both vary.
 */
static int look_divisor = 6;

static double time_base;

/* ── time ───────────────────────────────────────────────────────────── */

static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

double plat_time(void)
{
    return now_seconds() - time_base;
}

/* ── event queue ────────────────────────────────────────────────────── */

static void push_event(int type, int value)
{
    int next = (queue_tail + 1) % EVENT_QUEUE;

    if (next == queue_head)
        return;                     /* full: drop, rather than block */
    queue[queue_tail].type = type;
    queue[queue_tail].value = value;
    queue_tail = next;
}

int plat_poll_event(plat_event *ev)
{
    if (queue_head == queue_tail)
        return 0;
    *ev = queue[queue_head];
    queue_head = (queue_head + 1) % EVENT_QUEUE;
    return 1;
}

int plat_key_down(int key)
{
    if (key < 0 || key >= PLAT_MAX_KEYS)
        return 0;
    return key_state[key];
}

void plat_get_look_delta(int *dx, int *dy)
{
    if (dx) *dx = look_dx;
    if (dy) *dy = look_dy;
    look_dx = look_dy = 0;
}

/* ── evdev key mapping ──────────────────────────────────────────────── */

/*
 * The SL-C860's hardware buttons are wired to F-keys by the corgi keypad
 * driver, and this thumb keyboard has no F-row of its own, so those
 * codes mean the buttons and nothing else:
 *
 *   F1 Calendar   F2 Address   F3 Fn      F4 Cancel
 *   F10 Mail      F11 OK       F12 Menu   F7/F8 jog dial
 *
 * Cancel therefore has to be Escape: it is the only way out of a menu on
 * a keyboard with no Escape key.
 */
static int evkey_to_plat(unsigned code)
{
    switch (code) {
    case KEY_A: return 'A';  case KEY_B: return 'B';  case KEY_C: return 'C';
    case KEY_D: return 'D';  case KEY_E: return 'E';  case KEY_F: return 'F';
    case KEY_G: return 'G';  case KEY_H: return 'H';  case KEY_I: return 'I';
    case KEY_J: return 'J';  case KEY_K: return 'K';  case KEY_L: return 'L';
    case KEY_M: return 'M';  case KEY_N: return 'N';  case KEY_O: return 'O';
    case KEY_P: return 'P';  case KEY_Q: return 'Q';  case KEY_R: return 'R';
    case KEY_S: return 'S';  case KEY_T: return 'T';  case KEY_U: return 'U';
    case KEY_V: return 'V';  case KEY_W: return 'W';  case KEY_X: return 'X';
    case KEY_Y: return 'Y';  case KEY_Z: return 'Z';

    case KEY_0: return '0';  case KEY_1: return '1';  case KEY_2: return '2';
    case KEY_3: return '3';  case KEY_4: return '4';  case KEY_5: return '5';
    case KEY_6: return '6';  case KEY_7: return '7';  case KEY_8: return '8';
    case KEY_9: return '9';

    case KEY_SPACE:     return PLAT_KEY_SPACE;
    case KEY_TAB:       return PLAT_KEY_TAB;
    case KEY_ENTER:     return PLAT_KEY_ENTER;
    case KEY_BACKSPACE: return PLAT_KEY_BACKSPACE;
    case KEY_MINUS:     return PLAT_KEY_MINUS;
    case KEY_EQUAL:     return PLAT_KEY_EQUAL;
    case KEY_COMMA:     return PLAT_KEY_COMMA;
    case KEY_DOT:       return PLAT_KEY_PERIOD;

    case KEY_UP:        return PLAT_KEY_UP;
    case KEY_DOWN:      return PLAT_KEY_DOWN;
    case KEY_LEFT:      return PLAT_KEY_LEFT;
    case KEY_RIGHT:     return PLAT_KEY_RIGHT;

    case KEY_LEFTSHIFT:  return PLAT_KEY_LEFT_SHIFT;
    case KEY_RIGHTSHIFT: return PLAT_KEY_RIGHT_SHIFT;
    case KEY_LEFTCTRL:   return PLAT_KEY_LEFT_CONTROL;
    case KEY_RIGHTCTRL:  return PLAT_KEY_RIGHT_CONTROL;
    case KEY_LEFTALT:    return PLAT_KEY_LEFT_ALT;
    case KEY_RIGHTALT:   return PLAT_KEY_RIGHT_ALT;

    /* Cancel is the only Escape this keyboard has. */
    case KEY_F4:  return PLAT_KEY_ESCAPE;
    case KEY_ESC: return PLAT_KEY_ESCAPE;

    case KEY_F1:  return PLAT_KEY_F1;
    case KEY_F2:  return PLAT_KEY_F2;
    case KEY_F3:  return PLAT_KEY_F3;
    case KEY_F10: return PLAT_KEY_F10;
    case KEY_F11: return PLAT_KEY_F11;
    case KEY_F12: return PLAT_KEY_F12;
    default:      return -1;
    }
}

/* Printable character for the chat line, honouring shift. */
static int plat_key_to_char(int key)
{
    int shifted = key_state[PLAT_KEY_LEFT_SHIFT]
               || key_state[PLAT_KEY_RIGHT_SHIFT];

    if (key >= 'A' && key <= 'Z')
        return shifted ? key : key - 'A' + 'a';
    if (key >= '0' && key <= '9')
        return key;
    if (key == PLAT_KEY_SPACE)
        return ' ';
    if (key == PLAT_KEY_MINUS)
        return shifted ? '_' : '-';
    if (key == PLAT_KEY_PERIOD)
        return '.';
    if (key == PLAT_KEY_COMMA)
        return ',';
    return 0;
}

static void handle_key(int code, int pressed)
{
    int key = evkey_to_plat(code);

    if (key < 0 || key >= PLAT_MAX_KEYS)
        return;

    key_state[key] = pressed ? 1 : 0;
    push_event(pressed ? PLAT_EV_KEY_DOWN : PLAT_EV_KEY_UP, key);

    if (!pressed)
        return;

    /*
     * The two clicks. Edge-triggered on press: holding Ctrl should break
     * one block, not one per frame.
     */
    if (key == PLAT_KEY_LEFT_CONTROL || key == PLAT_KEY_RIGHT_CONTROL)
        push_event(PLAT_EV_ACTION, PLAT_ACTION_BREAK);
    else if (key == PLAT_KEY_RIGHT_ALT)
        push_event(PLAT_EV_ACTION, PLAT_ACTION_PLACE);
    else if (key == PLAT_KEY_LEFT_ALT)
        push_event(PLAT_EV_ACTION, PLAT_ACTION_PICK);

    /* The jog dial is the natural item selector on this hardware. */
    else if (code == KEY_F7)
        push_event(PLAT_EV_ITEM_PREV, 0);
    else if (code == KEY_F8)
        push_event(PLAT_EV_ITEM_NEXT, 0);

    {
        int ch = plat_key_to_char(key);
        if (ch)
            push_event(PLAT_EV_CHAR, ch);
    }
}

/* ── touchscreen ────────────────────────────────────────────────────── */

static void pen_sync(void)
{
    if (!pen_pending)
        return;
    pen_pending = 0;

    if (!pen_down) {
        pen_have_origin = 0;
        return;
    }

    if (!pen_have_origin) {
        /* First contact only latches. Emitting a delta here is what
         * would make the view snap to the touch point. */
        pen_last_x = pen_x;
        pen_last_y = pen_y;
        pen_have_origin = 1;
        return;
    }

    look_dx += (pen_x - pen_last_x) / look_divisor;
    look_dy += (pen_y - pen_last_y) / look_divisor;
    pen_last_x = pen_x;
    pen_last_y = pen_y;
}

static void handle_input_event(const struct input_event *ev)
{
    switch (ev->type) {
    case EV_KEY:
        if (ev->code == BTN_TOUCH) {
            pen_down = ev->value != 0;
            if (!pen_down)
                pen_have_origin = 0;     /* pen-up is NOT a click */
            pen_pending = 1;
        } else if (ev->value != 2) {     /* 2 = autorepeat, ignore */
            handle_key(ev->code, ev->value);
        }
        break;

    case EV_ABS:
        if (ev->code == ABS_X) { pen_x = ev->value; pen_pending = 1; }
        else if (ev->code == ABS_Y) { pen_y = ev->value; pen_pending = 1; }
        break;

    case EV_SYN:
        pen_sync();
        break;

    default:
        break;
    }
}

void plat_poll(void)
{
    struct input_event ev;
    int i;

    for (i = 0; i < input_count; i++) {
        for (;;) {
            ssize_t n = read(input_fds[i], &ev, sizeof(ev));
            if (n != (ssize_t)sizeof(ev))
                break;
            handle_input_event(&ev);
        }
    }
}

/* ── setup / teardown ───────────────────────────────────────────────── */

static void tty_set_graphics(int on)
{
    if (tty_fd < 0)
        return;
    if (ioctl(tty_fd, KDSETMODE, on ? KD_GRAPHICS : KD_TEXT) == 0)
        tty_graphics = on;
}

void plat_shutdown(void)
{
    int i;

    /*
     * Put the panel back on page 0 and undo the doubled virtual
     * resolution, while the fd is still open. Unconditional on purpose:
     * doing this only on a clean exit is how a crash leaves a device
     * that looks bricked.
     */
    if (fb_fd >= 0 && orig_var_valid) {
        struct fb_var_screeninfo var = orig_var;

        var.xoffset = 0;
        var.yoffset = 0;
        if (ioctl(fb_fd, FBIOPAN_DISPLAY, &var) < 0)
            ioctl(fb_fd, FBIOPUT_VSCREENINFO, &var);
        else if (page_flip)
            ioctl(fb_fd, FBIOPUT_VSCREENINFO, &var);
        page_flip = 0;
    }

    tty_set_graphics(0);
    if (tty_fd >= 0) {
        close(tty_fd);
        tty_fd = -1;
    }
    if (fb_mem != MAP_FAILED) {
        munmap(fb_mem, fb_mem_size);
        fb_mem = MAP_FAILED;
    }
    if (fb_fd >= 0) {
        close(fb_fd);
        fb_fd = -1;
    }
    for (i = 0; i < input_count; i++)
        close(input_fds[i]);
    input_count = 0;
}

static void on_fatal_signal(int sig)
{
    /* Without this a crash leaves the panel in graphics mode with no
     * console, i.e. a device that looks bricked and is not. */
    plat_shutdown();
    _exit(128 + sig);
}

static void open_inputs(void)
{
    char path[64];
    int i;

    for (i = 0; i < 8 && input_count < MAX_INPUT_FDS; i++) {
        int fd;
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0)
            continue;
        /* Grabbing keeps keystrokes out of the console underneath. Not
         * fatal if it fails -- the game still reads events. */
        ioctl(fd, EVIOCGRAB, 1);
        input_fds[input_count++] = fd;
    }
}

static int setup_framebuffer(void)
{
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;

    fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) {
        perror("plat_fb: /dev/fb0");
        return -1;
    }
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &var) < 0 ||
        ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix) < 0) {
        perror("plat_fb: screeninfo");
        return -1;
    }

    /* Keep a pristine copy before we ask for anything. */
    orig_var = var;
    orig_var_valid = 1;

    fb_width = var.xres;
    fb_height = var.yres;
    fb_bpp = var.bits_per_pixel;

    if (fb_bpp != 16) {
        fprintf(stderr, "plat_fb: need a 16bpp framebuffer, got %d\n", fb_bpp);
        return -1;
    }

    /* Ask for two pages if the driver can pan. */
    if (fix.ypanstep > 0) {
        var.yres_virtual = var.yres * 2;
        var.xoffset = 0;
        var.yoffset = 0;
        if (ioctl(fb_fd, FBIOPUT_VSCREENINFO, &var) == 0 &&
            var.yres_virtual >= var.yres * 2)
            page_flip = 1;
        /* Re-read: the driver may have adjusted the line length. */
        ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix);
    }

    fb_line_len = fix.line_length;
    page_bytes = fb_line_len * fb_height;
    fb_mem_size = page_flip ? (size_t)page_bytes * 2 : fix.smem_len;
    if (fb_mem_size < (size_t)page_bytes)
        fb_mem_size = page_bytes;

    fb_mem = mmap(NULL, fb_mem_size, PROT_READ | PROT_WRITE,
                  MAP_SHARED, fb_fd, 0);
    if (fb_mem == MAP_FAILED) {
        perror("plat_fb: mmap");
        return -1;
    }

    memset(fb_mem, 0, fb_mem_size);
    return 0;
}

int plat_init(const uint8_t *palette)
{
    const char *env;
    int i;

    time_base = now_seconds();

    for (i = 0; i < 256; i++) {
        unsigned r = palette[i * 3 + 0];
        unsigned g = palette[i * 3 + 1];
        unsigned b = palette[i * 3 + 2];
        palette16[i] = (uint16_t)(((r & 0xf8) << 8) |
                                  ((g & 0xfc) << 3) |
                                  ( b >> 3));
    }

    if (setup_framebuffer() != 0) {
        plat_shutdown();
        return -1;
    }

    tty_fd = open("/dev/tty0", O_RDWR);
    tty_set_graphics(1);

    open_inputs();
    if (input_count == 0)
        fprintf(stderr, "plat_fb: warning, no /dev/input/event* opened\n");

    env = getenv("OTCRAFT_LOOK_DIVISOR");
    if (env && atoi(env) > 0)
        look_divisor = atoi(env);

    signal(SIGINT, on_fatal_signal);
    signal(SIGTERM, on_fatal_signal);
    signal(SIGSEGV, on_fatal_signal);

    return 0;
}

/* ── present ────────────────────────────────────────────────────────── */

static double t_blit_acc, t_flip_acc;

void plat_present_timing(double *blit, double *flip)
{
    if (blit) *blit = t_blit_acc;
    if (flip) *flip = t_flip_acc;
}

void plat_present(const uint8_t *indices)
{
    uint8_t *base;
    int y;
    double t0;

    if (fb_mem == MAP_FAILED)
        return;

    t0 = now_seconds();

    base = (uint8_t *)fb_mem + (page_flip ? back_page * page_bytes : 0);

    if (SOFT_W * 2 == fb_width && SOFT_H * 2 == fb_height) {
        /*
         * The expected path: exact 2x pixel doubling, 320x240 -> 640x480.
         * Each source pixel becomes a pair of identical shorts written as
         * ONE 32-bit store, which halves the number of writes into
         * framebuffer memory -- and those writes, not the palette lookup,
         * are what this loop is limited by.
         *
         * The two output rows are written as SEPARATE sequential passes,
         * which looks wasteful (the palette is looked up twice per source
         * pixel) and is not.
         *
         * w100fb maps the framebuffer write-combining -- see
         * w100fb_mmap() in piko's modules/w100/. Write combining only
         * merges writes into bus bursts while they stay sequential.
         * Interleaving `d0[x]` and `d1[x]` in one loop alternates between
         * two addresses a whole scanline (1280 bytes) apart, so the write
         * buffer has to flush on every single pair and every store goes
         * to the bus alone. Two clean streams let it batch instead.
         *
         * Recomputing the palette lookup is a cached L1 hit; a stalled
         * store to the w100 across the PXA static bus measured ~152
         * cycles. Trading the former for fewer of the latter is not close.
         */
        for (y = 0; y < SOFT_H; y++) {
            const uint8_t *src = indices + y * SOFT_W;
            uint32_t *d0 = (uint32_t *)(base + (y * 2) * fb_line_len);
            uint32_t *d1 = (uint32_t *)(base + (y * 2 + 1) * fb_line_len);
            int x;

            for (x = 0; x < SOFT_W; x++) {
                uint32_t c = palette16[src[x]];
                d0[x] = c | (c << 16);
            }
            for (x = 0; x < SOFT_W; x++) {
                uint32_t c = palette16[src[x]];
                d1[x] = c | (c << 16);
            }
        }
    } else {
        /* Anything else: 1:1, centred, black borders. */
        int ox = (fb_width - SOFT_W) / 2;
        int oy = (fb_height - SOFT_H) / 2;

        if (ox < 0) ox = 0;
        if (oy < 0) oy = 0;

        for (y = 0; y < fb_height; y++) {
            uint16_t *drow = (uint16_t *)(base + y * fb_line_len);
            int sy = y - oy;
            int x;

            if (sy < 0 || sy >= SOFT_H) {
                memset(drow, 0, (size_t)fb_width * 2);
                continue;
            }
            for (x = 0; x < fb_width; x++) {
                int sx = x - ox;
                drow[x] = (sx >= 0 && sx < SOFT_W)
                        ? palette16[indices[sy * SOFT_W + sx]] : 0;
            }
        }
    }

    t_blit_acc += now_seconds() - t0;
    t0 = now_seconds();

    if (page_flip) {
        struct fb_var_screeninfo pan;

        if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &pan) == 0) {
            pan.xoffset = 0;
            pan.yoffset = back_page * fb_height;
            if (ioctl(fb_fd, FBIOPAN_DISPLAY, &pan) < 0) {
                fprintf(stderr, "plat_fb: pan failed (%s), "
                        "falling back to direct writes\n", strerror(errno));
                page_flip = 0;
            } else {
                back_page ^= 1;
            }
        } else {
            page_flip = 0;
        }
    }

    t_flip_acc += now_seconds() - t0;
}

/*
 * Not yet run on real hardware. The framebuffer geometry, the pan-based
 * page flip and the ads7846 event stream are all written from the
 * documented behaviour of this board rather than observed here, so the
 * first device run should check, in order: that the panel goes graphical
 * at all, that /proc/interrupts shows the ads7846 IRQ rising on taps,
 * and only then whether the look direction and divisor feel right.
 */
