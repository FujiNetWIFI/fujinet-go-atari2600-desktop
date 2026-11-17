/*
 * input -- private interface to core/src/input.c: every way of holding a
 * controller input (the keyboard, the Controllers window's mouse, a timed
 * pulse, each gamepad's buttons and stick) is a separate source of a target,
 * merged into what Stella sees. Used by session.cpp (the public press,
 * pointer and release calls), bindings.c (the keyboard) and gamepad_sdl.c.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef A2600_INPUT_H
#define A2600_INPUT_H

#include <stdint.h>

#include "session_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

void input_init(struct a2600session *s);
/* Stops the pulse thread and lets every source go. */
void input_stop(struct a2600session *s);
void input_destroy(struct a2600session *s);

/* The machine has (re)started and holds nothing: forget what was last sent
 * and hand it everything still held (a key down across a restart, a pad
 * stick), and drop the pulses (counted in the old machine's frames). */
void input_resync(struct a2600session *s);

/* One source of one target (A2600_TARGET_*): `src` is an A2600_SRC_* bit.
 * A per-port action or a momentary switch is held while any source holds it;
 * a toggling switch flips on each source's press (a repeat is ignored); a
 * system action runs on the press: directly for the window's own pointer, or
 * posted for the UI thread for any other source. */
void input_press(struct a2600session *s, uint32_t src, int target, int down);

/* `src`'s whole hold on one port's actions at once (bit per a2600_action), the
 * pointer's drag. */
void input_port_set(struct a2600session *s, uint32_t src, int port, unsigned mask);

/* Lets go of every target `srcmask` (any of the A2600_SRC_ bits) holds. */
void input_release_sources(struct a2600session *s, uint32_t srcmask);

/* A gamepad that goes away, or moves to another port, or changes what it
 * plays: lets go of its buttons and its stick, and recentres an analog
 * controller it was the last to move. */
void input_pad_clear(struct a2600session *s, int owner);

/* An analog controller's position: axis 0 paddle A, 1 paddle B, 2 the driving
 * controller; value -32767..32767. `src` is a pad's A2600_SRC_PAD(owner) or the
 * pointer. A pad writes only when its value changes, so a still pad never
 * fights a mouse drag. */
void input_analog(struct a2600session *s, uint32_t src, int port, int axis, int value);
/* The pointer lets go of an axis (a drag ends): the paddle keeps its position
 * but no longer belongs to the pointer. */
void input_analog_end(struct a2600session *s, uint32_t src, int port, int axis);
int  input_analog_get(struct a2600session *s, int port, int axis);

/* What Stella sees held on a port (bit per a2600_action, opposed pairs
 * cancelled) and whether a momentary switch is held (Select, Reset). */
unsigned input_held(struct a2600session *s, int port);
int      input_switch_held(struct a2600session *s, int sw);

/* Holds a momentary target (a switch, or a driving notch) for `frames`
 * machine frames, so a program that polls once a frame sees it. */
void input_pulse(struct a2600session *s, int target, int frames);

#ifdef __cplusplus
}
#endif

#endif /* A2600_INPUT_H */
