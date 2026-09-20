/*
 * a2600session -- the toolkit-agnostic desktop session for FujiNet Go
 * Atari 2600.
 *
 * Owns the emulator (Stella, on its own thread -- see core/stella/
 * StellaHost.hxx), the FujiNet cartridge's link, the SDL audio and gamepad
 * backends, the in-process FujiNet runtime, the shared settings store, the
 * remappable key/pad bindings and the media path layout. Frontends (GTK4,
 * Qt6, AppKit, Win32) drive this API and do only windowing, painting and
 * event translation. A frontend that needs something which is not one of
 * those three things belongs here instead.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef A2600SESSION_H
#define A2600SESSION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The TIA paints 160 pixels per line; the number of lines is the ROM's
 * (NTSC ~228-262, PAL up to 312). Frontends show it at 4:3 with 2:1 pixels. */
#define A2600SESSION_FB_WIDTH      160
#define A2600SESSION_FB_MAX_HEIGHT 320

/* FujiNet's BoIP listener and its web admin UI. High ports of this app's own
 * so a standalone fujinet-pc, or a sibling FujiNet Go app, never collides:
 * ADAM uses 65216/65214, Apple II 1985/8000, CoCo 65504, MSX 65505/64003,
 * Intellivision 65503/64003, Astrocade 11500/11501, ColecoVision
 * 11502/11503, and the 2600 work's own MAME/Stella dev harness uses 9995.
 *
 * Direction: FujiNet LISTENS and Stella's FujiNet cartridge dials in, as on
 * CoCo, MSX, Intellivision, Astrocade and ColecoVision. That decides startup
 * ordering -- see a2600session_start. */
#define A2600SESSION_BOIP_PORT  11504
#define A2600SESSION_WEBUI_PORT 11505

/* The host audio device: Stella's own resampler converts the TIA's
 * one-sample-per-scanline stream (31 440 Hz NTSC) to this. */
#define A2600SESSION_AUDIO_RATE 48000

/* The accent colour every frontend uses for its highlights (keypad keys
 * held, the Map target, the debugger's current line, the FujiNet status
 * dot) -- and the icon's background. */
#define A2600SESSION_ACCENT_RGB 0xFFA645

typedef struct a2600session a2600session;
typedef struct a2600debug a2600debug;

/* All members optional (NULL = default).
 *  config_dir:  default $XDG_CONFIG_HOME/fujinet-go-atari2600
 *  data_dir:    default $XDG_DATA_HOME/fujinet-go-atari2600
 *  fujinet_lib: path to libfujinet.so/.dylib/.dll; default searches
 *               $FUJINET_LIB, the executable's directory, the install
 *               libdir, then tools/fujinet/work/out. "" disables FujiNet.
 *  fujinet_runtime_src: directory holding the pristine fnconfig.ini + data/
 *               + SD/ used to provision the user's runtime tree on first
 *               start (a macOS app passes its bundle's runtime dir). */
typedef struct {
    const char *config_dir;
    const char *data_dir;
    const char *fujinet_lib;
    const char *fujinet_runtime_src;
} a2600session_paths;

a2600session *a2600session_new(const a2600session_paths *paths);
void a2600session_free(a2600session *s);

/* ---- settings (shared INI; one store for every frontend of this target) --- */
int         a2600session_get_int(a2600session *s, const char *key, int def);
void        a2600session_set_int(a2600session *s, const char *key, int value);
const char *a2600session_get_str(a2600session *s, const char *key,
                                 const char *def);
void        a2600session_set_str(a2600session *s, const char *key,
                                 const char *value);
void        a2600session_settings_flush(a2600session *s);

/* ---- machine options ------------------------------------------------------ */

/* TV format. AUTO lets Stella's frame-layout detection decide per ROM. */
typedef enum {
    A2600_TV_AUTO = 0, A2600_TV_NTSC, A2600_TV_PAL, A2600_TV_PAL60,
    A2600_TV_SECAM, A2600_TV_COUNT
} a2600_tv_format;

/* What is plugged into a controller port. AUTO is Stella's own choice from
 * its ROM database and controller detector; the others force a type. */
typedef enum {
    A2600_CTRL_AUTO = 0, A2600_CTRL_JOYSTICK, A2600_CTRL_PADDLES,
    A2600_CTRL_DRIVING, A2600_CTRL_KEYPAD, A2600_CTRL_COUNT
} a2600_ctrl_type;

/* Human-readable, NULL past the end (for filling combo boxes). */
const char *a2600_tv_format_name(int f);
const char *a2600_ctrl_type_name(int t);

