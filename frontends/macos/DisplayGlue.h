/*
 * DisplayGlue.h -- what DisplayView.m needs to know about THIS machine: the
 * session's functions, the frame size and the shape of the picture.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "a2600session.h"

#define FN_VIEW      A2600DisplayView
#define FN_SESSION   a2600session
#define FN_FB_W      A2600SESSION_FB_WIDTH
#define FN_FB_H      A2600SESSION_FB_MAX_HEIGHT
#define FN_INITIAL_W 960
#define FN_INITIAL_H 720

static inline int fn_machine_hz(FN_SESSION *s) { return a2600session_refresh_rate(s); }

static inline void fn_notify_vsync(FN_SESSION *s, int64_t ns)
{
    a2600session_notify_vsync(s, ns);
}

static inline int fn_pull_frame(FN_SESSION *s, uint32_t *dst, int *lines, uint64_t *serial)
{
    return a2600session_copy_frame(s, dst, lines, serial);
}

/* The television's 4:3, or the picture's own shape with square pixels. */
static inline double fn_picture_aspect(FN_SESSION *s, int lines, BOOL tv)
{
    (void)s;
    return tv ? (4.0 / 3.0) : (double)FN_FB_W / (double)lines;
}
