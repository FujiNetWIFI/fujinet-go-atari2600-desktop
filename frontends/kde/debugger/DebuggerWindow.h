/*
 * Debugger window (Qt6 Widgets) over Stella's own debugger engine, via
 * core/include/a2600debug.h. Mirrors the GNOME one tab for tab, and the
 * family's KDE debuggers' design: a QTabWidget under a toolbar, the
 * registers in a strip under the tabs.
 *
 * Showing the window stops the machine (as on every sibling); hiding it lets
 * the machine run on if it was running. The window is never deleted by its
 * close button: it is hidden, and comes back as it was.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
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
#include <QStringList>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <cstdint>
#include <functional>
#include <vector>

extern "C" {
#include "a2600debug.h"
#include "a2600session.h"
}

class VideoTab;

class DebuggerWindow : public QMainWindow {
    Q_OBJECT
public:
    /* Shows the window (which stops the machine). */
    static void showFor(QWidget *parent, a2600session *session);
    /* F12: shows it, or hides it when it is already up. */
    static void toggleFor(QWidget *parent, a2600session *session);

protected:
    bool event(QEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;
    bool eventFilter(QObject *obj, QEvent *e) override;

private:
    explicit DebuggerWindow(a2600session *session, QWidget *parent);
    ~DebuggerWindow() override;
    QWidget *buildToolbar();
    QWidget *buildConsole();
    QWidget *buildRegs();
    QWidget *buildStatesBox();
    QWidget *buildDisasm();
    QWidget *buildBasic();
    QWidget *buildMemory();
    QWidget *buildSearchBar();
    QWidget *buildVideo();
    QWidget *buildBreaks();
    QWidget *buildWatches();
    QWidget *buildCart();
    QWidget *buildHist(int idx);
    QWidget *buildSound();
    QWidget *buildSymbols();
    QWidget *buildFujinet();

    void refreshStatus();
    void refreshCpu();
    void refreshMem();
    void refreshBasic();
    void basicToggleAt(int line);
    void applyEnvBreak();
    void basicScrollToPc();
    void basicNote(const a2600srcmap_import_result &r, bool failed);
    void basicImported(const QString &path, bool folder);
    void basicPick(int kind);
    bool basicTab() const;
    void doStep();
    void doStepOver();
    void doStepOut();
    void refreshDisasm();
    void refreshVideo(bool force);
    void refreshBps();
    void refreshWatches(bool force);
    void refreshCart();
    void refreshHist(int idx, bool force);
    void refreshSound();
    void refreshSymbols(bool force);
    void refreshFujinet(bool force);
    void refreshPage(int page, bool live);
    void refreshAll();
    void resync();
    void syncLater();
    void settleAndSync();
    void tick();
    void recentreOnStop();

    void consoleAppend(const QString &text);
    void runConsole();
    void completeConsole();
    void say(const QString &msg);
    void message(const QString &msg);

    void toggleRun();
    void stepAnd(const std::function<void()> &fn);
    void runToCursor();
    void toggleBreakpoint(int addr);
    int disasmBank() const;
    int lineForAddress(uint16_t addr);
    void toggleSelected();
    void addBreakpoint();
    void breakAt();
    void setReg(int i);
    void setFlag(int id, bool on);
    void memGoto();
    void memSearch(bool backward);
    void showInMemory(uint16_t lo, uint16_t hi);
    void showInDisassembly(uint16_t addr);
    void scrollMemory(uint16_t addr);
    void popOut(int tab);
    void dock(int tab);
    int currentTab() const;
    bool tabVisible(int tab) const;
    void addWatch(const QString &text);
    void watchBreak(int index);
    void memWrite();
    void jumpTo();
    void loadSymbols();
    void defineSymbol();
    void clearSymbols();
    void save(const char *kind, const QString &title, const QString &file);
    int visibleRows() const;
    bool numberOf(const QString &text, long *out, QString *why = nullptr);
    bool addressOf(const QString &text, long *out, QString *why = nullptr);
    bool rangeOf(const QString &startText, const QString &endText, uint16_t *lo, uint16_t *hi, QString *why);
    void setEntry(QLineEdit *e, const QString &text);

    a2600session *m_session;
    a2600debug *m_dbg;
    unsigned m_seenGen = 0;
    bool m_wasStopped = false;
    int m_runningTicks = 0;
    QTimer m_timer;
    QTabWidget *m_tabs = nullptr;

    QLabel *m_status = nullptr;
    QPushButton *m_runBtn = nullptr;
    QLineEdit *m_breakAt = nullptr;

    /* console */
    QPlainTextEdit *m_conOut = nullptr;
    QLineEdit *m_conIn = nullptr;
    QStringList m_history;
    int m_historyPos = 0;

    /* registers strip */
    QLineEdit *m_reg[6] = {};
    QCheckBox *m_flag[7] = {};
    QLabel *m_beam = nullptr;

    /* memory: the 8K the 6507 sees, 16 bytes a row */
    QPlainTextEdit *m_mem = nullptr;
    QLineEdit *m_memGoto = nullptr, *m_memAddr = nullptr, *m_memVal = nullptr;
    bool m_hl = false;                  /* a range is highlighted in the dump */
    uint16_t m_hlLo = 0, m_hlHi = 0;
    QComboBox *m_srchKind = nullptr;
    QLineEdit *m_srchEntry = nullptr;
    QLabel *m_srchNote = nullptr;

    /* disassembly */
    QPlainTextEdit *m_disasm = nullptr;
    QCheckBox *m_followPc = nullptr;
    QComboBox *m_bank = nullptr;
    QLineEdit *m_jump = nullptr;
    int m_disasmFirst = 0;              /* the first listed line, in the bank's listing */
    int m_disasmBank = -1;              /* -1: the PC's bank */
    bool m_rebuildingBanks = false;
    uint64_t m_seenSerial = 0;          /* a2600debug_stop_serial last followed */
    int m_selected = -1;                /* the selected line's address, or -1 */
    int m_wheel = 0;                    /* wheel travel not yet a row */
    std::vector<uint16_t> m_lineAddr;

    /* BASIC: the imported source (a2600srcmap_shared) */
    QLabel *m_basNote = nullptr;
    QComboBox *m_basFiles = nullptr;
    QPlainTextEdit *m_basView = nullptr;
    int m_basFile = -1;                 /* the file shown, -1 none */
    unsigned m_basGen = ~0u, m_basDbgGen = ~0u;
    int m_basBar = -1;                  /* the block wearing the bar */
    uint64_t m_basSerial = 0;           /* the stop last followed */
    int m_basPcLine = 0, m_basPcFile = -1;
    QString m_envBreak;                 /* A2600_BASIC_BREAK, until it holds */

    /* video */
    VideoTab *m_video = nullptr;
    bool m_videoForce = true;           /* the tab was just shown: redraw everything */

    /* breakpoints */
    QTableWidget *m_bpTable = nullptr;
    QString m_bpSig;                    /* what the table shows, to rebuild on change */
    bool m_bpFilling = false;
    QLabel *m_bpEmpty = nullptr, *m_bpMsg = nullptr;
    QComboBox *m_bpType = nullptr, *m_bpBank = nullptr;
    QLineEdit *m_bpStart = nullptr, *m_bpEnd = nullptr, *m_bpCond = nullptr;
    QLabel *m_bpStop = nullptr;

    /* watches */
    QLineEdit *m_wtEntry = nullptr;
    QTableWidget *m_wtTable = nullptr;
    unsigned m_wtGen = ~0u;

    /* cart and I/O */
    QPlainTextEdit *m_io = nullptr;

    /* sound */
    QPlainTextEdit *m_sound = nullptr;

    /* symbols */
    QLineEdit *m_symFilter = nullptr;
    QTableWidget *m_symTable = nullptr;
    QLineEdit *m_symAddr = nullptr, *m_symName = nullptr;
    QLabel *m_symCount = nullptr;
    QString m_symSig;

    /* trace (0, live) and history (1, on a stop) share the engine's ring */
    QPushButton *m_hRec[2] = {};
    QComboBox *m_hFilter[2] = {};
    QLineEdit *m_hRange[2] = {};
    QPlainTextEdit *m_hText[2] = {};
    QLabel *m_hNote[2] = {};
    int m_hSeen[2] = { -1, -1 };
    bool m_hSync = false;

    /* tabs peeled off into windows of their own */
    QWidget *m_pages[16] = {};
    QWidget *m_popped[16] = {};
    bool m_closing = false;
    bool m_resumeOnHide = false;        /* it was running when the window came up */

    /* fujinet */
    QLabel *m_fujiNote = nullptr;
    QPlainTextEdit *m_fujiText = nullptr, *m_fujiState = nullptr;
    uint64_t m_fujiLast = 0;
    int m_fujiCount = -1;
};