/* ---- lifecycle ------------------------------------------------------------ */
typedef struct {
    const char *cart_path;    /* cartridge image; NULL boots the CONFIG client */
    int tv_format;            /* a2600_tv_format */
    int port_type[2];         /* a2600_ctrl_type per port (0 = left) */
    int analog_joystick;      /* gamepad sticks may drive a joystick */
    int analog_paddle;        /* ... a paddle */
    int analog_driving;       /* ... a driving controller */
    int enable_fujinet;       /* start the in-process FujiNet runtime */
    int enable_audio;         /* open the SDL audio device */
    int enable_gamepad;       /* start the SDL gamepad thread */
} a2600session_start_opts;

/* Fills opts from the settings store (keys: cart tv_format port0_type
 * port1_type analog_joystick analog_paddle analog_driving enable_fujinet
 * enable_audio enable_gamepad). */
void a2600session_default_opts(a2600session *s, a2600session_start_opts *opts);

/* Starts FujiNet (if enabled) and then the emulator.
 *
 * The order is not arbitrary: FujiNet listens and the cartridge dials in, so
 * the listener has to exist before the machine's first transaction or the
 * CONFIG client boots reporting no link. start() brings FujiNet up first and
 * waits briefly for the port.
 *
 * Returns 0, or -1 with a2600session_last_error() set. FujiNet failing to
 * start is NOT fatal: the machine boots with the cartridge reporting the
 * link down, which is far more useful than refusing to run. */
int  a2600session_start(a2600session *s, const a2600session_start_opts *opts);
void a2600session_stop(a2600session *s);
int  a2600session_is_running(const a2600session *s);
const char *a2600session_last_error(const a2600session *s);

/* ---- cartridges ------------------------------------------------------------
 * Open a local cartridge image directly (Stella autodetects the mapper).
 * The path is remembered in the "cart" setting. Returns 0 or -1 + error. */
int  a2600session_load_cart(a2600session *s, const char *path);
const char *a2600session_cart_path(const a2600session *s);
/* Eject: back to FujiNet's CONFIG client (the hardware-faithful behaviour
 * of a FujiNet cartridge; there is no "no cartridge" state worth having). */
int  a2600session_eject(a2600session *s);
/* Recreate the console from the CONFIG client. After a network boot the
 * cartridge is serving the game and the console's RESET switch cannot undo
 * that -- on hardware this is a power cycle. Bound to Escape by default. */
int  a2600session_reboot_to_config(a2600session *s);

/* ---- video ---------------------------------------------------------------
 * copy_frame copies the latest frame into dst (up to FB_WIDTH*FB_MAX_HEIGHT
 * uint32 XRGB8888 pixels, 160 per line) iff its serial differs from
 * *serial_inout, updates it, writes the frame's line count into *height and
 * returns 1; returns 0 when unchanged, leaving dst alone. Pass 0 to force a
 * copy (e.g. the first paint after a window map). */
int  a2600session_copy_frame(a2600session *s, uint32_t *dst, int *height,
                             uint64_t *serial_inout);
/* 60 or 50: the current ROM's refresh rate. */
int  a2600session_refresh_rate(a2600session *s);

/* Feed the UI's frame-clock ticks (CLOCK_MONOTONIC ns). While a steady ~60 Hz
 * stream arrives the emulator phase-locks one frame per tick; otherwise it
 * paces on the wall clock. A frontend with no frame clock simply never
 * calls this and loses nothing but the phase lock. */
void a2600session_notify_vsync(a2600session *s, int64_t frame_time_ns);

/* ---- audio ---------------------------------------------------------------
 * Owned by the session (SDL) when opts.enable_audio was set. A frontend
 * that wants the device itself can pull interleaved stereo float frames at
 * A2600SESSION_AUDIO_RATE instead. Returns the frames written (silence is
 * written for any shortfall). */
int  a2600session_render_audio(a2600session *s, float *out, int nframes);
void a2600session_set_volume(a2600session *s, int percent);

/* ---- input ---------------------------------------------------------------
 * Every control the machine has, flattened into one target index so the
 * bindings table, the keypad window and the settings store can all name
 * them. Per port: the joystick, both paddles, the driving controller and
 * the keypad; then the console switches; then the session's own actions. */
