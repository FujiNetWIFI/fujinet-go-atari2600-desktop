/*
 * What the main window needs from its display, so the GPU display and the
 * software fallback are interchangeable.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

class QWidget;

class DisplayView {
public:
    virtual ~DisplayView() = default;
    virtual QWidget *widget() = 0;
    /* "tv_aspect" setting: letterboxed into the 4:3 a television showed
     * when true, square pixels when false. */
    virtual void setTvAspect(bool tv) = 0;
    virtual void setSmooth(bool smooth) = 0;
};
