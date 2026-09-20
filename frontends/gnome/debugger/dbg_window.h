/*
 * Debugger window for the GNOME frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "a2600session.h"

G_BEGIN_DECLS

/* Shows (creating on first use) the debugger window for the session. */
void a2600_debugger_show(GtkWindow *parent, a2600session *session);

G_END_DECLS
