/*
 * SettingsDialog -- the Preferences dialog. Controller types and the
 * analog switches apply live; the TV format and host options need a
 * restart, which the caller does when run() says one of those changed.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QDialog>

extern "C" {
#include "a2600session.h"
}

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    /* Returns true if a restart option changed. */
    static bool run(QWidget *parent, a2600session *session);
};
