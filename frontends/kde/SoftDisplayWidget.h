/*
 * The software display: the fallback for a machine with no usable GPU
 * device (or QT_QUICK_BACKEND=software style setups). A QWidget that scales
 * with QPainter, fed by FrameFeed like the GPU one.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QImage>
#include <QWidget>
#include <cstdint>

#include "DisplayView.h"
#include "FrameFeed.h"
#include "a2600session.h"

class SoftDisplayWidget : public QWidget, public DisplayView {
    Q_OBJECT
public:
    explicit SoftDisplayWidget(a2600session *session, QWidget *parent = nullptr);
    ~SoftDisplayWidget() override;

    QWidget *widget() override { return this; }
    void setTvAspect(bool tv) override;
    void setSmooth(bool smooth) override;

protected:
    void paintEvent(QPaintEvent *) override;
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;
    bool eventFilter(QObject *watched, QEvent *e) override;
    bool focusNextPrevChild(bool) override { return false; }

private:
    void onFrame();
    void updateActive();

    a2600session *m_session;
    FrameFeed *m_feed;
    QImage m_image;            /* Format_RGB32, sized to the newest frame */
    QImage m_scratch;          /* FB_WIDTH x FB_MAX_HEIGHT, filled by copy_frame */
    uint64_t m_serial = 0;
    int m_height = 0;
    bool m_tvAspect = true;
    bool m_smooth = false;
};