typedef enum {
    A2600_ACT_JOY_UP = 0, A2600_ACT_JOY_DOWN, A2600_ACT_JOY_LEFT,
    A2600_ACT_JOY_RIGHT, A2600_ACT_JOY_FIRE,
    A2600_ACT_PADDLE_A_DEC, A2600_ACT_PADDLE_A_INC, A2600_ACT_PADDLE_A_FIRE,
    A2600_ACT_PADDLE_B_DEC, A2600_ACT_PADDLE_B_INC, A2600_ACT_PADDLE_B_FIRE,
    A2600_ACT_DRIVE_CCW, A2600_ACT_DRIVE_CW, A2600_ACT_DRIVE_FIRE,
    A2600_ACT_KEY_1, A2600_ACT_KEY_2, A2600_ACT_KEY_3,
    A2600_ACT_KEY_4, A2600_ACT_KEY_5, A2600_ACT_KEY_6,
    A2600_ACT_KEY_7, A2600_ACT_KEY_8, A2600_ACT_KEY_9,
    A2600_ACT_KEY_STAR, A2600_ACT_KEY_0, A2600_ACT_KEY_POUND,
    A2600_ACT_PER_PORT
} a2600_action;

typedef enum {
    A2600_SW_SELECT = 0, A2600_SW_RESET, A2600_SW_COLOR_BW,
    A2600_SW_LEFT_DIFF, A2600_SW_RIGHT_DIFF,
    A2600_SW_COUNT
} a2600_switch;

typedef enum {
    A2600_SYSACT_REBOOT_CONFIG = 0,   /* Escape by default */
    A2600_SYSACT_PAUSE,               /* stop in the debugger */
    A2600_SYSACT_COUNT
} a2600_sysaction;

#define A2600_TARGET_PORT(port, act) ((port) * A2600_ACT_PER_PORT + (act))
#define A2600_TARGET_SWITCH(sw)      (2 * A2600_ACT_PER_PORT + (sw))
#define A2600_TARGET_SYSACT(sa)      (2 * A2600_ACT_PER_PORT + A2600_SW_COUNT + (sa))
#define A2600_TARGET_COUNT           (2 * A2600_ACT_PER_PORT + A2600_SW_COUNT + A2600_SYSACT_COUNT)

/* Apply one control directly, the way the on-screen keypad window does.
 * `down` is press/release. The momentary console switches (Select, Reset)
 * follow down; Color/B&W and the difficulty switches TOGGLE on press. */
void a2600session_press(a2600session *s, int target, int down);
/* Analog input for a paddle (A: 0, B: 1) or the driving controller
 * (paddle -1) on `port`: value in -32767..32767. Continuous; the last value
 * per frame wins. */
void a2600session_analog(a2600session *s, int port, int paddle, int value);
/* Momentary console switches as a whole: press, hold for `frames`, release.
 * What the Select/Reset menu items do. */
void a2600session_switch_pulse(a2600session *s, int sw);
/* Current position of a toggling switch (Color/B&W: 1 = colour; difficulty:
 * 1 = A). */
int  a2600session_switch_get(a2600session *s, int sw);
void a2600session_switch_set(a2600session *s, int sw, int on);
void a2600session_sysaction(a2600session *s, int sysact);
/* Release everything the keyboard holds (focus loss). */
void a2600session_release_all(a2600session *s);

/* ---- keyboard translation ------------------------------------------------
 * keysym is an X11/xkb keysym (== a GDK keyval; Qt, Win32 and AppKit map
 * through the HID tables below), so one bindings table serves every
 * frontend. Returns 1 if the key drives a target (and has been applied /
 * released), 0 if it should be ignored. System actions are reported through
 * a2600session_key_sysaction instead and left to the frontend. */
int  a2600session_key(a2600session *s, uint32_t keysym, int down);
/* The system action a keysym is bound to, or -1. */
int  a2600session_key_sysaction(a2600session *s, uint32_t keysym);

