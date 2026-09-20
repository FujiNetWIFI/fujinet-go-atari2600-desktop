/*
 * The FujiNet console log window and the FujiNet configuration window.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QWidget>

extern "C" {
#include "a2600session.h"
}

/* Shows (raising an existing one) the FujiNet console log window. */
void fujinet_log_show(QWidget *parent, a2600session *session);
/* Shows the FujiNet web UI: embedded with QtWebEngine when built with it,
 * otherwise in the system browser. */
void fujinet_config_show(QWidget *parent, a2600session *session);

/* The accent colour, for highlights. */
QColor a2600AccentColor();
