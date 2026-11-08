/*
 * The Help window (Ctrl+Shift+H): the topics of data/help/help.md, built in
 * (a2600help.h).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QAbstractButton>
#include <QString>
#include <QWidget>

/* Shows the Help at id: a heading's slug or a context ("debugger/cpu",
 * "prefs/Machine"), or "?words" to search; empty stays where it was. home
 * is the main window it belongs to. */
void a2600HelpShow(QWidget *home, const QString &id);

/* Opens the Help for what window shows: its "a2600HelpContext" property
 * (or, in the Help, finds). */
void a2600HelpFor(QWidget *window);

/* A Help button: opens context, or with an empty one its window's. */
QAbstractButton *a2600HelpButton(const QString &context);
