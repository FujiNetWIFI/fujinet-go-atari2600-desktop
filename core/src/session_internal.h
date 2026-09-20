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

    /* cross-thread system-action latch (see a2600session_sysaction_post) */
    pthread_mutex_t sysact_mtx;
    unsigned sysact_pending;

    /* the running configuration */
    a2600session_start_opts opts;
    int  effective_type[2];   /* what Stella attached (AUTO resolved), for the gamepad thread */
    char cart_path[A2600_PATH_MAX];
    int  switch_state[A2600_SW_COUNT];    /* toggling switches: current position */

    /* keys the keyboard currently holds, by target, so a release clears
     * exactly what its press asserted even if the binding changed meanwhile */
    uint32_t held_keysym[A2600_TARGET_COUNT];

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

/* bindings.c */
void bindings_init(struct a2600session *s);

/* audio_sdl.c */
int  audio_start(struct a2600session *s);
void audio_stop(struct a2600session *s);

/* gamepad_sdl.c */
int  gamepad_start(struct a2600session *s);
void gamepad_stop(struct a2600session *s);
/* Called by the gamepad thread with the button/axis state it resolved. */
void session_gamepad_apply(struct a2600session *s, int port, int act, int down);
void session_gamepad_analog(struct a2600session *s, int port, int paddle, int value);

#ifdef __cplusplus
}
#endif

#endif /* A2600_SESSION_INTERNAL_H */
