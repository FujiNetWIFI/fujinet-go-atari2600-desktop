/*
 * The app's accent colour (A2600SESSION_ACCENT_RGB, Atari orange) for the
 * Controllers window. Buttons in the accent state -- Map while armed, the
 * chosen side of a segment -- carry the dynamic property "accent" and wear it
 * with its dark ink; main.cpp's style sheet draws them. Held parts in the
 * window use the held highlight, which is lighter, so it reads on black
 * plastic (DESIGN-atari.md section 1).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QColor>
#include <QPushButton>
#include <QStyle>

#include "a2600session.h"

/* the dark ink that reads on the orange */
#define A2600_CTL_ACCENT_INK_RGB 0x2A1604u
/* a held part on black plastic */
#define A2600_CTL_HELD_RGB 0xFFB04Au

inline QColor a2600CtlAccent()
{
    return QColor((A2600SESSION_ACCENT_RGB >> 16) & 0xff,
                  (A2600SESSION_ACCENT_RGB >> 8) & 0xff,
                  A2600SESSION_ACCENT_RGB & 0xff);
}

inline QColor a2600CtlAccentInk()
{
    return QColor((A2600_CTL_ACCENT_INK_RGB >> 16) & 0xff,
                  (A2600_CTL_ACCENT_INK_RGB >> 8) & 0xff,
                  A2600_CTL_ACCENT_INK_RGB & 0xff);
}

inline QColor a2600CtlHeld()
{
    return QColor((A2600_CTL_HELD_RGB >> 16) & 0xff,
                  (A2600_CTL_HELD_RGB >> 8) & 0xff,
                  A2600_CTL_HELD_RGB & 0xff);
}

/* Puts a button in or out of the accent state and repolishes it, so the style
 * sheet's [accent="true"] rule takes effect at once. */
inline void a2600CtlSetAccent(QPushButton *b, bool on)
{
    if (b->property("accent").toBool() == on) return;
    b->setProperty("accent", on);
    b->style()->unpolish(b);
    b->style()->polish(b);
}
