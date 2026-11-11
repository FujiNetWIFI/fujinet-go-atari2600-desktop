/*
 * debugger_test -- the Debugger window (debugger/DebuggerWindow.cpp) driven
 * with synthetic events on the offscreen platform against a running 2600
 * session (the CONFIG client, FujiNet off) whose zero-page RAM holds a
 * hand-assembled loop written through the debugger: the tabs, show and hide
 * (a stop and a resume), F12, the stepping keys and buttons, the console
 * (transcript, Tab completion, history), the disassembly (the PC's accent
 * bar, click / double-click / F9 for a breakpoint, selection and Run to
 * Cursor, the wheel, the jump box, the bank chooser), the register and memory
 * entries, the Breakpoints tab (every type, a bank for execute), Watches,
 * search, Trace and History, Cart & I/O (states, saves), Sound, Symbols
 * (Define), Break at, pop-out and dock, the Video tab on the real CONFIG
 * display, and F12 and the View menu from the main window.
 *
 * DBG_TEST_SHOTS=<dir> also grabs a PNG of each tab (DBG_TEST_LIGHT=1: in the
 * light scheme).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QFile>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QOpenGLContext>
#include <QTemporaryDir>
#include <QtTest>

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QMenu>
#include <QStatusBar>
#include <QStyleHints>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextBlock>
#include <QThread>
#include <QToolButton>

#include "a2600debug.h"
#include "a2600memtools.h"
#include "a2600session.h"
#include "DisplayWidget.h"
#include "MainWindow.h"
#include "debugger/DebuggerWindow.h"

namespace {

/* The test subject, in zero-page RAM at $80 (the 6507 runs from RAM as it
 * would from ROM):
 *   0080  LDA #$00
 *   0082  LDY #$03
 *   0084  JSR $00A0      ; INX / RTS
 *   0087  DEY
 *   0088  BNE $0084
 *   008A  CLC
 *   008B  ADC #$01
 *   008D  STA $00B0      ; write
 *   0090  LDA $00B0      ; read
 *   0093  JMP $0080 */
enum { kLda = 0x80, kLdy = 0x82, kJsr = 0x84, kDey = 0x87, kClc = 0x8A, kSta = 0x8D, kLda2 = 0x90,
       kJmp = 0x93, kSub = 0xA0, kData = 0xB0 };
const uint8_t kProg[] = {
    0xA9, 0x00, 0xA0, 0x03, 0x20, 0xA0, 0x00, 0x88, 0xD0, 0xFA, 0x18, 0x69, 0x01,
    0x8D, 0xB0, 0x00, 0xAD, 0xB0, 0x00, 0x4C, 0x80, 0x00,
};
const uint8_t kSubCode[] = { 0xE8, 0x60 };

/* The accent: the 2600's orange (the family's #FFA645). */
const QColor kAccent(0xFF, 0xA6, 0x45);

int pcOf(a2600debug *d)
{
    a2600debug_cpu c;
    a2600debug_cpu_get(d, &c);
    return c.pc;
}

/* With the machine stopped: the program in place, PC at its LDY #3. */
void loadProgram(a2600debug *d)
{
    for (size_t i = 0; i < sizeof kProg; i++) a2600debug_write(d, (uint16_t)(kLda + i), kProg[i]);
    for (size_t i = 0; i < sizeof kSubCode; i++) a2600debug_write(d, (uint16_t)(kSub + i), kSubCode[i]);
    a2600debug_cpu_set(d, A2600_FLAG_D, 0);
    a2600debug_cpu_set(d, A2600_REG_SP, 0xFF);
    a2600debug_cpu_set(d, A2600_REG_A, 0);
    a2600debug_cpu_set(d, A2600_REG_PC, kLda);
    /* run to the LDY: the next stop is then a plain one, with its reason */
    a2600debug_bp_add(d, A2600DEBUG_BP_EXEC, kLdy, kLdy, A2600DEBUG_ANY_BANK, "");
    a2600debug_resume(d);
    for (int i = 0; i < 600 && !(a2600debug_is_stopped(d) && pcOf(d) == kLdy); i++) QThread::msleep(5);
    a2600debug_breakpoint_clear(d);
}

} // namespace

class DebuggerTest : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;
    a2600session *m_s = nullptr;
    a2600debug *m_d = nullptr;
    QMainWindow *m_win = nullptr;
    QTabWidget *m_tabs = nullptr;
    QString m_shots;
    a2600session_start_opts m_opts = {};
    int m_start = -1;               /* CONFIG's Start, read before any test clears the symbols */

    QPushButton *button(const QString &textStart)
    {
        for (QPushButton *b : m_win->findChildren<QPushButton *>())
            if (b->text().startsWith(textStart)) return b;
        return nullptr;
    }
    QLineEdit *entry(const QString &placeholderStart)
    {
        for (QLineEdit *e : m_win->findChildren<QLineEdit *>())
            if (e->placeholderText().startsWith(placeholderStart)) return e;
        return nullptr;
    }
    QLineEdit *entryIs(const QString &placeholder)
    {
        for (QLineEdit *e : m_win->findChildren<QLineEdit *>())
            if (e->placeholderText() == placeholder) return e;
        return nullptr;
    }
    QPlainTextEdit *disasmView()
    {
        for (QPlainTextEdit *v : m_win->findChildren<QPlainTextEdit *>())
            if (v->verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOff) return v;
        return nullptr;
    }
    QPlainTextEdit *consoleView()
    {
        for (QPlainTextEdit *v : m_win->findChildren<QPlainTextEdit *>())
            if (v->lineWrapMode() == QPlainTextEdit::WidgetWidth) return v;
        return nullptr;
    }
    QLabel *statusLabel()
    {
        for (QLabel *l : m_win->findChildren<QLabel *>())
            if (l->text().startsWith(QLatin1String("Stopped")) ||
                l->text() == QLatin1String("Running"))
                return l;
        return nullptr;
    }
    QCheckBox *check(const QString &text)
    {
        for (QCheckBox *c : m_win->findChildren<QCheckBox *>())
            if (c->text() == text) return c;
        return nullptr;
    }
    QTableWidget *bpTable() { return m_tabs->widget(3)->findChild<QTableWidget *>(); }
    QPlainTextEdit *memView() { return m_tabs->widget(2)->findChild<QPlainTextEdit *>(); }

    void waitStopped()
    {
        QTRY_VERIFY_WITH_TIMEOUT(a2600debug_is_stopped(m_d), 3000);
        /* the window follows within its poll */
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel() &&
                                     statusLabel()->text().startsWith(QLatin1String("Stopped")), 2000);
    }
    QByteArray why() { return (statusLabel() ? statusLabel()->text() : QString()).toUtf8(); }
    void shot(const QString &name)
    {
        if (!m_shots.isEmpty()) m_win->grab().save(m_shots + QLatin1Char('/') + name + QStringLiteral(".png"));
    }

    /* The block of the disassembly that shows an address, or -1. */
    int lineOf(QPlainTextEdit *v, int addr)
    {
        const QString want = QStringLiteral("%1").arg(addr, 4, 16, QLatin1Char('0')).toUpper();
        int n = 0;
        for (QTextBlock b = v->document()->begin(); b.isValid(); b = b.next(), ++n)
            if (b.text().mid(2, 4) == want) return n;
        return -1;
    }
    QPoint lineAt(QPlainTextEdit *v, int line, int column)
    {
        QTextCursor c(v->document()->findBlockByNumber(line));
        c.setPosition(c.block().position() + column);
        const QRect r = v->cursorRect(c);
        return QPoint(r.left() + 1, r.center().y());
    }
    void clickLine(int addr, int column, bool dbl = false)
    {
        QPlainTextEdit *v = disasmView();
        const int line = lineOf(v, addr);
        QVERIFY2(line >= 0, qPrintable(QStringLiteral("line for %1").arg(addr, 0, 16)));
        const QPoint p = lineAt(v, line, column);
        if (dbl) QTest::mouseDClick(v->viewport(), Qt::LeftButton, {}, p);
        else QTest::mouseClick(v->viewport(), Qt::LeftButton, {}, p);
    }
    void typeInto(QLineEdit *e, const QString &text)
    {
        e->clear();
        QTest::keyClicks(e, text);
        QTest::keyClick(e, Qt::Key_Return);
    }

    /* The disassembly lists the cartridge, not RAM: the tests that read it put the PC at CONFIG's Start
     * and take the first instruction addresses from the listing. */
    bool gotoRom(QVector<int> *addrs)
    {
        if (m_start < 0) return false;
        a2600debug_cpu_set(m_d, A2600_REG_PC, m_start);
        static a2600debug_line lines[64];
        int total = 0, pcLine = -1;
        a2600debug_disassemble(m_d, -1, 0, lines, 1, &total, &pcLine);
        if (pcLine < 0) return false;
        const int got = a2600debug_disassemble(m_d, -1, pcLine, lines, 64, &total, &pcLine);
        addrs->clear();
        for (int i = 0; i < got && addrs->size() < 10; ++i)
            if (lines[i].disasm[0] && lines[i].disasm[0] != '.') addrs->append(lines[i].address);
        return addrs->size() >= 8 && addrs->at(0) == m_start;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        qputenv("SDL_AUDIODRIVER", "dummy");
        if (qEnvironmentVariableIsSet("DBG_TEST_SHOTS")) m_shots = qEnvironmentVariable("DBG_TEST_SHOTS");
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        if (qEnvironmentVariableIsSet("DBG_TEST_LIGHT"))
            QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Light);
