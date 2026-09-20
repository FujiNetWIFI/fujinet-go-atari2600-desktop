/*
 * The emulator display: a QWidget that pulls frames from the session.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QImage>
#include <QTimer>
#include <QWidget>
#include <cstdint>
#include <vector>

#include "a2600session.h"

class DisplayWidget : public QWidget {
    Q_OBJECT
public:
    explicit DisplayWidget(a2600session *session, QWidget *parent = nullptr);
    void setTvAspect(bool tv);
    void setSmooth(bool smooth);

protected:
    void paintEvent(QPaintEvent *) override;

private:
    void tick();

    a2600session *m_session;
    QImage m_image;
    std::vector<uint32_t> m_fb;
    int m_height = 0;
    uint64_t m_serial = 0;
    QTimer m_timer;
    bool m_tv = true;
    bool m_smooth = false;
};
