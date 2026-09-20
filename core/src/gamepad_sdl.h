/*
 * gamepad_sdl -- private interface: the pure helpers unit-tested without
 * hardware, and the start/stop the session calls.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef A2600_GAMEPAD_SDL_H
#define A2600_GAMEPAD_SDL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Direction bits a stick or D-pad resolves to. */
#define A2600_DIR_UP    1
#define A2600_DIR_DOWN  2
#define A2600_DIR_LEFT  4
#define A2600_DIR_RIGHT 8

/* An analog stick as a four-way switch with hysteresis: an axis engages
 * past `enter` and releases below `exit` (both 0..1 of full deflection),
 * so a thumb resting near the threshold cannot flutter. `prev` is the
 * previous result. Pure. */
int a2600_dir_from_stick(float x, float y, int prev, float enter, float exit);

/* Which pad drives `port`: an explicit assignment wins; among the rest,
 * connection order (first pad to port 0, second to port 1). `assign[i]` is
 * pad i's assignment (-1 = automatic). Returns the pad index or -1. Pure. */
int a2600_pad_for_port(const int *assign, int npads, int port);

/* SDL3's SDL_GamepadButton (a uint8_t) to the public numbering, and back.
 * Explicit tables, never a cast, so an SDL reordering cannot desync them. */
int a2600_pad_button_from_sdl(uint8_t button);
int a2600_sdl_button_from_pad(int button);

#ifdef __cplusplus
}
#endif

#endif /* A2600_GAMEPAD_SDL_H */
