/*
 * The Win32 debugger window over Stella's debugger engine (a2600debug.h).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <windows.h>

#include "a2600session.h"

/* Show (F12): created on first use, hidden not destroyed after. Stops the
 * machine, as the other frontends do, so there is something to look at. */
void a2600_debugger_show(HWND parent, a2600session *session);

/* Accelerators (F5/F7/F8/Shift+F8/F12) for the main loop's pretranslate
 * hook. Returns 1 if the message was consumed. */
int a2600_debugger_pretranslate(MSG *msg);
