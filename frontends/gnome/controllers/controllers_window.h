/*
 * The Controllers window: the Atari 2600's two controller ports drawn as a
 * player sees them (see controllers_window.c).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "a2600session.h"

G_BEGIN_DECLS

/* Shows the singleton window (creating it on first use). `parent` is the main
 * window: transient-for, and where typed keys and system actions go. */
void a2600_controllers_show(GtkWindow *parent, a2600session *session);
/* Shows it, or hides it if it is showing (F9, View > Controllers). */
void a2600_controllers_toggle(GtkWindow *parent, a2600session *session);
gboolean a2600_controllers_visible(void);

/* Map mode's share of a key, asked first by the main window's key path: while
 * Map is armed a press is the key to bind (once a target is picked) or is
 * swallowed, and Escape steps back. F9, F11, F12 and Ctrl chords are never
 * Map's. TRUE when Map took the key. */
gboolean a2600_controllers_map_key(guint keyval, guint code, GdkModifierType mods,
                                   gboolean down);

G_END_DECLS
