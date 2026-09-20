/*
 * Preferences dialog for the GNOME frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "a2600session.h"

G_BEGIN_DECLS

typedef struct _A2600Window A2600Window;

/* Shows the preferences dialog. Controller types and the analog switches
 * apply live; the TV format and the host options (FujiNet, audio, gamepads)
 * are read when the session starts, so the dialog restarts the session on
 * close if one of those changed. */
void a2600_prefs_show(A2600Window *parent, a2600session *session,
                      void (*restart)(A2600Window *parent));

G_END_DECLS
