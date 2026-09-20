/*
 * Debugger window (Qt6 Widgets) over Stella's own debugger engine, via
 * core/include/a2600debug.h. Mirrors the GNOME one tab for tab.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTimer>
#include <cstdint>
#include <vector>

extern "C" {
#include "a2600debug.h"
#include "a2600session.h"
}

class DebuggerWindow : public QMainWindow {
    Q_OBJECT
public:
    static void showFor(QWidget *parent, a2600session *session);

protected:
    void keyPressEvent(QKeyEvent *e) override;
    void closeEvent(QCloseEvent *e) override;
    bool eventFilter(QObject *obj, QEvent *e) override;

private:
    explicit DebuggerWindow(a2600session *session, QWidget *parent);
    QWidget *buildToolbar();
    QWidget *buildPrompt();
    QWidget *buildCpu();
    QWidget *buildDisasm();
    QWidget *buildTia();
    QWidget *buildRiot();
    QWidget *buildBreaks();
    QWidget *buildStates();

    void refreshAll();
    void refreshStatus();
    void refreshCpu();
    void refreshRam();
    void refreshDisasm();
    void refreshTia();
    void refreshRiot();
    void refreshBps();
    void tick();
    void runPrompt();
    void appendPrompt(const QString &text);
    void jumpTo(const QString &text);

    a2600session *m_session;
    a2600debug *m_dbg;
    unsigned m_seenGen = 0;
    bool m_wasStopped = false;
    int m_runningTicks = 0;
    QTimer m_timer;

    QLabel *m_status = nullptr;
    QPushButton *m_runBtn = nullptr;

    QPlainTextEdit *m_promptOut = nullptr;
    QLineEdit *m_promptIn = nullptr;

    QLineEdit *m_reg[6] = {};
    QCheckBox *m_flag[7] = {};
    QLabel *m_cycles = nullptr;
    QPlainTextEdit *m_ram = nullptr;
    QLineEdit *m_ramAddr = nullptr, *m_ramVal = nullptr;

    QPlainTextEdit *m_disasm = nullptr;
    QComboBox *m_bank = nullptr;
    QCheckBox *m_followPc = nullptr;
    QLineEdit *m_jump = nullptr;
    int m_disasmFirst = 0;
    int m_disasmBank = -1;
    std::vector<uint16_t> m_lineAddr;

    QPlainTextEdit *m_tiaText = nullptr;
    QLabel *m_tiaPic = nullptr;
    QCheckBox *m_tiaPartial = nullptr;
    QLineEdit *m_tiaReg = nullptr, *m_tiaVal = nullptr;
    std::vector<uint32_t> m_tiaPx;

    QPlainTextEdit *m_riot = nullptr;
    QPlainTextEdit *m_bps = nullptr;
    QLineEdit *m_bpEntry = nullptr;
    QLabel *m_cartInfo = nullptr;
};
