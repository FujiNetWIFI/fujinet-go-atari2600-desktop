/*
 * The Win32 Controllers window (F9): the two ports' Atari controllers (the
 * CX40 joystick, the CX30 paddle pair, the CX20 driving controller, the CX50
 * keypad) drawn as a player sees them over the six-switch console, the
 * gamepads and the port each drives, and Map mode, in a fixed-size floating
 * tool window. Hidden at start.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <windows.h>

#include "a2600session.h"

/* Show / hide (F9). Created on first use, hidden not destroyed after. */
void a2600_controllers_window_toggle(HWND parent, a2600session *session);
/* Show it (and raise it) if it is not showing. */
void a2600_controllers_window_show(HWND parent, a2600session *session);
int  a2600_controllers_window_visible(void);

/* The gamepad set, the bindings or a controller type changed: refresh. */
void a2600_controllers_window_refresh(void);
