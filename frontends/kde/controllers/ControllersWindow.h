/*
 * The Controllers window (F9): what plugs into the Atari 2600's two controller
 * ports, drawn as a player knows it -- the CX40 joystick, the CX30 paddle pair,
 * the CX20 driving controller or the CX50 keypad on each port, following what is
 * plugged in -- over the six-switch console, with the gamepads and the port each
 * plays, and Map mode for rebinding any control. Hidden at start; a fixed-size
 * floating window.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QPointF>
#include <QWidget>

#include "a2600session.h"

namespace a2600ctl {
/* The drawing's metrics, shared with the offscreen test: pixels per layout
 * unit (the layout is 30.6 x 16.6 units, the same for every combination of
 * controllers, so a game swapping one never resizes the window), and the
 * margin round it. */
constexpr double kUnit = 26.0;
constexpr int kMargin = 12;
constexpr int kStageW = 820;               /* ceil(30.6 * kUnit) + 2 * kMargin */
constexpr int kStageH = 456;               /* ceil(16.6 * kUnit) + 2 * kMargin */

/* where the layout's origin sits in the stage */
inline QPointF origin() { return QPointF(kMargin, kMargin); }
} // namespace a2600ctl

/* Shows the window, or hides it when it is showing. */
void a2600ControllersToggle(QWidget *parent, a2600session *session);
/* Shows (and raises) the window; leaves it showing if it already is. */
void a2600ControllersShow(QWidget *parent, a2600session *session);
bool a2600ControllersVisible();
/* The Controllers window has the keyboard focus (the main window then keeps
 * its keys down when it loses activation). */
bool a2600ControllersActive();
/* Preferences closed or the session restarted: redraw, and follow a port type
 * changed there. */
void a2600ControllersRefresh();