#endif
        a2600session_paths paths = {};
        const QByteArray dir = m_dir.path().toUtf8();
        paths.config_dir = dir.constData();
        paths.data_dir = dir.constData();
        paths.fujinet_lib = "";
        m_s = a2600session_new(&paths);
        QVERIFY(m_s);
        m_d = a2600session_debugger(m_s);
        QVERIFY(m_d);

        a2600session_default_opts(m_s, &m_opts);
        m_opts.cart_path = nullptr;
        m_opts.enable_fujinet = 0;
        m_opts.enable_audio = 0;
        m_opts.enable_gamepad = 0;
        QCOMPARE(a2600session_start(m_s, &m_opts), 0);
        m_start = a2600debug_label_address(m_d, "Start");
        QVERIFY2(m_start >= 0, "CONFIG has a Start label");

        /* the window opens on the tab A2600_DEBUGGER_TAB names, and
         * A2600_DEBUGGER_WATCH, _WATCH_BREAK and _MEM_GOTO are read at creation */
        qputenv("A2600_DEBUGGER_TAB", "i/o");
        qputenv("A2600_DEBUGGER_WATCH", "$90, $A0-$AF");
        qputenv("A2600_DEBUGGER_WATCH_BREAK", "$C0");
        qputenv("A2600_DEBUGGER_MEM_GOTO", "$D0-$DF");
        DebuggerWindow::showFor(nullptr, m_s);
        qunsetenv("A2600_DEBUGGER_TAB");
        qunsetenv("A2600_DEBUGGER_WATCH");
        qunsetenv("A2600_DEBUGGER_WATCH_BREAK");
        qunsetenv("A2600_DEBUGGER_MEM_GOTO");
        for (QWidget *w : QApplication::topLevelWidgets())
            if (w->objectName() == QLatin1String("debugger")) m_win = qobject_cast<QMainWindow *>(w);
        QVERIFY(m_win);
        QVERIFY(QTest::qWaitForWindowExposed(m_win));
        m_tabs = m_win->findChild<QTabWidget *>();
        QVERIFY(m_tabs);
        QCOMPARE(m_tabs->currentIndex(), 5);
        QCOMPARE(a2600watch_count(), 3);                      /* two watches and the break-on-change one */
        a2600debug_breakpoint list[4];
        QCOMPARE(a2600debug_bp_list(m_d, list, 4), 1);
        QCOMPARE((int)list[0].start, 0xC0);
        QVERIFY(QString::fromUtf8(list[0].condition).contains(QStringLiteral("old")));
        a2600watch_clear();
        m_tabs->setCurrentIndex(0);
        waitStopped();
    }

    void cleanupTestCase()
    {
        delete m_win;
        if (m_s) {
            a2600session_stop(m_s);
            a2600session_free(m_s);
        }
    }

    void init()
    {
        if (!m_win->isVisible()) DebuggerWindow::toggleFor(nullptr, m_s);
        if (!a2600debug_is_stopped(m_d)) a2600debug_stop(m_d);
        waitStopped();
        a2600debug_breakpoint_clear(m_d);
        loadProgram(m_d);
    }

    void tabsAndStop()
    {
        QStringList names;
        for (int i = 0; i < m_tabs->count(); ++i) names << m_tabs->tabText(i);
        QCOMPARE(names, (QStringList{ QStringLiteral("Console"), QStringLiteral("Disassembly"),
                                      QStringLiteral("Memory"), QStringLiteral("Breakpoints"),
                                      QStringLiteral("Watches"), QStringLiteral("Cart && I/O"),
                                      QStringLiteral("Trace"), QStringLiteral("History"),
                                      QStringLiteral("Video"), QStringLiteral("Sound"),
                                      QStringLiteral("Symbols"), QStringLiteral("FujiNet") }));
        QVERIFY(a2600debug_is_stopped(m_d));
        /* Run is the accent while stopped, whichever text it wears, in one size */
        QPushButton *run = button(QStringLiteral("Run (F5)"));
        QVERIFY(run);
        QVERIFY(run->isChecked());
        QVERIFY(run->styleSheet().contains(kAccent.name(), Qt::CaseInsensitive));
        const QSize size = run->size();
        QTest::mouseClick(run, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(!a2600debug_is_stopped(m_d), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(run->text() == QLatin1String("Stop (F5)"), 2000);
        QVERIFY(!run->isChecked());
        QCOMPARE(run->size(), size);
        QCOMPARE(statusLabel()->text(), QStringLiteral("Running"));
        QTest::mouseClick(run, Qt::LeftButton);
        waitStopped();
        QVERIFY(run->isChecked());
        QCOMPARE(run->size(), size);
        /* the toolbar has the family's buttons */
        for (const char *label : { "Step (F7)", "Step Over (F8)", "Step Out (Shift+F8)", "Scan+1", "Frame+1", "Rewind",
                                   "Unwind", "Clear Breakpoints", "Load Symbols", "Clear Symbols", "Pop Out" })
            QVERIFY2(button(QString::fromUtf8(label)), label);
        bool help = false;
        for (QToolButton *b : m_win->findChildren<QToolButton *>()) help |= b->text() == QLatin1String("Help");
        QVERIFY(help);
        QVERIFY(entry(QStringLiteral("symbol or")));
    }

    void stepKeys()
    {
        QCOMPARE(pcOf(m_d), (int)kLdy);
        QTest::keyClick(m_win, Qt::Key_F7);
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(m_d), (int)kJsr, 2000);
        QVERIFY2(statusLabel()->text().startsWith(QLatin1String("Stopped: step")), why().constData());
        /* F8 steps over the JSR */
        QTest::keyClick(m_win, Qt::Key_F8);
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(m_d), (int)kDey, 2000);
        /* the buttons do the same: Step Out from the subroutine */
        a2600debug_write(m_d, 0x00FE, 0x86);        /* a return address, as a JSR left it */
        a2600debug_write(m_d, 0x00FF, 0x00);
        a2600debug_cpu_set(m_d, A2600_REG_SP, 0xFD);
        a2600debug_cpu_set(m_d, A2600_REG_PC, kSub);
        QTest::mouseClick(button(QStringLiteral("Step Out")), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(m_d), (int)kDey, 2000);
        QTest::mouseClick(button(QStringLiteral("Step Over")), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(m_d), 0x88, 2000);
        /* Scan+1 runs on about a scan line (76 CPU cycles) */
        a2600debug_cpu c, c2;
        a2600debug_cpu_get(m_d, &c);
        QTest::mouseClick(button(QStringLiteral("Scan+1")), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(a2600debug_is_stopped(m_d) &&
                                     (a2600debug_cpu_get(m_d, &c2), c2.total_cycles > c.total_cycles + 10), 3000);
        /* stepping while running is refused with a word, not silently */
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(!a2600debug_is_stopped(m_d), 2000);
        QTest::keyClick(m_win, Qt::Key_F7);
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("Stop the machine")));
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
    }

    /* The stop reason is the status line, and it is never cut: the label has
     * the width its whole text needs at the default window width. */
    void stopReasonNotElided()
    {
        a2600debug_bp_add(m_d, A2600DEBUG_BP_WRITE, kData, kData, A2600DEBUG_ANY_BANK, "value != old");
        a2600debug_write(m_d, kData, 0x77);
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(a2600debug_is_stopped(m_d), 4000);
        QTRY_VERIFY2_WITH_TIMEOUT(statusLabel()->text().contains(QLatin1String("wrote $00B0")), why().constData(), 2000);
        QLabel *status = statusLabel();
        QVERIFY(status->text().contains(QLatin1String("(was $77)")));
        QVERIFY2(status->width() >= status->fontMetrics().horizontalAdvance(status->text()),
                 qPrintable(QStringLiteral("status %1 wide for %2").arg(status->width())
                                .arg(status->fontMetrics().horizontalAdvance(status->text()))));
        QVERIFY(!status->text().contains(QChar(0x2026)));
        shot(QStringLiteral("stopreason"));
    }

    void consoleCommands()
    {
        m_tabs->setCurrentIndex(0);
        QLineEdit *in = entry(QStringLiteral("command"));
        QPlainTextEdit *out = consoleView();
        QVERIFY(in && out);
        QCOMPARE(m_win->focusWidget(), in);         /* typing goes straight in */
        typeInto(in, QStringLiteral("print pc"));
        QVERIFY(out->toPlainText().contains(QStringLiteral("> print pc")));
        QVERIFY2(out->toPlainText().contains(QStringLiteral("0082")), qPrintable(out->toPlainText()));
        QVERIFY(in->text().isEmpty());
        /* a command that moves the machine moves the window */
        typeInto(in, QStringLiteral("break $84"));
        QCOMPARE(a2600debug_bp_list(m_d, nullptr, 0), 1);
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY2_WITH_TIMEOUT(a2600debug_is_stopped(m_d) && pcOf(m_d) == kJsr, "stopped at the break", 3000);
        QTRY_VERIFY2_WITH_TIMEOUT(out->toPlainText().contains(QStringLiteral("— BP: $0084")), qPrintable(out->toPlainText()), 3000);
        QTRY_VERIFY2_WITH_TIMEOUT(statusLabel()->text().contains(QStringLiteral("$0084")), why().constData(), 3000);
        /* Tab completes a command, lists several, and Up recalls */
        in->clear();
        QTest::keyClicks(in, QStringLiteral("clearB"));
        QTest::keyClick(in, Qt::Key_Tab);
        QCOMPARE(in->text(), QStringLiteral("clearBreaks "));
        in->clear();
        QTest::keyClicks(in, QStringLiteral("clear"));
        QTest::keyClick(in, Qt::Key_Tab);
        QVERIFY(out->toPlainText().contains(QStringLiteral("clearBreaks")));
        QVERIFY(out->toPlainText().contains(QStringLiteral("clearTraps")));
        QVERIFY(in->text().startsWith(QStringLiteral("clear")));
        in->clear();
        QTest::keyClick(in, Qt::Key_Up);
        QCOMPARE(in->text(), QStringLiteral("break $84"));
        QTest::keyClick(in, Qt::Key_Up);
        QCOMPARE(in->text(), QStringLiteral("print pc"));
        QTest::keyClick(in, Qt::Key_Down);
        QCOMPARE(in->text(), QStringLiteral("break $84"));
        in->clear();
        /* the family's own commands */
        typeInto(in, QStringLiteral("cart"));
        QVERIFY(out->toPlainText().contains(QStringLiteral("> cart")));
        typeInto(in, QStringLiteral("video"));
        shot(QStringLiteral("console"));
    }

    /* Load Symbols (no cartridge .sym: said, not silent), a label defined at the console shows in the
     * views, and Break at takes a symbol or a hex address. */
    void symbolsAndBreakAt()
    {
        QVector<int> rom;
        QVERIFY2(gotoRom(&rom), "the PC at CONFIG's Start");
        const QString second = QStringLiteral("%1").arg(rom[1], 4, 16, QLatin1Char('0')).toUpper();
        const QString third = QStringLiteral("%1").arg(rom[2], 4, 16, QLatin1Char('0')).toUpper();
        m_tabs->setCurrentIndex(0);
        const int before = consoleView()->document()->blockCount();
        QTest::mouseClick(button(QStringLiteral("Load Symbols")), Qt::LeftButton);
        QVERIFY(consoleView()->document()->blockCount() > before);
        QCOMPARE(a2600debug_label_address(m_d, "afterwards"), -1);
        QLineEdit *in = entry(QStringLiteral("command"));
        typeInto(in, QStringLiteral("define afterwards $") + second);
        QCOMPARE(a2600debug_label_address(m_d, "afterwards"), rom[1]);

        /* Break at: a symbol, then a hex address, then a repeat */
        QLineEdit *breakAt = entry(QStringLiteral("symbol or"));
        QVERIFY(breakAt);
        typeInto(breakAt, QStringLiteral("afterwards"));
        QVERIFY(a2600debug_breakpoint_check(m_d, (uint16_t)rom[1], A2600DEBUG_ANY_BANK));
        QVERIFY(breakAt->text().isEmpty());
        typeInto(breakAt, third);
        QVERIFY(a2600debug_breakpoint_check(m_d, (uint16_t)rom[2], A2600DEBUG_ANY_BANK));
        typeInto(breakAt, third);
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("already")));
        a2600debug_breakpoint list[4];
        QCOMPARE(a2600debug_bp_list(m_d, list, 4), 2);
        typeInto(breakAt, QStringLiteral("nosuchsymbol"));
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("Not a number")));
        a2600debug_breakpoint_clear(m_d);

        /* the stop reason names where it is */
        typeInto(breakAt, QStringLiteral("afterwards"));
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(a2600debug_is_stopped(m_d) && pcOf(m_d) == rom[1], 3000);
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel()->text().contains(QLatin1String("(afterwards)")), 2000);

        /* the Symbols tab lists them, filtered; activating a row opens the Disassembly there */
        m_tabs->setCurrentIndex(10);
        QTableWidget *table = m_tabs->currentWidget()->findChild<QTableWidget *>();
        QLineEdit *filter = m_tabs->currentWidget()->findChild<QLineEdit *>();
        QVERIFY(table && filter);
        QTRY_VERIFY_WITH_TIMEOUT(table->rowCount() >= 1, 2000);
        /* Define: an address and a name */
        QLineEdit *defAddr = entryIs(QStringLiteral("$address"));
        QLineEdit *defName = entry(QStringLiteral("label name"));
        QVERIFY(defAddr && defName);
        defName->setText(QStringLiteral("helper"));
        typeInto(defAddr, third);
        QCOMPARE(a2600debug_label_address(m_d, "helper"), rom[2]);
        QTRY_VERIFY_WITH_TIMEOUT(table->rowCount() >= 2, 2000);
        shot(QStringLiteral("symbols"));
        const int all = table->rowCount();
        filter->setText(QStringLiteral("AFTERW"));
        QTRY_COMPARE_WITH_TIMEOUT(table->rowCount(), 1, 2000);
        QCOMPARE(table->item(0, 1)->text(), QStringLiteral("afterwards"));
        QCOMPARE(table->item(0, 0)->text(), QStringLiteral("$") + second);
        table->setCurrentCell(0, 0);
        QTest::keyClick(table, Qt::Key_Return);             /* activating a row */
        QCOMPARE(m_tabs->currentIndex(), 1);
        QVERIFY(!check(QStringLiteral("Follow PC"))->isChecked());
        QVERIFY(lineOf(disasmView(), rom[1]) >= 0);
        m_tabs->setCurrentIndex(10);
        filter->clear();
        QTRY_COMPARE_WITH_TIMEOUT(table->rowCount(), all, 2000);

        /* Clear Symbols drops the loaded ones */
        QTest::mouseClick(button(QStringLiteral("Clear Symbols")), Qt::LeftButton);
        QCOMPARE(a2600debug_label_address(m_d, "afterwards"), -1);
        QTRY_VERIFY_WITH_TIMEOUT(table->rowCount() < all, 2000);
        m_tabs->setCurrentIndex(0);
    }

    void disassemblyHighlights()
    {
        QVector<int> rom;
        QVERIFY2(gotoRom(&rom), "the PC at CONFIG's Start");
        m_tabs->setCurrentIndex(1);
        QPlainTextEdit *v = disasmView();
        QVERIFY(v);
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, rom[0]) >= 0, 2000);
        const int pcLine = lineOf(v, rom[0]);
        QCOMPARE(v->document()->findBlockByNumber(pcLine).text().left(2), QStringLiteral(" >"));
        /* follow-PC keeps it about a third of the way down */
        const int rows = v->document()->blockCount();
        QVERIFY2(pcLine >= rows / 3 - 2 && pcLine <= rows / 3 + 2, qPrintable(QStringLiteral("pc line %1 of %2").arg(pcLine).arg(rows)));
        /* the PC's line: the accent as the block's own background, so it lies under the gutter */
        const QTextBlock pcBlock = v->document()->findBlockByNumber(pcLine);
        QCOMPARE(pcBlock.blockFormat().background().color(), kAccent);
        for (const auto &s : v->extraSelections())
            QVERIFY(!s.format.boolProperty(QTextFormat::FullWidthSelection));
        QCOMPARE(v->document()->findBlockByNumber(pcLine + 1).blockFormat().background().style(), Qt::NoBrush);

        /* a breakpoint's dot: filled and red, hollow and grey when disabled */
        const int bank = a2600debug_current_bank(m_d);
        a2600debug_breakpoint_toggle(m_d, (uint16_t)rom[2], bank);
        QTRY_VERIFY_WITH_TIMEOUT(v->document()->findBlockByNumber(lineOf(v, rom[2])).text().startsWith(
                                     QStringLiteral("●")), 2000);
        auto dotColor = [&](int line) {
            QColor c;
            for (const auto &s : v->extraSelections())
                if (s.cursor.blockNumber() == line && s.cursor.hasSelection() &&
                    s.cursor.selectionStart() == s.cursor.block().position() &&
                    s.cursor.selectionEnd() == s.cursor.block().position() + 1)
                    c = s.format.foreground().color();
            return c;
        };
        QCOMPARE(dotColor(lineOf(v, rom[2])), QColor(0xe0, 0x1b, 0x24));
        a2600debug_breakpoint b;
        QCOMPARE(a2600debug_bp_list(m_d, &b, 1), 1);
        a2600debug_bp_enable(m_d, b.id, 0);
        QTRY_VERIFY_WITH_TIMEOUT(v->document()->findBlockByNumber(lineOf(v, rom[2])).text().startsWith(
                                     QStringLiteral("○")), 2000);
        QCOMPARE(dotColor(lineOf(v, rom[2])), QColor(0x9a, 0x99, 0x96));
        /* a breakpoint on the PC's own line keeps its dot, in the ink on the bar */
        a2600debug_breakpoint_toggle(m_d, (uint16_t)rom[0], bank);
        QTRY_VERIFY_WITH_TIMEOUT(v->document()->findBlockByNumber(lineOf(v, rom[0])).text().startsWith(
                                     QStringLiteral("●")), 2000);
        QCOMPARE(dotColor(lineOf(v, rom[0])), QColor(Qt::black));
        /* a hardware access is marked in the cycles column */
        QVERIFY(v->toPlainText().contains(QStringLiteral(";2")));
        shot(QStringLiteral("disassembly"));
    }

    void clickSelectsAndGutterToggles()
    {
        QVector<int> rom;
        QVERIFY2(gotoRom(&rom), "the PC at CONFIG's Start");
        m_tabs->setCurrentIndex(1);
        QPlainTextEdit *v = disasmView();
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, rom[1]) >= 0, 2000);
        const int bank = a2600debug_current_bank(m_d);
        /* a click on the text selects (underlined, bold) and sets no breakpoint */
        clickLine(rom[1], 30);
        QVERIFY(!a2600debug_breakpoint_check(m_d, (uint16_t)rom[1], bank));
        {
            bool underlined = false;
            for (const auto &s : v->extraSelections())
                if (s.cursor.blockNumber() == lineOf(v, rom[1]) && s.format.fontUnderline() &&
                    s.format.fontWeight() == QFont::Bold)
                    underlined = true;
            QVERIFY(underlined);
        }
        /* moving to another line moves the selection; no caret-toggled breakpoints */
        clickLine(rom[3], 30);
        QVERIFY(!a2600debug_breakpoint_check(m_d, (uint16_t)rom[1], bank));
        QVERIFY(!a2600debug_breakpoint_check(m_d, (uint16_t)rom[3], bank));
        /* the left edge toggles, a double-click toggles, F9 toggles the selection */
        clickLine(rom[4], 0);
        QVERIFY(a2600debug_breakpoint_check(m_d, (uint16_t)rom[4], bank));
        clickLine(rom[4], 0);
        QVERIFY(!a2600debug_breakpoint_check(m_d, (uint16_t)rom[4], bank));
        clickLine(rom[5], 30, true);
        QVERIFY(a2600debug_breakpoint_check(m_d, (uint16_t)rom[5], bank));
        a2600debug_breakpoint_toggle(m_d, (uint16_t)rom[5], bank);
        clickLine(rom[6], 30);
        QTest::keyClick(m_win, Qt::Key_F9);
        QVERIFY(a2600debug_breakpoint_check(m_d, (uint16_t)rom[6], bank));
        QTest::keyClick(m_win, Qt::Key_F9);
        QVERIFY(!a2600debug_breakpoint_check(m_d, (uint16_t)rom[6], bank));
        a2600debug_breakpoint list[4];
        QCOMPARE(a2600debug_bp_list(m_d, list, 4), 0);
    }

    void runToCursor()
    {
        QVector<int> rom;
        QVERIFY2(gotoRom(&rom), "the PC at CONFIG's Start");
        m_tabs->setCurrentIndex(1);
        QPlainTextEdit *v = disasmView();
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, rom[3]) >= 0, 2000);
        clickLine(rom[3], 30);
        QTest::mouseClick(button(QStringLiteral("Run to Cursor")), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(m_d), rom[3], 3000);
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, rom[3]) >= 0, 2000);
    }

    void wheelJumpAndFollow()
    {
        QVector<int> rom;
        QVERIFY2(gotoRom(&rom), "the PC at CONFIG's Start");
        m_tabs->setCurrentIndex(1);
        QPlainTextEdit *v = disasmView();
        QCheckBox *follow = check(QStringLiteral("Follow PC"));
        QVERIFY(follow && follow->isChecked());
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, rom[0]) >= 0, 2000);
        const QPoint p = v->viewport()->rect().center();
        QWheelEvent wheel(p, v->viewport()->mapToGlobal(p), QPoint(), QPoint(0, -120), Qt::NoButton, {},
                          Qt::NoScrollPhase, false);
        QApplication::sendEvent(v->viewport(), &wheel);
        QVERIFY(!follow->isChecked());
        QVERIFY(lineOf(v, rom[0]) < 0 || lineOf(v, rom[0]) < v->document()->blockCount() / 3 - 1);
        /* the jump box: bare digits are hex here, a label wins first, then expressions */
        QLineEdit *jump = entryIs(QStringLiteral("$address or label"));
        QVERIFY(jump);
        const auto hex = [](int a) { return QStringLiteral("%1").arg(a, 4, 16, QLatin1Char('0')).toUpper(); };
        typeInto(jump, hex(rom[5]));
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, rom[5]) >= 0, 2000);
        char out[256];
        a2600debug_command(m_d, QStringLiteral("define back $%1").arg(hex(rom[2])).toUtf8().constData(), out, sizeof out);
        typeInto(jump, QStringLiteral("back"));
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, rom[2]) >= 0, 2000);
        QVERIFY2(v->toPlainText().contains(QStringLiteral("back")), qPrintable(v->toPlainText().left(400)));
        typeInto(jump, QStringLiteral("$") + hex(rom[4]) + QStringLiteral("+0"));
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, rom[4]) >= 0, 2000);
        typeInto(jump, QStringLiteral("nosuchlabel"));
        QVERIFY(!m_win->statusBar()->currentMessage().isEmpty());
        /* Follow PC again returns to the PC */
        QTest::mouseClick(follow, Qt::LeftButton, {}, QPoint(4, follow->height() / 2));
        QVERIFY(follow->isChecked());
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, rom[0]) >= 0, 2000);
        a2600debug_clear_symbols(m_d);
    }

    /* The bank chooser lists the cartridge's banks; a fixed bank is listed
     * without following the PC. */
    void bankChooser()
    {
        m_tabs->setCurrentIndex(1);
        QComboBox *bank = m_tabs->currentWidget()->findChild<QComboBox *>();
        QVERIFY(bank);
        QTRY_COMPARE_WITH_TIMEOUT(bank->count(), a2600debug_bank_count(m_d) + 1, 2000);
        QCOMPARE(bank->itemText(0), QStringLiteral("PC's bank"));
        QVERIFY(bank->count() >= 2);
        QCheckBox *follow = check(QStringLiteral("Follow PC"));
        QVERIFY(follow->isChecked());
        bank->setCurrentIndex(1);
        QVERIFY(!follow->isChecked());
        QPlainTextEdit *v = disasmView();
        QTRY_VERIFY_WITH_TIMEOUT(v->document()->blockCount() > 4, 2000);
        shot(QStringLiteral("bank"));
        /* a breakpoint set from this view belongs to that bank */
        QTRY_VERIFY_WITH_TIMEOUT(v->document()->findBlockByNumber(1).text().mid(2, 4).toInt(nullptr, 16) > 0, 2000);
        const int addr = v->document()->findBlockByNumber(1).text().mid(2, 4).toInt(nullptr, 16);
        clickLine(addr, 0);
        a2600debug_breakpoint bp;
        QCOMPARE(a2600debug_bp_list(m_d, &bp, 1), 1);
        QCOMPARE(bp.bank, 0);
        a2600debug_breakpoint_clear(m_d);
        bank->setCurrentIndex(0);
        follow->setChecked(true);
    }

    void registersFlagsMemory()
    {
        m_tabs->setCurrentIndex(2);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        QList<QLineEdit *> regs;
        for (QLineEdit *e : m_win->findChildren<QLineEdit *>())
            if (e->toolTip().startsWith(QLatin1String("Enter sets it"))) regs << e;
        QCOMPARE(regs.size(), 6);                   /* PC SP A X Y PS */
        QTRY_COMPARE_WITH_TIMEOUT(regs[0]->text(), QStringLiteral("0082"), 2000);   /* PC */
        typeInto(regs[0], QStringLiteral("0087"));
        QCOMPARE(pcOf(m_d), (int)kDey);
        typeInto(regs[2], QStringLiteral("34"));    /* A */
        a2600debug_cpu c;
        a2600debug_cpu_get(m_d, &c);
        QCOMPARE(c.a, 0x34);
        typeInto(regs[4], QStringLiteral("$20+2"));     /* Y, an expression */
        a2600debug_cpu_get(m_d, &c);
        QCOMPARE(c.y, 0x22);
        /* the flags, all seven */
        for (const char *name : { "N", "V", "B", "D", "I", "Z", "C" })
            QVERIFY2(check(QString::fromUtf8(name)), name);
        QCheckBox *carry = check(QStringLiteral("C"));
        a2600debug_cpu_set(m_d, A2600_FLAG_C, 0);
        QTRY_VERIFY_WITH_TIMEOUT(!carry->isChecked(), 2000);
        QTest::mouseClick(carry, Qt::LeftButton, {}, QPoint(6, carry->height() / 2));
        a2600debug_cpu_get(m_d, &c);
        QCOMPARE(c.c, 1);
        QCheckBox *neg = check(QStringLiteral("N"));
        a2600debug_cpu_set(m_d, A2600_FLAG_N, 0);
        QTRY_VERIFY_WITH_TIMEOUT(!neg->isChecked(), 2000);
        QTest::mouseClick(neg, Qt::LeftButton, {}, QPoint(6, neg->height() / 2));
        a2600debug_cpu_get(m_d, &c);
        QCOMPARE(c.n, 1);
        a2600debug_cpu_set(m_d, A2600_FLAG_N, 0);
        a2600debug_cpu_set(m_d, A2600_FLAG_C, 0);
        /* the beam / cycles line */
        bool beam = false;
        for (QLabel *l : m_win->findChildren<QLabel *>())
            if (l->text().startsWith(QLatin1String("scanline"))) beam = l->text().contains(QLatin1String("cycles"));
        QVERIFY(beam);
        /* memory: poke through the map, go to a label or an address */
        QLineEdit *addr = entryIs(QStringLiteral("$80"));
        QLineEdit *val = entryIs(QStringLiteral("$00"));
        QVERIFY(addr && val);
        addr->setText(QStringLiteral("C0"));       /* hex */
        typeInto(val, QStringLiteral("5a"));
        uint8_t b = 0;
        a2600debug_read(m_d, 0xC0, &b, 1);
        QCOMPARE((int)b, 0x5A);
        addr->setText(QStringLiteral("$C1"));
        typeInto(val, QStringLiteral("$A5"));
        a2600debug_read(m_d, 0xC1, &b, 1);
        QCOMPARE((int)b, 0xA5);
        a2600debug_write(m_d, 0xC2, 0x11);
        addr->setText(QStringLiteral("C2"));
        typeInto(val, QStringLiteral("1FF"));        /* not a byte */
        a2600debug_read(m_d, 0xC2, &b, 1);
        QCOMPARE((int)b, 0x11);
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("byte")));
        QPlainTextEdit *mem = memView();
        QLineEdit *go = entryIs(QStringLiteral("$80, label or lo-hi"));
        QVERIFY(go && mem);
        typeInto(go, QStringLiteral("80"));
        /* the 8K are there: the first row and the last, each named by its region */
        QVERIFY(mem->toPlainText().contains(QStringLiteral("$0000: ")));
        QVERIFY(mem->toPlainText().contains(QStringLiteral("$1FF0: ")));
        QVERIFY(mem->toPlainText().contains(QStringLiteral("$0080: A9 00 A0 03")));
        QCOMPARE(mem->document()->blockCount(), 513);
        const QString first = mem->document()->findBlockByNumber(1).text();
        const QString ram = mem->document()->findBlockByNumber(1 + 0x80 / 16).text();
        QVERIFY2(first.contains(QLatin1String("TIA")), qPrintable(first));
        QVERIFY2(ram.contains(QLatin1String("RAM")), qPrintable(ram));
        /* Go to marked it in the accent */
        bool marked = false;
        for (const auto &s : mem->extraSelections())
            marked |= s.format.background().color() == kAccent && s.format.foreground().color() == QColor(Qt::black);
        QVERIFY(marked);
        /* a cartridge address ($F000) is the 13-bit bus's $1000 */
        typeInto(go, QStringLiteral("F000-F00F"));
        QVERIFY(mem->verticalScrollBar()->value() > 0x100 / 16);
        /* edits need a stopped machine */
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(!a2600debug_is_stopped(m_d), 2000);
        typeInto(regs[0], QStringLiteral("0080"));
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("Stop the machine")));
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
        shot(QStringLiteral("memory"));
    }

    void breakpointsTab()
    {
        m_tabs->setCurrentIndex(3);
        QTableWidget *t = bpTable();
        QVERIFY(t);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        QCOMPARE(t->rowCount(), 0);
        QLineEdit *start = entry(QStringLiteral("address, range"));
        QLineEdit *end = entry(QStringLiteral("end"));
        QLineEdit *cond = entry(QStringLiteral("condition"));
        QVERIFY(start && end && cond);
        QList<QComboBox *> combos = m_tabs->currentWidget()->findChildren<QComboBox *>();
        QCOMPARE(combos.size(), 2);
        QComboBox *type = combos[0], *bank = combos[1];
        QPushButton *add = nullptr, *clear = nullptr;
        for (QPushButton *b : m_tabs->currentWidget()->findChildren<QPushButton *>()) {
            if (b->text() == QLatin1String("Add")) add = b;
            if (b->text() == QLatin1String("Clear Breakpoints")) clear = b;
        }
        QLabel *msg = nullptr;
        for (QLabel *l : m_tabs->currentWidget()->findChildren<QLabel *>())
            if (l->styleSheet().contains(QLatin1String("e01b24"))) msg = l;
        QVERIFY(add && clear && msg);
        QStringList types;
        for (int i = 0; i < type->count(); ++i) types << type->itemText(i);
        QCOMPARE(types, (QStringList{ QStringLiteral("Execute"), QStringLiteral("Read"), QStringLiteral("Write"),
                                      QStringLiteral("Access") }));
        QCOMPARE(bank->itemText(0), QStringLiteral("Any bank"));
        QVERIFY(bank->isEnabled());

        /* exec at a hex address */
        typeInto(start, QStringLiteral("84"));
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 1, 2000);
        QVERIFY2(t->item(0, 1)->text().startsWith(QLatin1String("Exec  $0084")), qPrintable(t->item(0, 1)->text()));
        QCOMPARE(t->item(0, 0)->checkState(), Qt::Checked);
        QVERIFY(start->text().isEmpty());
        /* a write trap over a range, with a condition; the bank chooser is for Execute only */
        type->setCurrentText(QStringLiteral("Write"));
        QVERIFY(!bank->isEnabled());
        start->setText(QStringLiteral("B0"));
        end->setText(QStringLiteral("$BF"));
        cond->setText(QStringLiteral("value > 0"));
        QTest::mouseClick(add, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 2, 2000);
        QVERIFY2(t->item(1, 1)->text().endsWith(QLatin1String("  if value > 0")) && t->item(1, 1)->text().startsWith(QLatin1String("Write  $00B0-$00BF")), qPrintable(t->item(1, 1)->text()));
        /* the start may itself be a range */
        cond->clear();
        end->clear();
        type->setCurrentText(QStringLiteral("Access"));
        typeInto(start, QStringLiteral("D0-DF"));
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 3, 2000);
        QVERIFY2(t->item(2, 1)->text().startsWith(QLatin1String("Access  $00D0-$00DF")), qPrintable(t->item(2, 1)->text()));
        type->setCurrentText(QStringLiteral("Read"));
        typeInto(start, QStringLiteral("28"));
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 4, 2000);
        QVERIFY2(t->item(3, 1)->text().startsWith(QLatin1String("Read  $0028")), qPrintable(t->item(3, 1)->text()));
        /* an execute breakpoint in one bank says so */
        type->setCurrentText(QStringLiteral("Execute"));
        bank->setCurrentIndex(1);
        typeInto(start, QStringLiteral("F100"));
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 5, 2000);
        QVERIFY2(t->item(4, 1)->text().contains(QLatin1String("bank 0")), qPrintable(t->item(4, 1)->text()));
        bank->setCurrentIndex(0);
        /* errors are shown, not swallowed */
        const int rowsBefore = t->rowCount();
        cond->setText(QStringLiteral("a == (1 +"));
        start->setText(QStringLiteral("90"));
        end->clear();
        QTest::mouseClick(add, Qt::LeftButton);
        QVERIFY(!msg->text().isEmpty());
        QCOMPARE(t->rowCount(), rowsBefore);
        cond->clear();
        start->setText(QStringLiteral("FF"));
        end->setText(QStringLiteral("FC"));
        QTest::mouseClick(add, Qt::LeftButton);
        QVERIFY(msg->text().contains(QLatin1String("before")));
        start->clear();
        end->clear();
        msg->clear();
        shot(QStringLiteral("breakpoints-types"));

        /* back to two: the exec one and the conditional write */
        QTest::mouseClick(clear, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 0, 2000);
        typeInto(start, QStringLiteral("84"));
        type->setCurrentText(QStringLiteral("Write"));
        start->setText(QStringLiteral("B0"));
        end->setText(QStringLiteral("$BF"));
        cond->setText(QStringLiteral("value > 0"));
        QTest::mouseClick(add, Qt::LeftButton);
        cond->clear();
        end->clear();
        type->setCurrentText(QStringLiteral("Execute"));
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 2, 2000);

        /* the exec breakpoint stops the run, with its hits counted */
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(a2600debug_is_stopped(m_d) && pcOf(m_d) == kJsr, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel()->text().contains(QLatin1String("$0084")), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(t->item(0, 2) && t->item(0, 2)->text().toInt() >= 1, 2000);
        /* unticking it lets the run pass; the write trap then stops it instead */
        t->item(0, 0)->setCheckState(Qt::Unchecked);
        a2600debug_breakpoint list[2];
        QCOMPARE(a2600debug_bp_list(m_d, list, 2), 2);
        QCOMPARE(list[0].enabled, 0);
        QCOMPARE(list[1].enabled, 1);
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(a2600debug_is_stopped(m_d) && statusLabel()->text().contains(QLatin1String("wrote $00B0")),
                                 4000);
        /* Clear Hits zeroes the counts */
        QPushButton *clearHits = nullptr;
        for (QPushButton *b : m_tabs->currentWidget()->findChildren<QPushButton *>())
            if (b->text() == QLatin1String("Clear Hits")) clearHits = b;
        QVERIFY(clearHits);
        QTRY_VERIFY_WITH_TIMEOUT(t->item(1, 2) && t->item(1, 2)->text().toInt() >= 1, 2000);
        QTest::mouseClick(clearHits, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(t->item(1, 2)->text(), QStringLiteral("0"), 2000);
        shot(QStringLiteral("breakpoints"));
        /* the row's delete button */
        auto *rm = qobject_cast<QToolButton *>(t->cellWidget(1, 3));
        QVERIFY(rm);
        QTest::mouseClick(rm, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 1, 2000);
        QCOMPARE(a2600debug_bp_list(m_d, list, 2), 1);
        QTest::mouseClick(clear, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 0, 2000);
        QCOMPARE(a2600debug_bp_list(m_d, list, 2), 0);
    }

    /* A read trap stops after the access. */
    void readWatchpoint()
    {
        QVERIFY(a2600debug_bp_add(m_d, A2600DEBUG_BP_READ, kData, kData, A2600DEBUG_ANY_BANK, "") >= 0);
        m_tabs->setCurrentIndex(3);
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(a2600debug_is_stopped(m_d), 3000);
        QTRY_VERIFY2_WITH_TIMEOUT(statusLabel()->text().contains(QLatin1String("RTrap")), why().constData(), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(bpTable()->item(0, 1) && bpTable()->item(0, 1)->text().startsWith(QLatin1String("Read  $00B0")), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(bpTable()->item(0, 2)->text().toInt() >= 1, 2000);
        a2600debug_breakpoint_clear(m_d);
        m_tabs->setCurrentIndex(0);
    }

    /* Watches: add, live value, Memory, Break on change, Remove. */
    void watchesTab()
    {
        a2600watch_clear();
        a2600debug_write(m_d, 0xC0, 0x42);
        m_tabs->setCurrentIndex(4);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        QTableWidget *t = m_tabs->currentWidget()->findChild<QTableWidget *>();
        QLineEdit *e = m_tabs->currentWidget()->findChild<QLineEdit *>();
        QVERIFY(t && e);
        QCOMPARE(t->rowCount(), 0);
        typeInto(e, QStringLiteral("$C0"));
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 1, 2000);
        QVERIFY(t->item(0, 0)->text().contains(QStringLiteral("$00C0")));
        QVERIFY(t->item(0, 0)->text().contains(QStringLiteral("$42")));
        a2600debug_write(m_d, 0xC0, 0x43);
        QTest::keyClick(m_win, Qt::Key_F7);             /* a stop refreshes the values */
        QTRY_VERIFY_WITH_TIMEOUT(t->item(0, 0)->text().contains(QStringLiteral("$43")), 2000);
        typeInto(e, QStringLiteral("$D0-$DF"));
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 2, 2000);
        /* while the machine runs the value is a dash */
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(!a2600debug_is_stopped(m_d), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(t->item(0, 0)->text().contains(QLatin1String("-")) &&
                                     !t->item(0, 0)->text().contains(QStringLiteral("$43")), 2000);
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
        shot(QStringLiteral("watches"));

        /* Break on change adds a write trap with the condition */
        a2600debug_breakpoint_clear(m_d);
        auto buttonOf = [&](int row, const QString &text) -> QPushButton * {
            for (QPushButton *b : t->cellWidget(row, 1)->findChildren<QPushButton *>())
                if (b->text() == text) return b;
            return nullptr;
        };
        QPushButton *brk = buttonOf(0, QStringLiteral("Break on change"));
        QVERIFY(brk);
        QTest::mouseClick(brk, Qt::LeftButton);
        a2600debug_breakpoint bp[2];
        QCOMPARE(a2600debug_bp_list(m_d, bp, 2), 1);
        QVERIFY(bp[0].type & A2600DEBUG_BP_WRITE);
        QCOMPARE((int)bp[0].start, 0xC0);
        QVERIFY(QString::fromUtf8(bp[0].condition).contains(QStringLiteral("old")));
        a2600debug_breakpoint_clear(m_d);

        /* Memory switches to the Memory tab with the range marked */
        QPushButton *mem = buttonOf(1, QStringLiteral("Memory"));
        QVERIFY(mem);
        QTest::mouseClick(mem, Qt::LeftButton);
        QCOMPARE(m_tabs->currentIndex(), 2);
        QVERIFY(memView()->toPlainText().contains(QStringLiteral("$00D0: ")));
        QVERIFY(!memView()->extraSelections().isEmpty());
        /* and every watch is marked in the dump, in its own colour */
        bool watched = false;
        for (const auto &s : memView()->extraSelections())
            watched |= s.format.background().color() == QColor(0x1c, 0x71, 0xd8);
        QVERIFY(watched);

        m_tabs->setCurrentIndex(4);
        QTest::mouseClick(buttonOf(1, QStringLiteral("Remove")), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 1, 2000);
        QCOMPARE(a2600watch_count(), 1);
        a2600watch_clear();
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 0, 2000);
        m_tabs->setCurrentIndex(0);
    }

    /* Go to marks a range; Find Next / Previous step through hits. */
    void memorySearch()
    {
        const uint8_t pat[4] = { 0xDE, 0xEE, 0xDE, 0xEE };
        for (int i = 0; i < 4; ++i) a2600debug_write(m_d, (uint16_t)(0xD0 + i), pat[i]);
        for (int i = 0; i < 4; ++i) a2600debug_write(m_d, (uint16_t)(0xE0 + i), pat[i]);
        m_tabs->setCurrentIndex(2);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        QLineEdit *go = entryIs(QStringLiteral("$80, label or lo-hi"));
        typeInto(go, QStringLiteral("D0-D3"));
        QVERIFY(memView()->toPlainText().contains(QStringLiteral("$00D0: DE EE DE EE")));
        QVERIFY(!memView()->extraSelections().isEmpty());
        /* a label also works */
        a2600debug_set_label(m_d, 0xE0, "found");
        typeInto(go, QStringLiteral("found"));
        QVERIFY(memView()->toPlainText().contains(QStringLiteral("$00E0: DE EE")));
        a2600debug_set_label(m_d, 0xE0, nullptr);

        QLineEdit *find = entry(QStringLiteral("bytes or text"));
        QVERIFY(find);
        QLabel *note = nullptr;
        for (QLabel *l : m_tabs->currentWidget()->findChildren<QLabel *>())
            if (l->styleSheet().contains(QLatin1String("gray")) && l->text().isEmpty()) note = l;
        QVERIFY(note);
        typeInto(go, QStringLiteral("D0-D3"));
        find->setText(QStringLiteral("DE EE DE"));
        QTest::mouseClick(button(QStringLiteral("Find Next")), Qt::LeftButton);
        QCOMPARE(note->text(), QStringLiteral("Found at $00E0"));          /* after the marked one */
        QTest::mouseClick(button(QStringLiteral("Find Previous")), Qt::LeftButton);
        QCOMPARE(note->text(), QStringLiteral("Found at $00D0"));
        find->setText(QStringLiteral("DE EE DE EE FF 77 66"));
        QTest::mouseClick(button(QStringLiteral("Find Next")), Qt::LeftButton);
        QCOMPARE(note->text(), QStringLiteral("Not found"));
        QCOMPARE(m_win->statusBar()->currentMessage(), QStringLiteral("Not found"));
        /* text, case-insensitive */
        for (int i = 0; i < 5; ++i) a2600debug_write(m_d, (uint16_t)(0xF0 + i), (uint8_t)"HeLLo"[i]);
        m_tabs->currentWidget()->findChild<QComboBox *>()->setCurrentText(QStringLiteral("Text"));
        find->setText(QStringLiteral("hello"));
        typeInto(go, QStringLiteral("80"));
        QTest::mouseClick(button(QStringLiteral("Find Next")), Qt::LeftButton);
        QCOMPARE(note->text(), QStringLiteral("Found at $00F0"));
        shot(QStringLiteral("search"));
        m_tabs->currentWidget()->findChild<QComboBox *>()->setCurrentText(QStringLiteral("Hex"));
        /* it needs a stopped machine */
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(!a2600debug_is_stopped(m_d), 2000);
        find->setText(QStringLiteral("DE EE"));
        QTest::mouseClick(button(QStringLiteral("Find Next")), Qt::LeftButton);
        QCOMPARE(m_win->statusBar()->currentMessage(), QStringLiteral("Stop the machine first"));
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
        m_tabs->setCurrentIndex(0);
    }

    /* Trace and History share the ring and the recorder. */
    void traceAndHistory()
    {
        a2600debug_trace_enable(m_d, 0);
        a2600debug_trace_clear(m_d);
        m_tabs->setCurrentIndex(6);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        QPushButton *rec = m_tabs->currentWidget()->findChild<QPushButton *>();
        QVERIFY(rec && rec->text() == QLatin1String("Record"));
        QPushButton *rec2 = m_tabs->widget(7)->findChild<QPushButton *>();
        QVERIFY(rec2 && rec2 != rec);
        QVERIFY(!rec->isChecked());
        QTest::mouseClick(rec, Qt::LeftButton);
        QVERIFY(a2600debug_trace_enabled(m_d));
        QVERIFY(rec->isChecked());
        QTRY_VERIFY_WITH_TIMEOUT(rec2->isChecked(), 2000);                   /* in sync */
        QTest::keyClick(m_win, Qt::Key_F5);                                  /* runs */
        QTRY_VERIFY_WITH_TIMEOUT(!a2600debug_is_stopped(m_d), 2000);
        auto *tr = m_tabs->currentWidget()->findChild<QPlainTextEdit *>();
        QTRY_VERIFY_WITH_TIMEOUT(tr->toPlainText().contains(QStringLiteral("ADC")), 3000);   /* live */
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
        QTRY_VERIFY_WITH_TIMEOUT(a2600debug_trace_count(m_d) > 20, 2000);
        /* History shows the same ring, oldest first, at the stop */
        m_tabs->setCurrentIndex(7);
        auto *hist = m_tabs->currentWidget()->findChild<QPlainTextEdit *>();
        QTRY_VERIFY_WITH_TIMEOUT(hist->toPlainText().contains(QStringLiteral("ADC")), 2000);
        QVERIFY(hist->toPlainText().count(QLatin1Char('\n')) < 2000);
        /* newest at the bottom: the view is scrolled to its end */
        QVERIFY(hist->verticalScrollBar()->value() >= hist->verticalScrollBar()->maximum() - 1);
        QLabel *note = nullptr;
        for (QLabel *l : m_tabs->currentWidget()->findChildren<QLabel *>())
            if (l->text().contains(QLatin1String("instructions"))) note = l;
        QVERIFY(note && note->text().startsWith(QLatin1String("recording")));
        shot(QStringLiteral("history"));
        /* a branches-only filter leaves jumps, calls and returns */
        auto *filter = m_tabs->currentWidget()->findChild<QComboBox *>();
        filter->setCurrentIndex(1);
        QTRY_VERIFY_WITH_TIMEOUT(!hist->toPlainText().contains(QStringLiteral("ADC")), 2000);
        QVERIFY(hist->toPlainText().contains(QStringLiteral("JMP")));
        /* a range filter */
        filter->setCurrentIndex(2);
        auto *range = m_tabs->currentWidget()->findChild<QLineEdit *>();
        typeInto(range, QStringLiteral("8D-8D"));
        QTRY_VERIFY_WITH_TIMEOUT(hist->toPlainText().contains(QStringLiteral("STA")) &&
                                     !hist->toPlainText().contains(QStringLiteral("ADC")), 2000);
        filter->setCurrentIndex(0);
        /* Clear */
        for (QPushButton *b : m_tabs->currentWidget()->findChildren<QPushButton *>())
            if (b->text() == QLatin1String("Clear")) QTest::mouseClick(b, Qt::LeftButton);
        QCOMPARE(a2600debug_trace_count(m_d), 0);
        QTRY_VERIFY_WITH_TIMEOUT(!hist->toPlainText().contains(QStringLiteral("ADC")), 2000);
        /* and off, from either tab */
        QTest::mouseClick(m_tabs->widget(7)->findChild<QPushButton *>(), Qt::LeftButton);
        QVERIFY(!a2600debug_trace_enabled(m_d));
        QTRY_VERIFY_WITH_TIMEOUT(!m_tabs->widget(6)->findChild<QPushButton *>()->isChecked(), 2000);
        m_tabs->setCurrentIndex(0);
    }

    /* Any tab can leave for a window of its own and come back to its place. */
    void popOutAndDock()
    {
        m_tabs->setCurrentIndex(3);
        QPushButton *pop = button(QStringLiteral("Pop Out"));
        QVERIFY(pop);
        QTest::mouseClick(pop, Qt::LeftButton);
        QCOMPARE(m_tabs->count(), 11);
        QWidget *win = nullptr;
        for (QWidget *w : QApplication::topLevelWidgets())
            if (w->windowTitle() == QLatin1String("Atari 2600 Debugger - Breakpoints")) win = w;
        QVERIFY(win);
        QTRY_VERIFY_WITH_TIMEOUT(win->isVisible(), 1000);
        QVERIFY(win->findChild<QTableWidget *>());
        QCOMPARE(win->property("a2600HelpContext").toString(), QStringLiteral("debugger/breakpoints"));
        /* the popped tab keeps refreshing: a breakpoint added shows in its window */
        a2600debug_breakpoint_toggle(m_d, kJsr, A2600DEBUG_ANY_BANK);
        QTRY_COMPARE_WITH_TIMEOUT(win->findChild<QTableWidget *>()->rowCount(), 1, 2000);
        a2600debug_breakpoint_clear(m_d);
        shot(QStringLiteral("popout"));
        QPushButton *dockBtn = nullptr;
        for (QPushButton *b : win->findChildren<QPushButton *>())
            if (b->text() == QLatin1String("Dock")) dockBtn = b;
        QVERIFY(dockBtn);
        QTest::mouseClick(dockBtn, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(m_tabs->count(), 12, 1000);
        QCOMPARE(m_tabs->tabText(3), QStringLiteral("Breakpoints"));       /* back in its place */
        /* closing the window docks it too; several can be out, but one stays */
        m_tabs->setCurrentIndex(5);
        QTest::mouseClick(pop, Qt::LeftButton);
        QCOMPARE(m_tabs->count(), 11);
        QCOMPARE(m_tabs->tabText(5), QStringLiteral("Trace"));
        QWidget *cart = nullptr;
        for (QWidget *w : QApplication::topLevelWidgets())
            if (w->windowTitle() == QLatin1String("Atari 2600 Debugger - Cart & I/O")) cart = w;
        QVERIFY(cart);
        cart->close();
        QTRY_COMPARE_WITH_TIMEOUT(m_tabs->count(), 12, 1000);
        QCOMPARE(m_tabs->tabText(5), QStringLiteral("Cart && I/O"));
        for (int i = 0; i < 11; ++i) {
            m_tabs->setCurrentIndex(0);
            QTest::mouseClick(pop, Qt::LeftButton);
        }
        QCOMPARE(m_tabs->count(), 1);
        QTest::mouseClick(pop, Qt::LeftButton);                          /* the last stays */
        QCOMPARE(m_tabs->count(), 1);
        for (QWidget *w : QApplication::topLevelWidgets())
            if (w->windowTitle().startsWith(QLatin1String("Atari 2600 Debugger - "))) w->close();
        QTRY_COMPARE_WITH_TIMEOUT(m_tabs->count(), 12, 1000);
        QStringList names;
        for (int i = 0; i < m_tabs->count(); ++i) names << m_tabs->tabText(i);
        QCOMPARE(names.first(), QStringLiteral("Console"));
        QCOMPARE(names.last(), QStringLiteral("FujiNet"));
        QCOMPARE(names.at(5), QStringLiteral("Cart && I/O"));
        QCOMPARE(names.at(10), QStringLiteral("Symbols"));
    }

    /* A new stop pulls every view back to the spot from wherever it was
     * scrolled: the disassembly to the PC, Memory to the PC, the Trace and
     * History to their newest line; and the Breakpoints tab says why. */
    void stopRecentresAndNames()
    {
        QVector<int> rom;
        QVERIFY2(gotoRom(&rom), "the PC at CONFIG's Start");
        m_tabs->setCurrentIndex(1);
        QCheckBox *follow = check(QStringLiteral("Follow PC"));
        QPlainTextEdit *v = disasmView();
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, rom[0]) >= 0, 2000);
        const QPoint p = v->viewport()->rect().center();
        QWheelEvent wheel(p, v->viewport()->mapToGlobal(p), QPoint(), QPoint(0, -600), Qt::NoButton, {},
                          Qt::NoScrollPhase, false);
        QApplication::sendEvent(v->viewport(), &wheel);
        QVERIFY(!follow->isChecked());
        /* Memory scrolled to the top */
        m_tabs->setCurrentIndex(2);
        memView()->verticalScrollBar()->setValue(0);
        m_tabs->setCurrentIndex(1);
        QTest::keyClick(m_win, Qt::Key_F7);
        QTRY_VERIFY_WITH_TIMEOUT(follow->isChecked(), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, pcOf(m_d)) >= 0, 2000);
        m_tabs->setCurrentIndex(3);
        QLabel *stop = nullptr;
        for (QLabel *l : m_tabs->currentWidget()->findChildren<QLabel *>())
            if (l->text().startsWith(QLatin1String("Stopped"))) stop = l;
        QVERIFY(stop);
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(stop->text() == QLatin1String("Running."), 2000);
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
        QVERIFY(button(QStringLiteral("Clear Hits")));
        m_tabs->setCurrentIndex(0);
    }

    /* Cart & I/O: the cartridge, the RIOT, the memory map once, the state slots and the file saves. */
    void cartAndIo()
    {
        m_tabs->setCurrentIndex(5);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        auto *io = m_tabs->currentWidget()->findChild<QPlainTextEdit *>();
        QVERIFY(io);
        QTRY_VERIFY_WITH_TIMEOUT(!io->toPlainText().isEmpty(), 2000);
        const QString text = io->toPlainText();
        QCOMPARE(text.count(QStringLiteral("Memory map")), 1);
        QVERIFY(text.contains(QStringLiteral("Cartridge")));
        QVERIFY(text.contains(QStringLiteral("SWCHA")));
        QVERIFY(text.contains(QStringLiteral("INPT0-5")));
        QVERIFY(text.contains(QStringLiteral("INTIM")));
        QVERIFY(text.contains(QStringLiteral("$0000-$003F")));
        QVERIFY(text.contains(QStringLiteral("RIOT RAM")));
        /* the save-state slots, and the file saves */
        int slotButtons = 0;
        QPushButton *save3 = nullptr, *load3 = nullptr;
        for (QPushButton *b : m_tabs->currentWidget()->findChildren<QPushButton *>()) {
            if (b->toolTip().startsWith(QLatin1String("Save state slot"))) ++slotButtons;
            if (b->toolTip().startsWith(QLatin1String("Load state slot"))) ++slotButtons;
            if (b->toolTip() == QLatin1String("Save state slot 3")) save3 = b;
            if (b->toolTip() == QLatin1String("Load state slot 3")) load3 = b;
        }
        QCOMPARE(slotButtons, 20);
        QVERIFY(save3 && load3);
        for (const char *label : { "Disassembly", "ROM (patched)", "Access counters", "Session", "TIA snapshot" })
            QVERIFY2(button(QString::fromUtf8(label)), label);
        /* a state saved, scribbled over, and loaded back */
        a2600debug_write(m_d, 0xC8, 0x11);
        QTest::mouseClick(save3, Qt::LeftButton);
        a2600debug_write(m_d, 0xC8, 0x22);
        QTest::mouseClick(load3, Qt::LeftButton);
        uint8_t b = 0;
        a2600debug_read(m_d, 0xC8, &b, 1);
        QCOMPARE((int)b, 0x11);
        shot(QStringLiteral("cart"));
    }

    void soundAndFujinet()
    {
        /* Sound: the TIA's two channels */
        m_tabs->setCurrentIndex(9);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        auto *sound = qobject_cast<QPlainTextEdit *>(m_tabs->currentWidget());
        QVERIFY(sound);
        QTRY_VERIFY_WITH_TIMEOUT(sound->toPlainText().contains(QStringLiteral("TIA")), 2000);
        QVERIFY(sound->toPlainText().contains(QStringLiteral("Channel 0")));
        QVERIFY(sound->toPlainText().contains(QStringLiteral("Channel 1")));
        QVERIFY(sound->toPlainText().contains(QStringLiteral("AUDF")));
        shot(QStringLiteral("sound"));
        /* FujiNet: empty without the runtime, and says so */
        m_tabs->setCurrentIndex(11);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        bool note = false;
        for (QLabel *l : m_tabs->currentWidget()->findChildren<QLabel *>())
            note |= l->text().contains(QLatin1String("FujiNet"));
        QVERIFY(note);
        QVERIFY(m_tabs->currentWidget()->findChild<QPushButton *>()->text() == QLatin1String("Clear"));
        shot(QStringLiteral("fujinet"));
        m_tabs->setCurrentIndex(0);
    }

    /* The Video tab on the real CONFIG display (its TIA frame, its objects):
     * views, the Decoded source, zoom, overlays, hover and pin, the tables,
     * Break on writes, Break on a colour register's write, the Writes table,
     * and setting a TIA register. A fresh machine is run for a few frames and
     * stopped at a frame boundary. */
    void videoTab()
    {
        a2600debug_breakpoint_clear(m_d);
        m_tabs->setCurrentIndex(0);
        a2600session_stop(m_s);
        QCOMPARE(a2600session_start(m_s, &m_opts), 0);
        QTest::qWait(200);
        if (a2600debug_is_stopped(m_d)) QTest::keyClick(m_win, Qt::Key_F5);      /* the new machine runs */
        QTRY_VERIFY_WITH_TIMEOUT(!a2600debug_is_stopped(m_d), 2000);
        a2600debug_tia frames;
        QTRY_VERIFY_WITH_TIMEOUT((a2600debug_tia_get(m_d, &frames), frames.frame_count >= 120), 10000);
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
        QTest::mouseClick(button(QStringLiteral("Frame+1")), Qt::LeftButton);    /* a frame: stops at its end */
        waitStopped();
        QTest::qWait(100);

        m_tabs->setCurrentIndex(8);
        QWidget *page = m_tabs->currentWidget();
        QTRY_VERIFY_WITH_TIMEOUT(page->isVisible(), 1000);
        auto combos = page->findChildren<QComboBox *>();
        QVERIFY(combos.size() >= 3);
        QComboBox *view = combos[0], *source = combos[1], *zoom = combos[2];
        QStringList views;
        for (int i = 0; i < view->count(); ++i) views << view->itemText(i);
        QCOMPARE(views, (QStringList{ QStringLiteral("Display"), QStringLiteral("Decoded"), QStringLiteral("Beam"),
                                      QStringLiteral("Values") }));
        QCOMPARE(source->itemText(0), QStringLiteral("Objects"));
        QCOMPARE(source->count(), 2);
        QVERIFY(!source->isEnabled());                      /* only Decoded has a source */
        QCOMPARE(zoom->itemText(0), QStringLiteral("Fit"));
        QWidget *pic = page->findChild<QWidget *>(QStringLiteral("videoPicture"));
        QVERIFY(pic);
        QTRY_VERIFY_WITH_TIMEOUT(pic->width() >= 100 && pic->height() >= 100, 2000);
        shot(QStringLiteral("video-display"));

        /* a change of view redraws */
        view->setCurrentIndex(1);
        QVERIFY(source->isEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(pic->width() >= 100, 1000);
        shot(QStringLiteral("video-decoded"));
        for (int i = 1; i < source->count(); ++i) {
            source->setCurrentIndex(i);
            QTest::qWait(30);
            QTRY_VERIFY_WITH_TIMEOUT(pic->width() >= 100, 1000);
            shot(QStringLiteral("video-decoded-%1").arg(i));
        }
        source->setCurrentIndex(0);
        view->setCurrentIndex(2);
        QTest::qWait(30);
        QTRY_VERIFY_WITH_TIMEOUT(pic->width() >= 100, 1000);     /* the machine is stopped: Beam works */
        shot(QStringLiteral("video-beam"));
        view->setCurrentIndex(3);
        QTest::qWait(30);
        shot(QStringLiteral("video-values"));
        view->setCurrentIndex(0);

        /* zoom resizes the picture */
        zoom->setCurrentIndex(zoom->findText(QStringLiteral("1×")));
        QTest::qWait(50);
        const int w1 = pic->width();
        zoom->setCurrentIndex(zoom->findText(QStringLiteral("2×")));
        QTRY_VERIFY2_WITH_TIMEOUT(pic->width() == w1 * 2, qPrintable(QStringLiteral("1x %1 2x %2").arg(w1).arg(pic->width())), 1000);
        zoom->setCurrentIndex(zoom->findText(QStringLiteral("1×")));

        /* an overlay checkbox */
        QCheckBox *positions = nullptr;
        for (QCheckBox *box : page->findChildren<QCheckBox *>())
            if (box->text().startsWith(QLatin1String("Object positions"))) positions = box;
        QVERIFY(positions);
        positions->setChecked(true);
        QTest::qWait(50);
        shot(QStringLiteral("video-positions"));
        positions->setChecked(false);

        /* hover shows, a click pins, the same spot unpins */
        auto *tabs = page->findChild<QTabWidget *>();
        QVERIFY(tabs);
        QCOMPARE(tabs->tabText(0), QStringLiteral("Pixel"));
        QStringList sections;
        for (int i = 0; i < tabs->count(); ++i) sections << tabs->tabText(i);
        QCOMPARE(sections, (QStringList{ QStringLiteral("Pixel"), QStringLiteral("Registers"), QStringLiteral("Colours"),
                                         QStringLiteral("Lines"), QStringLiteral("Writes"), QStringLiteral("Timing") }));
        tabs->setCurrentIndex(0);
        QLabel *pixel = nullptr;
        for (QLabel *l : tabs->widget(0)->findChildren<QLabel *>()) pixel = l;
        QVERIFY(pixel);
        QVERIFY(pixel->text().contains(QLatin1String("Move over")));
        const QPoint at(pic->width() / 2, pic->height() / 2);
        QTest::mouseMove(pic, at);
        QTRY_VERIFY_WITH_TIMEOUT(!pixel->text().contains(QLatin1String("Move over")), 1000);
        QTest::mouseClick(pic, Qt::LeftButton, {}, at);
        QTest::mouseMove(pic, at + QPoint(30, 30));
        QVERIFY(!pixel->text().contains(QLatin1String("Move over")));   /* the pin stays */
        shot(QStringLiteral("video-pixel"));
        QTest::mouseClick(pic, Qt::LeftButton, {}, at);
        QTest::mouseMove(pic, QPoint(-5, -5));

        /* the tables */
        QTableWidget *regs = nullptr, *lines = nullptr, *colours = nullptr, *writes = nullptr;
        for (QTableWidget *t : page->findChildren<QTableWidget *>()) {
            if (t->horizontalHeaderItem(0) && t->horizontalHeaderItem(0)->text() == QLatin1String("Name")) regs = t;
            if (t->horizontalHeaderItem(0) && t->horizontalHeaderItem(0)->text() == QLatin1String("Row")) lines = t;
            if (t->horizontalHeaderItem(1) && t->horizontalHeaderItem(1)->text() == QLatin1String("Object")) colours = t;
            if (t->horizontalHeaderItem(0) && t->horizontalHeaderItem(0)->text() == QLatin1String("Scanline")
                && t->horizontalHeaderItem(3) && t->horizontalHeaderItem(3)->text() == QLatin1String("Register")) writes = t;
        }
        QVERIFY(regs && lines && colours && writes);
        tabs->setCurrentIndex(1);
        QTRY_VERIFY_WITH_TIMEOUT(regs->rowCount() > 0, 1000);
        shot(QStringLiteral("video-registers"));
        tabs->setCurrentIndex(2);
        QTRY_COMPARE_WITH_TIMEOUT(colours->rowCount(), 7, 1000);          /* BK PF P0 P1 M0 M1 BL */
        shot(QStringLiteral("video-colours"));
        tabs->setCurrentIndex(3);
        QTRY_VERIFY_WITH_TIMEOUT(lines->rowCount() > 0, 1000);
        shot(QStringLiteral("video-lines"));
        tabs->setCurrentIndex(4);
        QTRY_VERIFY_WITH_TIMEOUT(writes->rowCount() > 10, 1000);
        shot(QStringLiteral("video-writes"));
        tabs->setCurrentIndex(5);
        QTest::qWait(50);
        shot(QStringLiteral("video-timing"));

        /* Break on write, per colour register (the playfield colour: COLUPF = $08) */
        tabs->setCurrentIndex(2);
        auto *swatchButton = qobject_cast<QPushButton *>(colours->cellWidget(1, 6));
        QVERIFY(swatchButton);
        a2600debug_breakpoint bps[16];
        QCOMPARE(a2600debug_bp_list(m_d, bps, 16), 0);
        QTest::mouseClick(swatchButton, Qt::LeftButton);
        QCOMPARE(a2600debug_bp_list(m_d, bps, 16), 1);
        QVERIFY(bps[0].type & A2600DEBUG_BP_WRITE);
        QCOMPARE((int)bps[0].start, 0x08);
        a2600debug_breakpoint_clear(m_d);
        /* a write double-clicked in the Writes table traps its register */
        tabs->setCurrentIndex(4);
        QTRY_VERIFY_WITH_TIMEOUT(writes->rowCount() > 0, 1000);
        const unsigned reg = writes->item(0, 3)->data(Qt::UserRole).toUInt();
        QMetaObject::invokeMethod(writes, "cellDoubleClicked", Q_ARG(int, 0), Q_ARG(int, 3));
        QCOMPARE(a2600debug_bp_list(m_d, bps, 16), 1);
        QCOMPARE((int)bps[0].start, (int)reg);
        a2600debug_breakpoint_clear(m_d);

        /* the Registers section sets a TIA register by name, and fires a strobe */
        tabs->setCurrentIndex(1);
        QLineEdit *setReg = page->findChild<QLineEdit *>(QString(), Qt::FindChildrenRecursively);
        for (QLineEdit *e : page->findChildren<QLineEdit *>())
            if (e->placeholderText().startsWith(QLatin1String("colup0"))) setReg = e;
        QLineEdit *setVal = nullptr;
        for (QLineEdit *e : page->findChildren<QLineEdit *>())
            if (e->placeholderText() == QLatin1String("$1E")) setVal = e;
        QVERIFY(setReg && setVal);
        setVal->setText(QStringLiteral("$7C"));
        typeInto(setReg, QStringLiteral("colubk"));
        a2600debug_tia tia;
        a2600debug_tia_get(m_d, &tia);
        QCOMPARE((int)tia.colubk, 0x7C);
        setVal->clear();
        typeInto(setReg, QStringLiteral("cxclr"));          /* a strobe takes no value */
        QVERIFY(!pixel->text().isEmpty());

        /* Break on writes: write traps on the inspected pixel's registers */
        tabs->setCurrentIndex(0);
        QPushButton *brk = nullptr;
        for (QPushButton *b : page->findChildren<QPushButton *>())
            if (b->text() == QLatin1String("Break on writes")) brk = b;
        QVERIFY(brk);
        int made = 0;
        for (int row = 1; row < 9 && !made; ++row)              /* a pixel the display uses */
            for (int col = 1; col < 5 && !made; ++col) {
                const QPoint spot(pic->width() * col / 5, pic->height() * row / 9);
                QTest::mouseMove(pic, spot);
                QTest::mouseClick(pic, Qt::LeftButton, {}, spot);
                QTest::mouseClick(brk, Qt::LeftButton);
                made = a2600debug_bp_list(m_d, bps, 16);
            }
        QVERIFY2(made >= 1, "Break on writes made no breakpoint on any pixel");
        for (int i = 0; i < made; ++i) QVERIFY(bps[i].type & A2600DEBUG_BP_WRITE);
        shot(QStringLiteral("video-pixel-break"));
        a2600debug_breakpoint_clear(m_d);
        zoom->setCurrentIndex(0);
        view->setCurrentIndex(0);
        m_tabs->setCurrentIndex(0);
    }

    /* The main window's F12 (display focused: its ShortcutOverride lets the
     * chord through to the View menu's action) and the View menu entry. */
    void f12FromTheMainWindow()
    {
        /* the display is a QRhiWidget; with no GPU device (CI's offscreen Qt)
         * the window swaps in the software display, so ask for whichever
         * is there */
        QVERIFY(QFile::exists(QStringLiteral(":/shaders/display.vert.qsb")));
        QVERIFY(QFile::exists(QStringLiteral(":/shaders/display.frag.qsb")));
        MainWindow main(m_s);
        main.show();
        QVERIFY(QTest::qWaitForWindowExposed(&main));
        main.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&main));
        QTest::qWait(300);   /* let a queued renderFailed swap the display */
        QWidget *display = main.centralWidget();
        QVERIFY(display);
        display->setFocus();
        QTRY_VERIFY_WITH_TIMEOUT(QApplication::focusWidget() == display, 2000);
        QCOMPARE(QApplication::focusWidget(), display);
        QVERIFY(m_win->isVisible());
        /* F12 in the debugger hides it (the machine was stopped when it came up, so it stays stopped) */
        QTest::keyClick(m_win, Qt::Key_F12);
        QVERIFY(!m_win->isVisible());
        main.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&main));
        QTest::keyClick(display, Qt::Key_F12);
        QVERIFY(m_win->isVisible());
        waitStopped();
        /* the menu entry shows it again, stopping the machine */
        QAction *entry = nullptr;
        for (QAction *a : main.findChildren<QAction *>())
            if (a->text() == QLatin1String("&Debugger")) entry = a;
        QVERIFY(entry);
        QCOMPARE(entry->shortcut(), QKeySequence(Qt::Key_F12));
        a2600debug_resume(m_d);
        entry->trigger();
        QVERIFY(m_win->isVisible());
        waitStopped();
    }

    /* Hiding the window lets the machine go on if it was running when the
     * window came up; showing it stops the machine. */
    void hideResumes()
    {
        QVERIFY(m_win->isVisible());
        m_win->hide();
        QVERIFY(!m_win->isVisible());
        a2600debug_resume(m_d);
        DebuggerWindow::toggleFor(nullptr, m_s);                /* shown: stops it */
        QVERIFY(m_win->isVisible());
        waitStopped();
        DebuggerWindow::toggleFor(nullptr, m_s);                /* hidden: it was running, so it runs on */
        QVERIFY(!m_win->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(!a2600debug_is_stopped(m_d), 2000);
        DebuggerWindow::toggleFor(nullptr, m_s);
        QVERIFY(m_win->isVisible());
        waitStopped();
    }
};

QTEST_MAIN(DebuggerTest)
#include "debugger_test.moc"
