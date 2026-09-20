/*
 * A2600KeypadWindow -- both keyboard controllers side by side, the console
 * switches, and Map mode for rebinding any control to a key or a gamepad
 * button.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "a2600session.h"

/* Toggles visibility: shows the singleton window (creating it on first
 * call), or hides it if already showing. `parent` is only used the first
 * time, to set the transient-for relationship. */
void a2600_keypad_window_toggle(GtkWindow *parent, a2600session *session);
gboolean a2600_keypad_window_is_visible(void);