/* Non-printing keys in the keysym space the frontends translate to. */
enum {
    A2600_KEYSYM_NONE = 0,
    A2600_KEYSYM_UP = 0xff52, A2600_KEYSYM_DOWN = 0xff54,
    A2600_KEYSYM_LEFT = 0xff51, A2600_KEYSYM_RIGHT = 0xff53,
    A2600_KEYSYM_ESCAPE = 0xff1b, A2600_KEYSYM_RETURN = 0xff0d,
    A2600_KEYSYM_BACKSPACE = 0xff08, A2600_KEYSYM_TAB = 0xff09,
    A2600_KEYSYM_SPACE = 0x20,
    A2600_KEYSYM_F1 = 0xffbe, A2600_KEYSYM_F2, A2600_KEYSYM_F3, A2600_KEYSYM_F4,
    A2600_KEYSYM_F5, A2600_KEYSYM_F6, A2600_KEYSYM_F7, A2600_KEYSYM_F8,
    A2600_KEYSYM_F9, A2600_KEYSYM_F10, A2600_KEYSYM_F11, A2600_KEYSYM_F12,
    A2600_KEYSYM_LSHIFT = 0xffe1, A2600_KEYSYM_RSHIFT = 0xffe2,
    A2600_KEYSYM_LCTRL = 0xffe3, A2600_KEYSYM_RCTRL = 0xffe4,
    A2600_KEYSYM_LALT = 0xffe9, A2600_KEYSYM_RALT = 0xffea,
    A2600_KEYSYM_KP_0 = 0xffb0, A2600_KEYSYM_KP_1, A2600_KEYSYM_KP_2,
    A2600_KEYSYM_KP_3, A2600_KEYSYM_KP_4, A2600_KEYSYM_KP_5, A2600_KEYSYM_KP_6,
    A2600_KEYSYM_KP_7, A2600_KEYSYM_KP_8, A2600_KEYSYM_KP_9,
    A2600_KEYSYM_KP_ENTER = 0xff8d, A2600_KEYSYM_KP_MULTIPLY = 0xffaa,
    A2600_KEYSYM_KP_DIVIDE = 0xffaf, A2600_KEYSYM_KP_PERIOD = 0xffae
};
/* Native key codes for the platforms whose toolkits do not deliver keysyms:
 * Windows scan code (set 1, with the E0 flag), Linux evdev code (GTK/Qt
 * keycode minus 8) and macOS virtual key code, each mapped to a keysym. 0
 * when unknown. */
uint32_t a2600session_keysym_from_win_scancode(unsigned scancode, int extended);
uint32_t a2600session_keysym_from_evdev(unsigned code);
uint32_t a2600session_keysym_from_macos_keycode(unsigned keycode);
/* Name for a keysym ("F1", "Space", "a", "Keypad 5"); returns length. */
int a2600session_keysym_name(uint32_t keysym, char *dst, int dstsz);

/* ---- remappable bindings --------------------------------------------------
 * Every target can be driven by one keyboard key and one gamepad button.
 * Rebinding STEALS: a key drives exactly one target, because one keystroke
 * doing two things is worse than losing the old binding. Persisted in the
 * settings store under "bindings" as only the entries that differ from the
 * defaults. */
typedef enum {
    A2600_PAD_BTN_NONE = -1,
    A2600_PAD_BTN_SOUTH = 0, A2600_PAD_BTN_EAST, A2600_PAD_BTN_WEST,
    A2600_PAD_BTN_NORTH, A2600_PAD_BTN_BACK, A2600_PAD_BTN_GUIDE,
    A2600_PAD_BTN_START, A2600_PAD_BTN_LEFT_STICK, A2600_PAD_BTN_RIGHT_STICK,
    A2600_PAD_BTN_LEFT_SHOULDER, A2600_PAD_BTN_RIGHT_SHOULDER,
    A2600_PAD_BTN_DPAD_UP, A2600_PAD_BTN_DPAD_DOWN, A2600_PAD_BTN_DPAD_LEFT,
    A2600_PAD_BTN_DPAD_RIGHT,
    A2600_PAD_BTN_LEFT_TRIGGER, A2600_PAD_BTN_RIGHT_TRIGGER,
    A2600_PAD_BTN_COUNT,
    /* Raw joystick bands for devices SDL has no gamepad mapping for (a
     * plain HID adapter): button index, and hat directions (hat*8 + dir). */
    A2600_PAD_BTN_RAW_BASE = 64, A2600_PAD_BTN_RAW_LAST = 127,
    A2600_PAD_HAT_BASE = 192, A2600_PAD_HAT_LAST = 255
} a2600_pad_button;
#define A2600_PAD_HAT_DIRS 8
#define A2600_PAD_BTN_IS_NAMED(b) ((b) >= 0 && (b) < A2600_PAD_BTN_COUNT)
#define A2600_PAD_BTN_IS_RAW(b)   ((b) >= A2600_PAD_BTN_RAW_BASE && (b) <= A2600_PAD_BTN_RAW_LAST)
#define A2600_PAD_BTN_IS_HAT(b)   ((b) >= A2600_PAD_HAT_BASE && (b) <= A2600_PAD_HAT_LAST)

typedef struct {
    uint32_t keysym;     /* 0 = no key */
    int      button;     /* a2600_pad_button, NONE = no gamepad button */
} a2600_binding;

