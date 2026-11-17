/*
 * a2600session's private state. Not installed; only the core/src sources
 * include it. Plain C so the C modules (settings, paths, media, audio,
 * gamepads, bindings) and the C++ session (session.cpp, which owns the
 * StellaHost) share one struct.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef A2600_SESSION_INTERNAL_H
#define A2600_SESSION_INTERNAL_H

#include <pthread.h>
#include <stdint.h>

#include "a2600session.h"

#ifdef __cplusplus
extern "C" {
#endif

#define A2600_PATH_MAX 1024

typedef struct setting_kv {
    char *key;
    char *val;
    struct setting_kv *next;
} setting_kv;

struct a2600session {
    char config_dir[A2600_PATH_MAX];
    char data_dir[A2600_PATH_MAX];
    char carts_dir[A2600_PATH_MAX];
    char stella_dir[A2600_PATH_MAX];   /* <data>/stella/ -- Stella's base dir */
    char settings_file[A2600_PATH_MAX];

    setting_kv *settings;
    pthread_mutex_t settings_mtx;
    int settings_dirty;

    char last_error[256];

    /* ---- FujiNet runtime (fujinet_runtime.c) ---- */
    char fujinet_root[A2600_PATH_MAX];    /* <data>/fujinet */
    char fujinet_config[A2600_PATH_MAX];  /* .../fnconfig.ini */
    char fujinet_sd[A2600_PATH_MAX];      /* .../SD */
    char fujinet_data[A2600_PATH_MAX];    /* .../data */
    char fujinet_lib[A2600_PATH_MAX];     /* resolved libfujinet path, "" until then */
    char fujinet_runtime_src[A2600_PATH_MAX]; /* caller-given pristine tree, or "" */
    char webui_url[64];                   /* http://127.0.0.1:11505/ */
    int  fujinet_running;
    int  fujinet_external;      /* dialling a FujiNet that isn't ours */
    char boip_host[64];         /* where the cartridge dials */
    int  boip_port;
    char boip_endpoint[80];     /* "host:port" for display */

    /* cross-thread system-action latch (see a2600session_sysaction_post) */
    pthread_mutex_t sysact_mtx;
    unsigned sysact_pending;

    /* the running configuration */
    a2600session_start_opts opts;
    int  effective_type[2];   /* what Stella attached (AUTO resolved), for the gamepad thread */
    char cart_path[A2600_PATH_MAX];
    int  switch_state[A2600_SW_COUNT];    /* toggling switches: current position */

    /* keys the keyboard currently holds, by target, so a release clears
     * exactly what its press asserted even if the binding changed meanwhile
     * (UI thread only) */
    uint32_t held_keysym[A2600_TARGET_COUNT];

    /* ---- controllers (input.c) ----
     * What drives each target, one bit per source (A2600_SRC_*), so letting
     * go of one source never releases another's hold. input_mtx covers all of
     * this, the pulse thread's state and the calls that hand the merged result
     * to Stella; it is a leaf lock (nothing below it takes another). */
    pthread_mutex_t input_mtx;
    uint32_t src[A2600_TARGET_COUNT];
    uint32_t sent_acts[2];     /* per port: the actions Stella was last told are
                                * held (bit per a2600_action), after the
                                * opposed pairs cancelled */
    uint8_t  sent_sw[2];       /* Select, Reset as last sent */
    int      analog_val[2][3]; /* per port: paddle A, paddle B, driving: the
                                * position last handed to Stella */
    uint32_t analog_owner[2][3]; /* the source that wrote it last (0 = none) */
    pthread_t pulse_thread;
    pthread_cond_t pulse_cond;
    int pulse_thread_up, pulse_quit;
    uint64_t pulse_until[A2600_TARGET_COUNT]; /* machine frame a pulse ends at */

    /* ---- gamepads (gamepad_sdl.c) ---- */
    /* the pad generation, carried across gamepad_stop/start so a restart of
     * the session never replays an old toast */
    unsigned pad_gen_base;
    /* a pad thread has run before: its first scan finds the same pads again,
     * which is no hot-plug (no toast, no generation change) */
    int pad_restarted;
    /* the last hot-plug event, for a frontend toast */
    pthread_mutex_t pad_event_mtx;
    char pad_event[128];

    void *host;               /* StellaHost*, session.cpp only */
    void *audio;              /* audio_sdl.c state, NULL until started */
    void *gamepad;            /* gamepad_sdl.c state, NULL until started */
    void *debugger;           /* a2600debug, lazily created */
    int running;
};

void settings_init(struct a2600session *s);
void settings_free_all(struct a2600session *s);

int paths_init(struct a2600session *s, const char *config_dir,
               const char *data_dir);
/* Locate libfujinet and provision the runtime tree (fnconfig.ini + data/ +
 * SD/) into <data>/fujinet on first run. Returns 0, or -1 if no runtime is
 * available (not fatal to the session -- see fujinet_start). */
int paths_provision_fujinet(struct a2600session *s);

void session_set_error(struct a2600session *s, const char *fmt, ...);

/* fujinet_runtime.c */
int  fujinet_start(struct a2600session *s);
void fujinet_stop(struct a2600session *s);
/* Block (up to timeout_ms) until the BoIP port accepts, so the emulator's
 * first dial-out finds the listener. Returns 0 once up, -1 on timeout. */
int  fujinet_wait_for_boip(struct a2600session *s, int timeout_ms);

/* Who is holding an input down (the bits of src[]): the keyboard, the
 * Controllers window's mouse, a timed pulse (a wheel notch, a menu's Select),
 * and for each gamepad (by the stable owner number its slot was given, 0 to
 * A2600_MAX_PADS - 1) its buttons and its stick / D-pad separately. So letting
 * go of one never releases another, even two pads or two buttons bound to the
 * one key. */
#define A2600_MAX_PADS 8
#define A2600_SRC_KEYBOARD   (1u << 0)
#define A2600_SRC_POINTER    (1u << 1)
#define A2600_SRC_PULSE      (1u << 2)
#define A2600_SRC_PAD(owner)   (1u << (3 + (owner)))
#define A2600_SRC_STICK(owner) (1u << (3 + A2600_MAX_PADS + (owner)))

/* bindings.c */
void bindings_init(struct a2600session *s);

/* audio_sdl.c */
int  audio_start(struct a2600session *s);
void audio_stop(struct a2600session *s);

/* gamepad_sdl.c */
int  gamepad_start(struct a2600session *s);
void gamepad_stop(struct a2600session *s);
/* Called by the gamepad thread on a connect/disconnect. */
void session_gamepad_event(struct a2600session *s, const char *text);

/* session.cpp: hand one resolved input to Stella. Safe from any thread (it
 * only queues an event); a no-op while the machine is stopped, which input.c
 * accounts for by re-sending what is held when the machine starts. */
void session_machine_event(struct a2600session *s, int port, int act, int down);
void session_machine_switch(struct a2600session *s, int sw, int down); /* SELECT, RESET */
void session_machine_analog(struct a2600session *s, int port, int paddle, int value);

#ifdef __cplusplus
}
#endif

#endif /* A2600_SESSION_INTERNAL_H */
