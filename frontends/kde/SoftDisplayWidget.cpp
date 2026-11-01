/*
 * SoftDisplayWidget -- see SoftDisplayWidget.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "SoftDisplayWidget.h"

#include "DisplayGeometry.h"

#include <QEvent>
#include <QPainter>

SoftDisplayWidget::SoftDisplayWidget(a2600session *session, QWidget *parent)
    : QWidget(parent), m_session(session), m_feed(new FrameFeed(session, this)),
      m_scratch(A2600SESSION_FB_WIDTH, A2600SESSION_FB_MAX_HEIGHT, QImage::Format_RGB32)
{
    m_scratch.fill(0);
    setMinimumSize(320, 240);
    setFocusPolicy(Qt::StrongFocus);
    setAutoFillBackground(false);
    connect(m_feed, &FrameFeed::frameReady, this, &SoftDisplayWidget::onFrame);
}

SoftDisplayWidget::~SoftDisplayWidget()
{
    m_feed->setActive(false);
}

void SoftDisplayWidget::setTvAspect(bool tv) { m_tvAspect = tv; update(); }
void SoftDisplayWidget::setSmooth(bool smooth) { m_smooth = smooth; update(); }

void SoftDisplayWidget::updateActive()
{
    m_feed->setActive(isVisible() && window() && !window()->isMinimized());
}

void SoftDisplayWidget::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    if (window())
        window()->installEventFilter(this);
    updateActive();
}

void SoftDisplayWidget::hideEvent(QHideEvent *e)
{
    QWidget::hideEvent(e);
    if (window())
        window()->removeEventFilter(this);
    updateActive();
}

bool SoftDisplayWidget::eventFilter(QObject *watched, QEvent *e)
{
    if (e->type() == QEvent::WindowStateChange || e->type() == QEvent::Show ||
        e->type() == QEvent::Hide)
        updateActive();
    return QWidget::eventFilter(watched, e);
}

void SoftDisplayWidget::onFrame()
{
    int h = 0;
    if (!a2600session_copy_frame(m_session, reinterpret_cast<uint32_t *>(m_scratch.bits()),
                               &h, &m_serial))
        return;
    if (h <= 0 || h > A2600SESSION_FB_MAX_HEIGHT)
        return;
    m_height = h;
    /* The pixels are 0x00RRGGBB; Format_RGB32 needs 0xFFRRGGBB, or KWin
     * composites the zero alpha as see-through, so force alpha on. */
    m_image = m_scratch.copy(0, 0, A2600SESSION_FB_WIDTH, h);
    quint32 *px = reinterpret_cast<quint32 *>(m_image.bits());
    const size_t n = (size_t)A2600SESSION_FB_WIDTH * h;
    for (size_t i = 0; i < n; i++)
        px[i] |= 0xFF000000u;
    update();
}

void SoftDisplayWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    if (m_height <= 0) return;

    p.setRenderHint(QPainter::SmoothPixmapTransform, m_smooth);
    p.drawImage(displayRect(QSizeF(size()), m_height, m_tvAspect), m_image);
}