const char   *a2600_target_name(int target);           /* "Left joystick: Up" */
const char   *a2600_target_short_name(int target);     /* "Up" */
a2600_binding a2600session_binding_get(a2600session *s, int target);
/* Bind; the previous holder of the key/button (if any) is described into
 * `stolen` (may be NULL). keysym 0 / button NONE unbinds. */
void a2600session_binding_set_key(a2600session *s, int target, uint32_t keysym,
                                  char *stolen, int stolensz);
void a2600session_binding_set_button(a2600session *s, int target, int button,
                                     char *stolen, int stolensz);
void a2600session_bindings_reset(a2600session *s);
const char *a2600_pad_button_name(int button);
/* The target a keysym / pad button drives, or -1. */
int a2600session_target_for_key(a2600session *s, uint32_t keysym);
int a2600session_target_for_button(a2600session *s, int port, int button);

/* Map mode: after begin(), the next gamepad button pressed on any pad is
 * reported by poll() (returns 1 and the button). cancel() disarms. */
void a2600session_gamepad_capture_begin(a2600session *s);
void a2600session_gamepad_capture_cancel(a2600session *s);
int  a2600session_gamepad_capture_poll(a2600session *s, int *button);

/* ---- controller types (live) ---------------------------------------------
 * Change what is plugged into a port without restarting -- a FujiNet-booted
 * game survives. Persisted as port0_type / port1_type. */
void a2600session_set_port_type(a2600session *s, int port, int type);
int  a2600session_port_type(a2600session *s, int port);
/* What Stella actually attached (for AUTO: the detected type). */
int  a2600session_detected_port_type(a2600session *s, int port);
void a2600session_set_analog(a2600session *s, int joystick, int paddle, int driving);

/* ---- gamepads (SDL, hotplugged; started by a2600session_start) ---------
 * Pads are assigned to ports in connection order unless assigned
 * explicitly. What a pad drives depends on the port's controller type. */
int  a2600session_gamepad_count(a2600session *s);
int  a2600session_gamepad_name(a2600session *s, int idx, char *dst, int dstsz);
void a2600session_gamepad_assign(a2600session *s, int idx, int port); /* -1 = auto */
int  a2600session_gamepad_assignment(a2600session *s, int idx);
int  a2600session_gamepad_effective_port(a2600session *s, int idx);
/* Bumped on every add/remove so a frontend can refresh its lists cheaply. */
unsigned a2600session_gamepad_generation(a2600session *s);

/* ---- cross-thread system actions ------------------------------------------
 * The gamepad thread cannot call into a UI toolkit, so a system action it
 * resolves is posted here and a frontend's timer takes it. */
void a2600session_sysaction_post(a2600session *s, int sysact);
int  a2600session_sysaction_take(a2600session *s, int *out);

/* ---- FujiNet -------------------------------------------------------------*/
int         a2600session_fujinet_running(const a2600session *s);
const char *a2600session_fujinet_webui_url(const a2600session *s);
int         a2600session_fujinet_copy_log(a2600session *s, char *dst, int max);
/* The cartridge's link to FujiNet: 1 up, 0 down, -1 not a FujiNet cart. */
int         a2600session_cart_link_up(a2600session *s);
/* Status text from the cartridge ("connected", "link down", "booted a
 * game; mailbox closed", ...). Returns length. */
int         a2600session_cart_status(a2600session *s, char *dst, int dstsz);
/* 1 once a network boot has swapped a game in over the CONFIG client. */
int         a2600session_cart_booted_game(a2600session *s);

/* ---- media ---------------------------------------------------------------
 * Import a cartridge into FujiNet's SD folder so CONFIG can boot it through
 * the cartridge. Returns 0 and the destination path, or -1 with the error. */
int  a2600session_import_cart_to_sd(a2600session *s, const char *src_path,
                                    char *dest_out, int dest_sz);
/* Routes a dropped file: cartridge images (.a26 .bin .rom .fuji) to the
 * cartridge directory (the returned path is usable with load_cart), disk
 * images to the FujiNet SD folder. */
int  a2600session_import_media(a2600session *s, const char *src_path,
                               char *dest_out, int dest_sz);
int  a2600session_media_is_cartridge(const char *path);

const char *a2600session_config_path(const a2600session *s);
const char *a2600session_data_path(const a2600session *s);
const char *a2600session_carts_path(const a2600session *s);
const char *a2600session_sd_path(const a2600session *s);

/* ---- debugger ------------------------------------------------------------*/
a2600debug *a2600session_debugger(a2600session *s);

#ifdef __cplusplus
}
#endif

#endif /* A2600SESSION_H */
