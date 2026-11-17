/*
 * Qt key events to X11/xkb keysyms.
 *
 * The session's bindings are keyed by the PHYSICAL key -- the unshifted
 * US-layout symbol of the hardware key -- so that Shift (itself a binding on
 * some controllers) cannot change what a key means, and so that Qt's
 * already-shifted key() cannot kill a whole row of keypad keys. The route is
 * the hardware scan code (evdev on Linux) through the session's HID table,
 * with a Qt::Key fallback for anything the table has no usage for.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QKeyEvent>
#include <cstdint>

#include "a2600session.h"

/* Returns the keysym, or 0 when the key has no equivalent worth forwarding. */
uint32_t a2600KeysymFromQt(const QKeyEvent *e);

/* Presses or releases the session's control for a key event, as the main window
 * does; a key bound to a console action posts it to the session's queue, which
 * the main window's timer runs. Auto-repeat is ignored. True when the session
 * took the key. */
bool a2600ForwardKey(a2600session *session, const QKeyEvent *e, bool down);
