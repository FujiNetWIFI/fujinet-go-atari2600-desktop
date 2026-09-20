/*
 * The main application window.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "a2600session.h"

#define A2600_TYPE_WINDOW (a2600_window_get_type())
G_DECLARE_FINAL_TYPE(A2600Window, a2600_window, A2600, WINDOW,
                     AdwApplicationWindow)

GtkWidget *a2600_window_new(AdwApplication *app, a2600session *session);
/* main.c: the icon name to use (the installed id, or the in-tree art). */
const char *a2600_icon_name(void);
void a2600_window_toast(A2600Window *self, const char *text);

/* The accent colour (A2600SESSION_ACCENT_RGB) as a CSS class every window
 * can use: ".a2600-accent" paints a widget's background in it, and
 * ".a2600-accent-text" its label. Installed once by the main window. */
void a2600_install_accent_css(void);
