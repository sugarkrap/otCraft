/*
 * plat.h -- platform layer, replacing GLFW.
 *
 * Two backends: plat_fb.c drives /dev/fb0 and evdev on the Zaurus,
 * plat_sdl.c (host) exists only so the game can be run on the dev
 * machine. The interface is deliberately shaped like the small part of
 * GLFW that Craft actually used -- a polled key array, an event queue
 * and a relative look delta -- so porting main.c is mostly a rename.
 *
 * Key codes match GLFW 3's numbering, and letters are their ASCII
 * uppercase, so `glfwGetKey(window, 'W')` becomes `plat_key_down('W')`
 * with no table in between.
 */

#ifndef SOFT_PLAT_H
#define SOFT_PLAT_H

#include <stdint.h>

/* ── key codes (GLFW 3 numbering) ───────────────────────────────────── */

#define PLAT_KEY_SPACE         32
#define PLAT_KEY_APOSTROPHE    39
#define PLAT_KEY_COMMA         44
#define PLAT_KEY_MINUS         45
#define PLAT_KEY_PERIOD        46
#define PLAT_KEY_SLASH         47
/* 48..57 are '0'..'9', 65..90 are 'A'..'Z' */
#define PLAT_KEY_SEMICOLON     59
#define PLAT_KEY_EQUAL         61
#define PLAT_KEY_BACKSLASH     92
#define PLAT_KEY_GRAVE         96

#define PLAT_KEY_ESCAPE        256
#define PLAT_KEY_ENTER         257
#define PLAT_KEY_TAB           258
#define PLAT_KEY_BACKSPACE     259
#define PLAT_KEY_INSERT        260
#define PLAT_KEY_DELETE        261
#define PLAT_KEY_RIGHT         262
#define PLAT_KEY_LEFT          263
#define PLAT_KEY_DOWN          264
#define PLAT_KEY_UP            265
#define PLAT_KEY_PAGE_UP       266
#define PLAT_KEY_PAGE_DOWN     267
#define PLAT_KEY_HOME          268
#define PLAT_KEY_END           269

#define PLAT_KEY_F1            290
#define PLAT_KEY_F2            291
#define PLAT_KEY_F3            292
#define PLAT_KEY_F4            293
#define PLAT_KEY_F5            294
#define PLAT_KEY_F10           299
#define PLAT_KEY_F11           300
#define PLAT_KEY_F12           301

#define PLAT_KEY_LEFT_SHIFT    340
#define PLAT_KEY_LEFT_CONTROL  341
#define PLAT_KEY_LEFT_ALT      342
#define PLAT_KEY_RIGHT_SHIFT   344
#define PLAT_KEY_RIGHT_CONTROL 345
#define PLAT_KEY_RIGHT_ALT     346

#define PLAT_MAX_KEYS          512

/* ── synthetic actions ──────────────────────────────────────────────── */

/*
 * The two mouse buttons, which this device does not have.
 *
 * The touchscreen deliberately does NOT click: it is the look control,
 * and a tap that also broke a block would make aiming unusable. So the
 * clicks move to keys, per the ROM's conventions:
 *
 *   Ctrl    -> break block (what a left click did)
 *   AltGr   -> place block (what a right click did)
 *
 * Both are reported as edge-triggered events rather than held state,
 * matching how Craft consumed on_left_click()/on_right_click().
 */
#define PLAT_ACTION_BREAK 1
#define PLAT_ACTION_PLACE 2
#define PLAT_ACTION_PICK  3      /* middle click: copy the block looked at */

/* ── events ─────────────────────────────────────────────────────────── */

enum {
    PLAT_EV_NONE = 0,
    PLAT_EV_KEY_DOWN,
    PLAT_EV_KEY_UP,
    PLAT_EV_CHAR,          /* value is a codepoint, for the chat line */
    PLAT_EV_ACTION,        /* value is a PLAT_ACTION_* */
    PLAT_EV_ITEM_NEXT,     /* jog dial down */
    PLAT_EV_ITEM_PREV,     /* jog dial up */
    PLAT_EV_QUIT
};

typedef struct {
    int type;
    int value;
} plat_event;

/* ── lifecycle ──────────────────────────────────────────────────────── */

/*
 * `palette` is the 768-byte table from mkassets.py; the backend builds
 * whatever lookup its output format needs (RGB565 on this panel).
 * Returns 0 on success.
 */
int  plat_init(const uint8_t *palette);
void plat_shutdown(void);

/* Push a 320x240 index buffer to the display. */
void plat_present(const uint8_t *indices);

/* Drain the input devices into the queue and the key array. */
void plat_poll(void);

int  plat_poll_event(plat_event *ev);   /* 0 when the queue is empty */
int  plat_key_down(int key);

/*
 * Accumulated stylus motion since the last call, in look units, and
 * zeroed by reading. See plat_fb.c for why this is relative and why pen
 * contact produces no jump.
 */
void plat_get_look_delta(int *dx, int *dy);

/* Monotonic seconds since plat_init(). */
double plat_time(void);

#endif /* SOFT_PLAT_H */
