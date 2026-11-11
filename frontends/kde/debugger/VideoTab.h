/*
 * The debugger's Video tab (Qt6 Widgets): a thin view over core/include/a2600vidtab.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QList>
#include <QWidget>

extern "C" {
#include "a2600vidtab.h"
}

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QScrollArea;
class QTabWidget;
class QTableWidget;

class VideoPicture;

class VideoTab : public QWidget
{
    Q_OBJECT
public:
    explicit VideoTab(a2600session *session, QWidget *parent = nullptr);
    ~VideoTab() override;

    /* Take a snapshot and redraw what changed (force: everything). Cheap to call every tick. */
    void refresh(bool force);

protected:
    void resizeEvent(QResizeEvent *e) override;

private:
    friend class VideoPicture;
    void refreshView(unsigned what);
    void updatePixelSection();
    void fillRegs();
    void fillColours();
    void fillLines();
    void fillWrites();
    void setOrStrobe();
    void fillTiming();
    void note(const QString &text);
    void breakOnPixel();
    void updateSourceEnabled();

    a2600session *m_session;
    a2600vid_tab *m_tab = nullptr;
    QComboBox *m_view = nullptr, *m_source = nullptr, *m_zoom = nullptr;
    QList<QCheckBox *> m_overlays;
    QPushButton *m_breakBtn = nullptr;
    QLabel *m_status = nullptr, *m_note = nullptr;
    VideoPicture *m_pic = nullptr;
    QScrollArea *m_scroll = nullptr;
    QTabWidget *m_sections = nullptr;
    QLabel *m_pixelText = nullptr, *m_timingText = nullptr;
    QTableWidget *m_regs = nullptr, *m_lines = nullptr, *m_swatches = nullptr, *m_writes = nullptr;
    QLineEdit *m_setReg = nullptr, *m_setVal = nullptr;
    QProgressBar *m_beam = nullptr;
    QString m_regsSig, m_linesSig, m_writesSig;
    bool m_updating = false;
};
