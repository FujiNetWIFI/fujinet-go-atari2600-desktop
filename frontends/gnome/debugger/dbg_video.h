/*
 * The debugger's Video tab (GTK4): a thin view over core/include/a2600vidtab.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef A2600_GNOME_DBG_VIDEO_H
#define A2600_GNOME_DBG_VIDEO_H

#include <gtk/gtk.h>

#include "a2600session.h"

/* The page. It owns its controller and frees it when destroyed. */
GtkWidget *dbg_video_new(a2600session *session);

/* Bring the page up to date (only what changed is redrawn). The first call
 * after the page is mapped, or force, redraws everything. */
void dbg_video_refresh(GtkWidget *page, gboolean force);

#endif
