/*
 * The Win32 keypad panel: both keyboard controllers, the console switches
 * and the Map row, in a fixed-size tool window.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <windows.h>

#include "a2600session.h"

/* Show / hide (F9). Created on first use, hidden not destroyed after. */
void a2600_keypad_window_toggle(HWND parent, a2600session *session);

/* Give the panel first refusal on a message from the main loop, so its
 * keys work whatever child has the focus. Returns 1 if consumed. */
int a2600_keypad_pretranslate(MSG *msg);
