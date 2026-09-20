/*
 * The emulator display: a GtkWidget that pulls frames from the session on
 * the compositor's own frame clock.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "a2600session.h"

#define A2600_TYPE_DISPLAY (a2600_display_get_type())
G_DECLARE_FINAL_TYPE(A2600Display, a2600_display, A2600, DISPLAY, GtkWidget)

GtkWidget *a2600_display_new(a2600session *session);
/* The 4:3 a television showed (2:1 pixels), or square pixels. */
void a2600_display_set_tv_aspect(A2600Display *self, gboolean tv);
void a2600_display_set_smooth(A2600Display *self, gboolean smooth);
