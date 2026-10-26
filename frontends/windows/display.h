/*
 * The emulator picture -- see display.c.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef A2600_WIN_DISPLAY_H
#define A2600_WIN_DISPLAY_H

#include <windows.h>

#include "a2600session.h"

/* Creates the picture as a child of parent (size it with MoveWindow) and
 * starts its render thread. One per process. */
HWND a2600_display_create(HWND parent, a2600session *session);
/* Stops the render thread; call before the parent is destroyed. */
void a2600_display_destroy(void);
/* aspect: the television's pixel aspect (else square pixels); smooth:
 * bilinear (else sharp pixels). Safe from any thread. */
void a2600_display_set_options(int aspect, int smooth);
/* "Direct3D 11", "Direct3D 11 (WARP)" or "GDI": what is drawing. */
const char *a2600_display_renderer(void);

#endif
