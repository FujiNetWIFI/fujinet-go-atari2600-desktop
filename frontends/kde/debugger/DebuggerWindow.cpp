/*
 * Debugger window (Qt6 Widgets) over Stella's own debugger engine, via
 * core/include/a2600debug.h. Mirrors the GNOME one tab for tab, and follows
 * the family's KDE debuggers.
 *
 * Tabs, in this order (A2600_DEBUGGER_TAB picks the one a window opens on, by
 * index or by name -- 0 console, 1 disassembly, 2 basic, 3 memory, 4 breakpoints,
 * 5 watches, 6 cart, 7 trace, 8 history, 9 video, 10 sound, 11 symbols,
 * 12 fujinet -- or by the start of a label): the Console (Stella's command
 * parser, with a history and Tab completion), the Disassembly (one bank's
 * DiStella listing, a bank chooser; click a line to select it for Run to
 * Cursor; click its left edge, double-click it or press F9 for its
 * breakpoint), BASIC (an imported batari Basic source, one row a line: click
 * a line for its breakpoint; Step, Step Over and Step Out go by line while it
 * shows; A2600_BASIC_IMPORT / A2600_BASIC_BREAK=name.bas:line for captures),
 * Memory (the 8K the 6507 sees, Go to a label / address /
 * range, a hex or text search, Poke), Breakpoints (execute, read, write and
 * access, with a bank for execute and a condition), Watches (a value list,
 * Break on change), Cart & I/O (the cartridge, the RIOT, the memory map, the
 * save-state slots and the save-to-file buttons), Trace and History (the
 * instruction ring: live, and read at a stop), Video (the TIA's pictures,
 * registers, colours, lines and writes), Sound (the TIA's two channels),
 * Symbols (every label, filtered, and Define) and FujiNet (the FujiBus
 * packets on the cartridge's link). The registers sit in a strip under the
 * tabs, on every one of them. Any tab can be popped out into a window of its
 * own (A2600_DEBUGGER_POPOUT names one at start). The toolbar carries the
 * stepping controls, Break at and symbol loading; the family's keys apply (F5
 * run/stop, F7 step, F8 step over, Shift+F8 step out, F9 breakpoint, F12
 * close).
 *
 * Showing the window stops the machine (as on every sibling); hiding it
 * lets the machine run on if it was running.
 *
 * A short timer polls the engine's generation and stop serial and refreshes
 * the tab showing when they moved; while the machine runs, the live tabs
 * refresh every 400 ms. Every new stop re-centres every view on the spot.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "DebuggerWindow.h"
#include "VideoTab.h"
#include "../FujiNetWindows.h"
#include "../help/HelpWindow.h"

extern "C" {
#include "a2600memtools.h"
}

#include <QCloseEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QIcon>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPointer>
#include <QRegularExpression>
#include <QScrollBar>
#include <QShortcut>
#include <QShowEvent>
#include <QSplitter>
#include <QStatusBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QThread>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <cstdio>
#include <cstring>
#include <utility>


#define DISASM_WINDOW 64        /* lines fetched; as many show as fit */
#define MEM_ROWS 512            /* the 8K the 6507 sees, 16 bytes a row */
#define MEM_ROW_TEXT 140        /* bytes of text a Memory row can take */
#define BP_MAX 256
#define TRACE_MAX 512
#define HIST_SHOWN 2000         /* trace lines shown */
#define RING_MAX 65536
#define CONSOLE_LINES 5000
#define REGION_MAX 128

namespace {

enum {
    TAB_CONSOLE, TAB_DISASM, TAB_BASIC, TAB_MEMORY, TAB_BREAKS, TAB_WATCHES, TAB_CART, TAB_TRACE, TAB_HISTORY, TAB_VIDEO,
    TAB_SOUND, TAB_SYMBOLS, TAB_FUJINET, TAB_COUNT
};

/* The tabs, in order: the names A2600_DEBUGGER_TAB takes, their labels (which
 * it takes too), and their Help contexts (debugger/<help>). */
const struct { const char *name, *label, *help; } kTabs[TAB_COUNT] = {
    { "console", "Console", "console" },           { "disassembly", "Disassembly", "disassembly" },
    { "basic", "BASIC", "basic" },
    { "memory", "Memory", "memory" },              { "breakpoints", "Breakpoints", "breakpoints" },
    { "watches", "Watches", "watches" },           { "cart", "Cart & I/O", "cart" },
    { "trace", "Trace", "trace" },                 { "history", "History", "history" },
    { "video", "Video", "video" },                 { "sound", "Sound", "sound" },
    { "symbols", "Symbols", "symbols" },           { "fujinet", "FujiNet", "fujinet" },
};

/* The register entries, in the order shown, and the a2600debug_reg each sets;
 * every one shows two or four hex digits. The 6507 has a 13-bit bus, but PC is
 * a 16-bit register. */
const struct { const char *name; int id, digits; } kRegs[6] = {
    { "PC", A2600_REG_PC, 4 }, { "SP", A2600_REG_SP, 2 }, { "A", A2600_REG_A, 2 },
    { "X", A2600_REG_X, 2 },   { "Y", A2600_REG_Y, 2 },   { "PS", A2600_REG_PS, 2 },
};

/* The flags of PS the engine can set. */
const struct { const char *name; int id; } kFlags[7] = {
    { "N", A2600_FLAG_N }, { "V", A2600_FLAG_V }, { "B", A2600_FLAG_B }, { "D", A2600_FLAG_D },
    { "I", A2600_FLAG_I }, { "Z", A2600_FLAG_Z }, { "C", A2600_FLAG_C },
};

/* The breakpoint types the Breakpoints tab offers, in the order of its chooser. */
const struct { const char *label; int type; } kBpTypes[4] = {
    { "Execute", A2600DEBUG_BP_EXEC }, { "Read", A2600DEBUG_BP_READ },
    { "Write", A2600DEBUG_BP_WRITE },  { "Access", A2600DEBUG_BP_ACCESS },
};

const QString kGray = QStringLiteral("color: gray;");

/* The PC's line and a marked memory row: the 2600 orange, with black ink: the
 * session's dark accent (A2600SESSION_ACCENT_DARK_RGB) is only 2.5:1 against it. */
QColor accentColor()
{
    return a2600AccentColor();
}

QColor accentInk()
{
    return QColor(Qt::black);
}

QTextCharFormat accentFormat()
{
    QTextCharFormat f;
    f.setBackground(accentColor());
    f.setForeground(accentInk());
    return f;
}

/* A watched range in the dump: a blue the accent does not hide. */
QTextCharFormat watchFormat()
{
    QTextCharFormat f;
    f.setBackground(QColor(0x1c, 0x71, 0xd8));
    f.setForeground(Qt::white);
    return f;
}

void setMono(QWidget *w)
{
    QFont f = w->font();
    f.setFamily(QStringLiteral("monospace"));
    f.setStyleHint(QFont::TypeWriter);
    w->setFont(f);
}

QPlainTextEdit *monoView()
{
    auto *v = new QPlainTextEdit;
    v->setReadOnly(true);
    setMono(v);
    v->setLineWrapMode(QPlainTextEdit::NoWrap);
    return v;
}

/* Engine text for a view: control bytes dropped. */
QString stripControl(const char *s)
{
    const QString in = QString::fromUtf8(s);
    QString out;
    out.reserve(in.size());
    for (const QChar c : in)
        if (c.unicode() >= 0x20 || c == QLatin1Char('\n') || c == QLatin1Char('\t')) out += c;
    return out;
}

QString hx(unsigned v, int digits)
{
    return QStringLiteral("%1").arg(v, digits, 16, QLatin1Char('0')).toUpper();
}

/* "Stopped" and the engine's reason: "stopped at $0038" reads "Stopped at
 * $0038", anything else "Stopped: <reason>". */
QString stoppedText(const QString &reason)
{
    if (reason.isEmpty()) return QStringLiteral("Stopped");
    if (reason.startsWith(QLatin1String("stopped"))) return QStringLiteral("S") + reason.mid(1);
    return QStringLiteral("Stopped: ") + reason;
}

QLabel *titleLabel(const QString &text)
{
    auto *l = new QLabel(text);
    QFont f = l->font();
    f.setBold(true);
    l->setFont(f);
    return l;
}

QLabel *hintLabel(const QString &text)
{
    auto *l = new QLabel(text);
    l->setStyleSheet(kGray);
    l->setWordWrap(true);
    return l;
}

QLineEdit *smallEntry(const QString &placeholder, int chars)
{
    auto *e = new QLineEdit;
    e->setPlaceholderText(placeholder);
    e->setFixedWidth(e->fontMetrics().averageCharWidth() * chars + 16);
    return e;
}

/* A register entry exactly wide enough for its hex digits. */
void setDigits(QLineEdit *e, int digits)
{
    e->setFixedWidth(e->fontMetrics().horizontalAdvance(QString(digits, QLatin1Char('0'))) + 16);
}

/* A toggle takes the checked style (the accent, a box of its own), which is
 * not the native button's: one size for both states and every text,
 * so it never jumps. */
void fixToggleSize(QPushButton *b, const QStringList &texts, int minHeight)
{
    b->ensurePolished();
    QSize s(0, minHeight);
    for (const bool on : { false, true })
        for (const QString &t : texts) {
            b->setChecked(on);
            b->setText(t);
            s = s.expandedTo(b->sizeHint());
        }
    b->setChecked(false);
    b->setText(texts.first());
    b->setFixedSize(s);
}

/* Whether a view's reader is at its end (a log then follows new lines). */
bool atEnd(QPlainTextEdit *v)
{
    return v->verticalScrollBar()->value() >= v->verticalScrollBar()->maximum() - 1;
}

/* Replaces a view's text where the reader left it (a refresh must not throw
 * a scrolled view back to its top), or, following, at its end. */
void setText(QPlainTextEdit *v, const QString &text, bool follow = false)
{
    if (v->toPlainText() == text) {
        if (follow) v->verticalScrollBar()->setValue(v->verticalScrollBar()->maximum());
        return;
    }
    const int top = v->verticalScrollBar()->value(), left = v->horizontalScrollBar()->value();
    v->setPlainText(text);
    v->verticalScrollBar()->setValue(follow ? v->verticalScrollBar()->maximum() : top);
    v->horizontalScrollBar()->setValue(left);
}

/* A stretch of one line of a view in a format. */
QTextEdit::ExtraSelection spanSelection(QPlainTextEdit *v, int line, int from, int length, const QTextCharFormat &fmt)
{
    QTextEdit::ExtraSelection s;
    const QTextBlock block = v->document()->findBlockByNumber(line);
    s.cursor = QTextCursor(block);
    s.cursor.setPosition(block.position() + from);
    s.cursor.setPosition(block.position() + from + length, QTextCursor::KeepAnchor);
    s.format = fmt;
    return s;
}

/* A2600_DEBUGGER_TAB: a page number, or a tab's name or label (any case), or
 * the start of one, or of a word of a label ("i/o" ...). The names of the
 * tabs the older windows had still reach their successors. */
int tabFromEnv(const char *var = "A2600_DEBUGGER_TAB")
{
    const QString tab = qEnvironmentVariable(var).trimmed();
    if (tab.isEmpty()) return -1;
    static const struct { const char *name; int tab; } aliases[] = {
        { "io", TAB_CART }, { "cartridge", TAB_CART }, { "prompt", TAB_CONSOLE }, { "cpu", TAB_MEMORY },
        { "tia", TAB_VIDEO }, { "states", TAB_CART }, { "breaks", TAB_BREAKS } };
    for (const auto &alias : aliases)
        if (tab.compare(QLatin1String(alias.name), Qt::CaseInsensitive) == 0) return alias.tab;
    bool isNum = false;
    const int n = tab.toInt(&isNum);
    if (isNum) return n >= 0 && n < TAB_COUNT ? n : -1;
    for (int pass = 0; pass < 3; ++pass)
        for (int i = 0; i < TAB_COUNT; ++i) {
            const QString name = QString::fromUtf8(kTabs[i].name), label = QString::fromUtf8(kTabs[i].label);
            if (pass == 0 && name.startsWith(tab, Qt::CaseInsensitive)) return i;
            if (pass == 1 && label.startsWith(tab, Qt::CaseInsensitive)) return i;
            if (pass == 2) {
                const QStringList words = label.split(QLatin1Char(' '));
                for (int w = 1; w < words.size(); ++w)
                    if (words[w].startsWith(tab, Qt::CaseInsensitive)) return i;
            }
        }
    return -1;
}

/* The family's keys: F5 run/stop, F7 step, F8 step over, Shift+F8 step out,
 * F9 breakpoint, F12 close. */
bool isDebuggerKey(const QKeyEvent *e)
{
    const Qt::KeyboardModifiers mods = e->modifiers() & ~Qt::KeypadModifier;
    switch (e->key()) {
    case Qt::Key_F5: case Qt::Key_F7: case Qt::Key_F9: case Qt::Key_F12: return mods == Qt::NoModifier;
    case Qt::Key_F8: return mods == Qt::NoModifier || mods == Qt::ShiftModifier;
    default: return false;
    }
}

/* BASIC tab gutter: a breakpoint's dot (state 1 on, 2 off), blank otherwise. */
QString dotFor(int state)
{
    return state == 1 ? QStringLiteral("\u25CF") : state == 2 ? QStringLiteral("\u25CB") : QStringLiteral(" ");
}

QPointer<DebuggerWindow> s_win;

/* The breakpoint type of a chooser row. */
int bpTypeAt(int row)
{
    return kBpTypes[qBound(0, row, 3)].type;
}

} // namespace

void DebuggerWindow::showFor(QWidget *parent, a2600session *session)
{
    if (!s_win) s_win = new DebuggerWindow(session, parent);
    const bool wasShown = s_win->isVisible();
    /* showEvent stops the machine */
    s_win->show();
    s_win->raise();
    s_win->activateWindow();
    if (wasShown) {
        a2600debug_stop(s_win->m_dbg);
        s_win->settleAndSync();
    }
}

void DebuggerWindow::toggleFor(QWidget *parent, a2600session *session)
{
    if (s_win && s_win->isVisible()) s_win->hide();
    else showFor(parent, session);
}

DebuggerWindow::DebuggerWindow(a2600session *session, QWidget *parent)
    : QMainWindow(parent, Qt::Window), m_session(session), m_dbg(a2600session_debugger(session))
{
    setObjectName(QStringLiteral("debugger"));
    setWindowTitle(QStringLiteral("Atari 2600 Debugger"));
    resize(1400, 820);

    auto *central = new QWidget;
    auto *root = new QVBoxLayout(central);
    root->addWidget(buildToolbar());
    m_tabs = new QTabWidget;
    m_tabs->setUsesScrollButtons(true);
    m_pages[TAB_CONSOLE] = buildConsole();
    m_pages[TAB_DISASM] = buildDisasm();
    m_pages[TAB_BASIC] = buildBasic();
    m_pages[TAB_MEMORY] = buildMemory();
    m_pages[TAB_BREAKS] = buildBreaks();
    m_pages[TAB_WATCHES] = buildWatches();
    m_pages[TAB_CART] = buildCart();
    m_pages[TAB_TRACE] = buildHist(0);
    m_pages[TAB_HISTORY] = buildHist(1);
    m_pages[TAB_VIDEO] = buildVideo();
    m_pages[TAB_SOUND] = buildSound();
    m_pages[TAB_SYMBOLS] = buildSymbols();
    m_pages[TAB_FUJINET] = buildFujinet();
    for (int i = 0; i < TAB_COUNT; ++i) {
        /* a page's place in the tab widget is not its identity once tabs move
         * into windows of their own: each carries its own number */
        m_pages[i]->setProperty("tab", i);
        m_tabs->addTab(m_pages[i], QString::fromUtf8(kTabs[i].label).replace(QLatin1Char('&'), QLatin1String("&&")));
    }
    const int tab = tabFromEnv();
    if (tab >= 0) m_tabs->setCurrentIndex(tab);
    /* Help's contexts, one per tab */
    auto setHelpContext = [this](int page) {
        if (page >= 0 && page < TAB_COUNT)
            setProperty("a2600HelpContext", QStringLiteral("debugger/%1").arg(QLatin1String(kTabs[page].help)));
    };
    setHelpContext(currentTab());
    new QShortcut(QKeySequence(Qt::Key_F1), this, [this] { a2600HelpFor(this); });
    connect(m_tabs, &QTabWidget::currentChanged, this, [this, setHelpContext](int) {
        const int page = currentTab();
        setHelpContext(page);
        if (page == TAB_VIDEO) m_videoForce = true;
        if (page == TAB_HISTORY) m_hSeen[1] = -1;
        refreshPage(page, false);
        if (page == TAB_CONSOLE) m_conIn->setFocus();
        if (page == TAB_BASIC) basicScrollToPc();   /* the view has no size until it shows */
    });
    root->addWidget(m_tabs, 1);
    root->addWidget(buildRegs());
    setCentralWidget(central);
    statusBar();

    /* the source beside the cartridge, if there is one */
    {
        a2600srcmap_import_result res;
        if (a2600debug_basic_auto_import(m_dbg, &res) == 1) basicNote(res, false);
    }
    /* A2600_BASIC_IMPORT=<.bas file or folder> imports it as Import File / Import
     * Folder would, A2600_BASIC_BREAK=<name.bas>:<line> toggles a breakpoint on
     * that line */
    if (const QByteArray imp = qgetenv("A2600_BASIC_IMPORT"); !imp.isEmpty()) basicImported(QString::fromUtf8(imp), false);
    m_envBreak = qEnvironmentVariable("A2600_BASIC_BREAK");
    applyEnvBreak();

    /* for captures and tests: A2600_DEBUGGER_WATCH adds watches (a comma list),
     * A2600_DEBUGGER_WATCH_BREAK a watch with a break-on-change over it,
     * A2600_DEBUGGER_MEM_GOTO shows a range in Memory, A2600_DEBUGGER_HISTORY
     * records the trace and A2600_DEBUGGER_POPOUT peels a tab off */
    {
        const QByteArray mg = qgetenv("A2600_DEBUGGER_MEM_GOTO");
        const QString wb = qEnvironmentVariable("A2600_DEBUGGER_WATCH_BREAK").trimmed();
        for (const QString &text : qEnvironmentVariable("A2600_DEBUGGER_WATCH").split(QLatin1Char(','), Qt::SkipEmptyParts))
            if (!text.trimmed().isEmpty()) a2600watch_add(text.trimmed().toUtf8().constData());
        if (!wb.isEmpty()) {
            const int index = a2600watch_add(wb.toUtf8().constData());
            if (index >= 0) watchBreak(index);
        }
        uint16_t lo, hi;
        if (!mg.isEmpty() && a2600sym_parse_range(m_dbg, mg.constData(), &lo, &hi)) showInMemory(lo, hi);
        if (tabFromEnv("A2600_DEBUGGER_POPOUT") >= 0) popOut(tabFromEnv("A2600_DEBUGGER_POPOUT"));
    }

    /* the timer runs only while the window is shown (showEvent/hideEvent) */
    connect(&m_timer, &QTimer::timeout, this, [this] { tick(); });
}

/* Only at exit, with the main window: the session outlives its windows. */
DebuggerWindow::~DebuggerWindow()
{
    m_closing = true;       /* a peeled-off tab's window goes with the debugger */
    for (QWidget *&win : m_popped)
        if (win) {
            QWidget *w = win;
            win = nullptr;
            w->close();
        }
}

/* ---- building ------------------------------------------------------------- */

/* Two rows: the stepping controls, Pop Out and Help; then Break at, the
 * symbols, Save and the status line, which has the room that is left and
 * wraps rather than being cut. */
QWidget *DebuggerWindow::buildToolbar()
{
    auto *bar = new QWidget;
    auto *rows = new QVBoxLayout(bar);
    rows->setContentsMargins(0, 0, 0, 0);
    rows->setSpacing(2);
    auto *h = new QHBoxLayout;
    h->setContentsMargins(0, 0, 0, 0);
    rows->addLayout(h);
    auto add = [&](const QString &label, const QString &tip, std::function<void()> fn) {
        auto *b = new QPushButton(label);
        b->setFocusPolicy(Qt::NoFocus);
        b->setToolTip(tip);
        connect(b, &QPushButton::clicked, this, fn);
        h->addWidget(b);
        return b;
    };
    /* Run is a checkable button, checked while the machine is stopped: its
     * stylesheet gives that the accent. */
    m_runBtn = add(QStringLiteral("Stop (F5)"), QStringLiteral("Run, or stop before the next instruction"),
                   [this] { toggleRun(); });
    m_runBtn->setCheckable(true);
    m_runBtn->setStyleSheet(QStringLiteral("QPushButton:checked { background:%1; color:%2; }")
                                .arg(accentColor().name(), accentInk().name()));
    auto *step = add(QStringLiteral("Step (F7)"), QStringLiteral("One instruction"),
                     [this] { stepAnd([this] { doStep(); }); });
    add(QStringLiteral("Step Over (F8)"), QStringLiteral("Over a JSR"),
        [this] { stepAnd([this] { doStepOver(); }); });
    add(QStringLiteral("Step Out (Shift+F8)"), QStringLiteral("On until the routine returns"),
        [this] { stepAnd([this] { doStepOut(); }); });
    add(QStringLiteral("Run to Cursor"), QStringLiteral("On to the line selected in the Disassembly"),
        [this] { runToCursor(); });
    add(QStringLiteral("Scan+1"), QStringLiteral("On to the next scan line"),
        [this] { stepAnd([this] { a2600debug_scanline(m_dbg, 1); }); });
    add(QStringLiteral("Frame+1"), QStringLiteral("On to the next frame"),
        [this] { stepAnd([this] { a2600debug_frame(m_dbg, 1); }); });
    add(QStringLiteral("Rewind"), QStringLiteral("Back one recorded state"),
        [this] { stepAnd([this] { a2600debug_rewind(m_dbg, 1); }); });
    add(QStringLiteral("Unwind"), QStringLiteral("Forward one recorded state"),
        [this] { stepAnd([this] { a2600debug_unwind(m_dbg, 1); }); });
    add(QStringLiteral("Clear Breakpoints"), QStringLiteral("Remove every breakpoint"), [this] {
        a2600debug_breakpoint_clear(m_dbg);
        resync();
    });
    fixToggleSize(m_runBtn, { QStringLiteral("Stop (F5)"), QStringLiteral("Run (F5)") }, step->sizeHint().height());
    h->addStretch(1);
    auto *pop = new QPushButton(QStringLiteral("Pop Out"));
    pop->setFocusPolicy(Qt::NoFocus);
    pop->setToolTip(QStringLiteral("Move the tab that shows into a window of its own; Dock puts it back"));
    connect(pop, &QPushButton::clicked, this, [this] { popOut(currentTab()); });
    h->addWidget(pop);
    auto *help = qobject_cast<QToolButton *>(a2600HelpButton(QString()));
    if (help) {
        help->setAutoRaise(false);
        help->setText(QStringLiteral("Help"));
        help->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    }
    h->addWidget(help);

    auto *second = new QHBoxLayout;
    second->setContentsMargins(0, 0, 0, 0);
    rows->addLayout(second);
    second->addWidget(new QLabel(QStringLiteral("Break at:")));
    m_breakAt = smallEntry(QStringLiteral("symbol or $addr"), 18);
    m_breakAt->setToolTip(QStringLiteral("A symbol or a hex address, then Enter: an execute breakpoint there"));
    connect(m_breakAt, &QLineEdit::returnPressed, this, [this] { breakAt(); });
    second->addWidget(m_breakAt);

    auto *sym = new QPushButton(QStringLiteral("Load Symbols"));
    sym->setFocusPolicy(Qt::NoFocus);
    sym->setToolTip(QStringLiteral("Labels from the DASM .sym or .lst file next to the cartridge"));
    connect(sym, &QPushButton::clicked, this, [this] { loadSymbols(); });
    second->addWidget(sym);
    auto *clearSym = new QPushButton(QStringLiteral("Clear Symbols"));
    clearSym->setFocusPolicy(Qt::NoFocus);
    clearSym->setToolTip(QStringLiteral("Drop every loaded symbol and label"));
    connect(clearSym, &QPushButton::clicked, this, [this] { clearSymbols(); });
    second->addWidget(clearSym);

    m_status = new QLabel;
    m_status->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_status->setStyleSheet(kGray);
    m_status->setContentsMargins(8, 0, 8, 0);
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_status->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    second->addWidget(m_status, 1);
    return bar;
}

QWidget *DebuggerWindow::buildConsole()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    m_conOut = monoView();
    m_conOut->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_conOut->setMaximumBlockCount(CONSOLE_LINES);
    m_conOut->setPlainText(QStringLiteral("Stella's debugger console. Type 'help' for every command; 'cart', 'video', "
                                          "'watch' and 'find' are this app's own. Numbers are hex: 80, $80 or "
                                          "0x80.\n"));
    m_conIn = new QLineEdit;
    setMono(m_conIn);
    m_conIn->setPlaceholderText(QStringLiteral("command (help, step, trace, break, breakIf, trap, watch, frame, tia, cart, "
                                               "video ...) — Tab completes, Up/Down recall"));
    m_conIn->installEventFilter(this);
    connect(m_conIn, &QLineEdit::returnPressed, this, [this] { runConsole(); });
    v->addWidget(m_conOut, 1);
    v->addWidget(m_conIn);
    return w;
}

/* The registers, under the tabs and so on every one of them (Enter sets
 * one), then the flags of PS and the beam. */
QWidget *DebuggerWindow::buildRegs()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(2);

    auto *regs = new QHBoxLayout;
    regs->setSpacing(8);
    for (int i = 0; i < 6; ++i) {
        auto *l = new QLabel(QString::fromUtf8(kRegs[i].name));
        l->setStyleSheet(kGray);
        regs->addWidget(l);
        m_reg[i] = new QLineEdit;
        setMono(m_reg[i]);
        m_reg[i]->setMaxLength(12);
        setDigits(m_reg[i], kRegs[i].digits);
        m_reg[i]->setToolTip(QStringLiteral("Enter sets it (hex, or an expression; only while stopped)"));
        connect(m_reg[i], &QLineEdit::returnPressed, this, [this, i] { setReg(i); });
        regs->addWidget(m_reg[i]);
    }
    regs->addSpacing(16);
    /* N V - B D I Z C of PS */
    for (int i = 0; i < 7; ++i) {
        m_flag[i] = new QCheckBox(QString::fromUtf8(kFlags[i].name));
        connect(m_flag[i], &QCheckBox::clicked, this, [this, i](bool on) { setFlag(kFlags[i].id, on); });
        regs->addWidget(m_flag[i]);
    }
    regs->addStretch(1);
    v->addLayout(regs);

    m_beam = new QLabel;
    setMono(m_beam);
    m_beam->setStyleSheet(kGray);
    v->addWidget(m_beam);
    return w;
}

/* The dump, Go to (a label, address or range, which it marks), the write
 * control, and the search bar. */
QWidget *DebuggerWindow::buildMemory()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);

    v->addWidget(titleLabel(QStringLiteral("Memory (the 8K bus the 6507 sees, $0000-$1FFF)")));
    m_mem = monoView();
    v->addWidget(m_mem, 1);

    auto *nav = new QHBoxLayout;
    nav->addWidget(new QLabel(QStringLiteral("Go to")));
    m_memGoto = smallEntry(QStringLiteral("$80, label or lo-hi"), 20);
    connect(m_memGoto, &QLineEdit::returnPressed, this, [this] { memGoto(); });
    nav->addWidget(m_memGoto);
    nav->addSpacing(12);
    nav->addWidget(new QLabel(QStringLiteral("Poke")));
    m_memAddr = smallEntry(QStringLiteral("$80"), 10);
    m_memVal = smallEntry(QStringLiteral("$00"), 5);
    connect(m_memAddr, &QLineEdit::returnPressed, this, [this] { memWrite(); });
    connect(m_memVal, &QLineEdit::returnPressed, this, [this] { memWrite(); });
    nav->addWidget(m_memAddr);
    nav->addWidget(new QLabel(QStringLiteral("value")));
    nav->addWidget(m_memVal);
    nav->addWidget(hintLabel(QStringLiteral("A bus write: RAM takes it, the TIA and RIOT act on it, "
                                            "ROM does not (it is not a patch)")), 1);
    v->addLayout(nav);
    v->addWidget(buildSearchBar());
    return w;
}

QWidget *DebuggerWindow::buildSearchBar()
{
    auto *bar = new QWidget;
    auto *row = new QHBoxLayout(bar);
    row->setContentsMargins(0, 0, 0, 0);
    row->addWidget(new QLabel(QStringLiteral("Search")));
    m_srchKind = new QComboBox;
    m_srchKind->addItems({ QStringLiteral("Hex"), QStringLiteral("Text") });
    m_srchKind->setToolTip(QStringLiteral("Hex bytes (3E 5A) or text, one byte per character"));
    row->addWidget(m_srchKind);
    m_srchEntry = new QLineEdit;
    setMono(m_srchEntry);
    m_srchEntry->setPlaceholderText(QStringLiteral("bytes or text to find"));
    connect(m_srchEntry, &QLineEdit::returnPressed, this, [this] { memSearch(false); });
    row->addWidget(m_srchEntry, 1);
    auto *next = new QPushButton(QStringLiteral("Find Next"));
    auto *prev = new QPushButton(QStringLiteral("Find Previous"));
    connect(next, &QPushButton::clicked, this, [this] { memSearch(false); });
    connect(prev, &QPushButton::clicked, this, [this] { memSearch(true); });
    row->addWidget(next);
    row->addWidget(prev);
    m_srchNote = new QLabel;
    m_srchNote->setStyleSheet(kGray);
    row->addWidget(m_srchNote);
    return bar;
}

QWidget *DebuggerWindow::buildDisasm()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    auto *row = new QHBoxLayout;
    m_bank = new QComboBox;
    m_bank->setToolTip(QStringLiteral("The bank listed: the one holding the PC, or a fixed one"));
    m_bank->addItem(QStringLiteral("PC's bank"));
    m_followPc = new QCheckBox(QStringLiteral("Follow PC"));
    m_followPc->setChecked(true);
    connect(m_followPc, &QCheckBox::clicked, this, [this] { refreshDisasm(); });
    m_jump = smallEntry(QStringLiteral("$address or label"), 20);
    connect(m_jump, &QLineEdit::returnPressed, this, [this] { jumpTo(); });
    connect(m_bank, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int sel) {
        if (sel < 0 || m_rebuildingBanks) return;
        m_disasmBank = sel - 1;
        if (sel > 0) m_followPc->setChecked(false);
        m_disasmFirst = 0;
        refreshDisasm();
    });
    row->addWidget(m_bank);
    row->addWidget(m_followPc);
    row->addSpacing(8);
    row->addWidget(new QLabel(QStringLiteral("Jump to")));
    row->addWidget(m_jump);
    row->addSpacing(8);
    row->addWidget(hintLabel(QStringLiteral("Click a line to select it (Run to Cursor); click its left edge, "
                                            "double-click it or press F9 for a breakpoint; scroll to browse")), 1);
    v->addLayout(row);
    m_disasm = monoView();
    /* no scrollbar: the wheel scrolls by instructions, as many lines as fit */
    m_disasm->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_disasm->setFocusPolicy(Qt::NoFocus);
    m_disasm->viewport()->setCursor(Qt::ArrowCursor);
    m_disasm->viewport()->installEventFilter(this);
    v->addWidget(m_disasm, 1);
    return w;
}

QWidget *DebuggerWindow::buildVideo()
{
    m_video = new VideoTab(m_session);
    return m_video;
}

QWidget *DebuggerWindow::buildBreaks()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);

    m_bpStop = new QLabel;
    m_bpStop->setStyleSheet(QStringLiteral("font-weight: bold;"));
    m_bpStop->setWordWrap(true);
    v->addWidget(m_bpStop);

    v->addWidget(hintLabel(QStringLiteral("Execute stops before the instruction runs (in one bank, or any); "
                                          "the others after the access. A condition may use old and value.")));

    m_bpTable = new QTableWidget(0, 4);
    m_bpTable->setHorizontalHeaderLabels({ QStringLiteral("On"), QStringLiteral("Breakpoint"), QStringLiteral("Hits"),
                                           QString() });
    m_bpTable->verticalHeader()->setVisible(false);
    m_bpTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_bpTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_bpTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_bpTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_bpTable->setFocusPolicy(Qt::NoFocus);
    connect(m_bpTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
        if (m_bpFilling || item->column() != 0) return;
        a2600debug_bp_enable(m_dbg, item->data(Qt::UserRole).toInt(), item->checkState() == Qt::Checked);
        syncLater();
    });
    v->addWidget(m_bpTable, 1);
    m_bpEmpty = hintLabel(QStringLiteral("No breakpoints. Click the left edge of a disassembly line, or add one below."));
    v->addWidget(m_bpEmpty);

    auto *row = new QHBoxLayout;
    m_bpType = new QComboBox;
    for (const auto &entry : kBpTypes) m_bpType->addItem(QString::fromUtf8(entry.label));
    m_bpBank = new QComboBox;
    m_bpBank->setToolTip(QStringLiteral("An execute breakpoint's bank"));
    m_bpBank->addItem(QStringLiteral("Any bank"));
    m_bpStart = smallEntry(QStringLiteral("address, range or symbol"), 24);
    m_bpEnd = smallEntry(QStringLiteral("end (optional)"), 16);
    m_bpCond = new QLineEdit;
    m_bpCond->setPlaceholderText(QStringLiteral("condition (optional, e.g. a==$ff or value != old)"));
    auto *add = new QPushButton(QStringLiteral("Add"));
    auto *clear = new QPushButton(QStringLiteral("Clear Breakpoints"));
    auto *clearHits = new QPushButton(QStringLiteral("Clear Hits"));
    connect(clearHits, &QPushButton::clicked, this, [this] {
        a2600debug_bp_clear_hits(m_dbg);
        resync();
    });
    connect(add, &QPushButton::clicked, this, [this] { addBreakpoint(); });
    connect(clear, &QPushButton::clicked, this, [this] {
        a2600debug_breakpoint_clear(m_dbg);
        resync();
    });
    for (QLineEdit *e : { m_bpStart, m_bpEnd, m_bpCond })
        connect(e, &QLineEdit::returnPressed, this, [this] { addBreakpoint(); });
    connect(m_bpType, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int index) { m_bpBank->setEnabled(bpTypeAt(index) == A2600DEBUG_BP_EXEC); });
    row->addWidget(m_bpType);
    row->addWidget(m_bpBank);
    row->addWidget(m_bpStart);
    row->addWidget(m_bpEnd);
    row->addWidget(m_bpCond, 1);
    row->addWidget(add);
    row->addWidget(clearHits);
    row->addWidget(clear);
    v->addLayout(row);

    m_bpMsg = new QLabel;
    m_bpMsg->setWordWrap(true);
    m_bpMsg->setStyleSheet(QStringLiteral("color: #e01b24;"));
    v->addWidget(m_bpMsg);
    return w;
}

QWidget *DebuggerWindow::buildWatches()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    auto *row = new QHBoxLayout;
    row->addWidget(new QLabel(QStringLiteral("Watch")));
    m_wtEntry = new QLineEdit;
    setMono(m_wtEntry);
    m_wtEntry->setPlaceholderText(QStringLiteral("address, label, expression or lo-hi"));
    connect(m_wtEntry, &QLineEdit::returnPressed, this, [this] { addWatch(m_wtEntry->text()); });
    auto *add = new QPushButton(QStringLiteral("Add"));
    connect(add, &QPushButton::clicked, this, [this] { addWatch(m_wtEntry->text()); });
    row->addWidget(m_wtEntry, 1);
    row->addWidget(add);
    v->addLayout(row);

    m_wtTable = new QTableWidget(0, 2);
    m_wtTable->setHorizontalHeaderLabels({ QStringLiteral("Watch"), QString() });
    m_wtTable->verticalHeader()->setVisible(false);
    m_wtTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_wtTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    m_wtTable->setColumnWidth(1, 350);
    m_wtTable->verticalHeader()->setDefaultSectionSize(34);
    m_wtTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_wtTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_wtTable->setFocusPolicy(Qt::NoFocus);
    v->addWidget(m_wtTable, 1);
    v->addWidget(hintLabel(QStringLiteral("The values are read at a stop and show - while the machine runs. A watch "
                                          "is kept as text, so a label that loads later starts resolving. "
                                          "Break on change stops after a write that alters the value.")));
    return w;
}

QWidget *DebuggerWindow::buildCart()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    m_io = monoView();
    v->addWidget(m_io, 1);

    /* the save-state slots, and the files the engine can write */
    v->addWidget(titleLabel(QStringLiteral("Emulator states (Stella's saveState / loadState slots)")));
    auto *grid = new QGridLayout;
    const struct { const char *label; bool save; } kinds[2] = { { "Save", true }, { "Load", false } };
    for (int row = 0; row < 2; ++row) {
        grid->addWidget(new QLabel(QString::fromUtf8(kinds[row].label)), row, 0);
        for (int slot = 0; slot < 10; ++slot) {
            auto *b = new QPushButton(QString::number(slot));
            b->setFocusPolicy(Qt::NoFocus);
            b->setToolTip(QStringLiteral("%1 state slot %2").arg(QString::fromUtf8(kinds[row].label)).arg(slot));
            const bool save = kinds[row].save;
            connect(b, &QPushButton::clicked, this, [this, slot, save] {
                if (save) a2600debug_state_save(m_dbg, slot);
                else a2600debug_state_load(m_dbg, slot);
                say(QStringLiteral("%1 state %2").arg(save ? QStringLiteral("Saved") : QStringLiteral("Loaded")).arg(slot));
                resync();
            });
            grid->addWidget(b, row, slot + 1);
        }
    }
    grid->setColumnStretch(11, 1);
    v->addLayout(grid);

    auto *files = new QHBoxLayout;
    files->addWidget(new QLabel(QStringLiteral("Save to a file")));
    static const struct { const char *kind, *title, *file; } saves[] = {
        { "dis", "Disassembly", "disassembly.asm" }, { "rom", "ROM (patched)", "patched.bin" },
        { "access", "Access counters", "access.txt" }, { "ses", "Session", "session.ses" },
        { "snap", "TIA snapshot", "snapshot.png" } };
    for (const auto &entry : saves) {
        auto *b = new QPushButton(QString::fromUtf8(entry.title));
        b->setFocusPolicy(Qt::NoFocus);
        const char *kind = entry.kind;
        const QString title = QString::fromUtf8(entry.title), file = QString::fromUtf8(entry.file);
        connect(b, &QPushButton::clicked, this, [this, kind, title, file] { save(kind, title, file); });
        files->addWidget(b);
    }
    files->addStretch(1);
    v->addLayout(files);
    return w;
}

/* Trace (idx 0) reads the instruction ring live; History (idx 1) reads it at
 * a stop. They share the one ring and recorder, so the two Record buttons
 * follow each other. */
QWidget *DebuggerWindow::buildHist(int idx)
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    auto *bar = new QHBoxLayout;

    m_hRec[idx] = new QPushButton(QStringLiteral("Record"));
    m_hRec[idx]->setCheckable(true);
    m_hRec[idx]->setFocusPolicy(Qt::NoFocus);
    m_hRec[idx]->setToolTip(QStringLiteral("Record every instruction the CPU runs (the machine runs a little slower)"));
    m_hRec[idx]->setStyleSheet(
        QStringLiteral("QPushButton:checked { background:%1; color:white; }").arg(accentColor().name()));
    fixToggleSize(m_hRec[idx], { QStringLiteral("Record") }, m_hRec[idx]->sizeHint().height());
    connect(m_hRec[idx], &QPushButton::toggled, this, [this](bool on) {
        if (m_hSync) return;
        a2600debug_trace_enable(m_dbg, on);
        for (int i = 0; i < 2; ++i) refreshHist(i, true);
    });
    m_hFilter[idx] = new QComboBox;
    m_hFilter[idx]->addItems({ QStringLiteral("All instructions"), QStringLiteral("Branches only"),
                               QStringLiteral("In range") });
    m_hRange[idx] = new QLineEdit;
    setMono(m_hRange[idx]);
    m_hRange[idx]->setPlaceholderText(QStringLiteral("range: lo-hi or a label"));
    connect(m_hFilter[idx], QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this, idx](int) { refreshHist(idx, true); });
    connect(m_hRange[idx], &QLineEdit::returnPressed, this, [this, idx] { refreshHist(idx, true); });
    auto *clear = new QPushButton(QStringLiteral("Clear"));
    connect(clear, &QPushButton::clicked, this, [this] {
        a2600debug_trace_clear(m_dbg);
        for (int i = 0; i < 2; ++i) refreshHist(i, true);
    });
    m_hNote[idx] = new QLabel;
    m_hNote[idx]->setStyleSheet(kGray);
    m_hNote[idx]->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    bar->addWidget(m_hRec[idx]);
    bar->addWidget(m_hFilter[idx]);
    bar->addWidget(m_hRange[idx], 1);
    bar->addWidget(clear);
    bar->addWidget(m_hNote[idx], 1);
    v->addLayout(bar);
    m_hText[idx] = monoView();
    v->addWidget(m_hText[idx], 1);
    return w;
}

QWidget *DebuggerWindow::buildSound()
{
    m_sound = monoView();
    return m_sound;
}

/* A filter over a table of address and name; activating a row opens
 * that address in the Disassembly. */
QWidget *DebuggerWindow::buildSymbols()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    auto *row = new QHBoxLayout;
    row->addWidget(new QLabel(QStringLiteral("Filter")));
    m_symFilter = new QLineEdit;
    m_symFilter->setPlaceholderText(QStringLiteral("part of a name"));
    m_symFilter->setClearButtonEnabled(true);
    connect(m_symFilter, &QLineEdit::textChanged, this, [this] { refreshSymbols(true); });
    row->addWidget(m_symFilter, 1);
    m_symCount = new QLabel;
    m_symCount->setStyleSheet(kGray);
    row->addWidget(m_symCount);
    v->addLayout(row);

    m_symTable = new QTableWidget(0, 2);
    m_symTable->setHorizontalHeaderLabels({ QStringLiteral("Address"), QStringLiteral("Name") });
    m_symTable->verticalHeader()->setVisible(false);
    m_symTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_symTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_symTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_symTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_symTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    setMono(m_symTable);
    auto activate = [this](int row) {
        QTableWidgetItem *item = row >= 0 ? m_symTable->item(row, 0) : nullptr;
        if (item) showInDisassembly((uint16_t)item->data(Qt::UserRole).toUInt());
    };
    connect(m_symTable, &QTableWidget::cellDoubleClicked, this, [activate](int row, int) { activate(row); });
    connect(m_symTable, &QTableWidget::itemActivated, this, [activate](QTableWidgetItem *item) { activate(item->row()); });
    v->addWidget(m_symTable, 1);
    auto *define = new QHBoxLayout;
    define->addWidget(new QLabel(QStringLiteral("Define")));
    m_symAddr = smallEntry(QStringLiteral("$address"), 10);
    m_symName = new QLineEdit;
    m_symName->setPlaceholderText(QStringLiteral("label name"));
    auto *defineBtn = new QPushButton(QStringLiteral("Define"));
    connect(m_symAddr, &QLineEdit::returnPressed, this, [this] { defineSymbol(); });
    connect(m_symName, &QLineEdit::returnPressed, this, [this] { defineSymbol(); });
    connect(defineBtn, &QPushButton::clicked, this, [this] { defineSymbol(); });
    define->addWidget(m_symAddr);
    define->addWidget(m_symName, 1);
    define->addWidget(defineBtn);
    v->addLayout(define);
    v->addWidget(hintLabel(QStringLiteral("Double-click a row (or press Enter on it) to open its address in the "
                                          "Disassembly. Stella's own equates (WSYNC ...) are not listed.")));
    return w;
}

/* The packets left (a note and Clear over them), the link and counts
 * right. */
QWidget *DebuggerWindow::buildFujinet()
{
    auto *split = new QSplitter(Qt::Horizontal);
    auto *left = new QWidget;
    auto *lv = new QVBoxLayout(left);
    lv->addWidget(titleLabel(QStringLiteral("FujiBus packets (SLIP-decoded)")));
    auto *row = new QHBoxLayout;
    m_fujiNote = new QLabel;
    m_fujiNote->setStyleSheet(kGray);
    m_fujiNote->setWordWrap(true);
    auto *clear = new QPushButton(QStringLiteral("Clear"));
    connect(clear, &QPushButton::clicked, this, [this] {
        a2600debug_fujinet_trace_clear(m_dbg);
        refreshFujinet(true);
    });
    row->addWidget(m_fujiNote, 1);
    row->addWidget(clear);
    lv->addLayout(row);
    m_fujiText = monoView();
    lv->addWidget(m_fujiText, 1);
    left->setMinimumWidth(560);

    auto *right = new QWidget;
    auto *rv = new QVBoxLayout(right);
    rv->addWidget(titleLabel(QStringLiteral("The FujiNet cartridge")));
    m_fujiState = monoView();
    rv->addWidget(m_fujiState, 1);
    right->setMinimumWidth(380);
    split->addWidget(left);
    split->addWidget(right);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    split->setChildrenCollapsible(false);
    return split;
}

/* ---- refresh -------------------------------------------------------------- */

void DebuggerWindow::refreshStatus()
{
    char reason[160];
    int addr = -1;
    const bool stopped = a2600debug_is_stopped(m_dbg) != 0;
    reason[0] = '\0';
    a2600debug_stop_reason(m_dbg, reason, sizeof reason, &addr);
    QString text = QStringLiteral("Running");
    if (stopped) {
        text = stoppedText(stripControl(reason));
        /* where, by name when it has one */
        char name[64];
        if (addr >= 0 && a2600debug_address_label(m_dbg, (uint16_t)addr, name, sizeof name) > 0 && name[0])
            text += QStringLiteral("  (%1)").arg(QString::fromUtf8(name));
    }
    if (m_status->text() != text) {
        m_status->setText(text);
        m_status->setToolTip(text);
    }
    const QString run = stopped ? QStringLiteral("Run (F5)") : QStringLiteral("Stop (F5)");
    if (m_runBtn->text() != run) m_runBtn->setText(run);
    if (m_runBtn->isChecked() != stopped) m_runBtn->setChecked(stopped);
}

/* The refresh never overwrites a value being typed. */
void DebuggerWindow::setEntry(QLineEdit *e, const QString &text)
{
    if (e->hasFocus()) return;
    if (e->text() != text) e->setText(text);
}

void DebuggerWindow::refreshCpu()
{
    a2600debug_cpu c;
    a2600debug_cpu_get(m_dbg, &c);
    const int values[6] = { c.pc, c.sp, c.a, c.x, c.y, c.ps };
    for (int i = 0; i < 6; ++i)
        setEntry(m_reg[i], hx((unsigned)values[i] & (kRegs[i].digits == 4 ? 0xFFFF : 0xFF), kRegs[i].digits));
    const int flags[7] = { c.n, c.v, c.b, c.d, c.i, c.z, c.c };
    for (int i = 0; i < 7; ++i) m_flag[i]->setChecked(flags[i] != 0);
    a2600debug_tia t;
    a2600debug_tia_get(m_dbg, &t);
    m_beam->setText(QStringLiteral("scanline %1  clock %2  frame %3  last instruction %4 cycles  %5 cycles in all")
                        .arg(t.scanlines, 3).arg(t.clocks_this_line, 3).arg(t.frame_count).arg(c.cycles)
                        .arg((qulonglong)c.total_cycles));
}

/* The 8K the 6507 sees, a row of 16 bytes at a time, each row
 * named by the memory-map region it starts in (one fetch of the map for the
 * refresh). The Go to / search range stays marked, and so does every watch. */
void DebuggerWindow::refreshMem()
{
    static uint8_t mem[MEM_ROWS * 16];
    static char text[MEM_ROWS * MEM_ROW_TEXT + 256];
    static a2600debug_region map[REGION_MAX];
    memset(mem, 0, sizeof mem);
    a2600debug_read(m_dbg, 0, mem, (int)sizeof mem);
    const int regions = a2600debug_memory_map(m_dbg, map, REGION_MAX);

    int len = snprintf(text, sizeof text, "       ");
    for (int col = 0; col < 16; ++col) len += snprintf(text + len, sizeof text - (size_t)len, "%X  ", col);
    for (int row = 0; row < MEM_ROWS; ++row) {
        const uint8_t *bytes = mem + row * 16;
        len += snprintf(text + len, sizeof text - (size_t)len, "\n$%04X: ", row * 16);
        for (int col = 0; col < 16; ++col) len += snprintf(text + len, sizeof text - (size_t)len, "%02X ", bytes[col]);
        text[len++] = ' ';
        for (int col = 0; col < 16; ++col) text[len++] = bytes[col] >= 0x20 && bytes[col] < 0x7F ? (char)bytes[col] : '.';
        char name[64];
        a2600mem_region_name(map, regions, (uint16_t)(row * 16), name, sizeof name);
        len += snprintf(text + len, sizeof text - (size_t)len, "  %s", name);
    }
    setText(m_mem, QString::fromLatin1(text, len));

    /* the ranges marked: each row's bytes in the hex and in the text column */
    QList<QTextEdit::ExtraSelection> sel;
    auto mark = [&](uint16_t from, uint16_t to, const QTextCharFormat &fmt) {
        for (int row = from / 16; row <= to / 16; ++row) {
            const int first = qMax((int)from - row * 16, 0), last = qMin((int)to - row * 16, 15);
            sel << spanSelection(m_mem, 1 + row, 7 + first * 3, (last - first) * 3 + 2, fmt);
            sel << spanSelection(m_mem, 1 + row, 7 + 48 + 1 + first, last - first + 1, fmt);
        }
    };
    for (int i = 0; i < a2600watch_count(); ++i) {
        uint16_t lo, hi;
        if (a2600watch_range(m_dbg, i, &lo, &hi)) mark(lo, hi, watchFormat());
    }
    if (m_hl) mark(m_hlLo, m_hlHi, accentFormat());
    m_mem->setExtraSelections(sel);
}

/* Memory scrolled so the row holding addr sits a third of the way down. */
void DebuggerWindow::scrollMemory(uint16_t addr)
{
    const int line = 1 + addr / 16;
    const int shown = m_mem->viewport()->height() / qMax(1, m_mem->fontMetrics().lineSpacing());
    m_mem->verticalScrollBar()->setValue(qMax(0, line - shown * 3 / 10));
}

/* The disassembly lines that fit the view. */
int DebuggerWindow::visibleRows() const
{
    const int line = m_disasm->fontMetrics().lineSpacing();
    const int height = m_disasm->viewport()->height() - 2 * (int)m_disasm->document()->documentMargin();
    if (line <= 0 || height <= 0) return 32;
    return qBound(4, height / line, DISASM_WINDOW);
}

/* The bank a line's breakpoint belongs to: the one listed. */
int DebuggerWindow::disasmBank() const
{
    return m_disasmBank < 0 ? a2600debug_current_bank(m_dbg) : m_disasmBank;
}

/* The line of the listed bank that holds addr (or the first after it), or -1. */
int DebuggerWindow::lineForAddress(uint16_t addr)
{
    static a2600debug_line lines[256];
    int total = 0, pcLine = -1, first = 0;
    for (;;) {
        const int n = a2600debug_disassemble(m_dbg, m_disasmBank, first, lines, 256, &total, &pcLine);
        if (n <= 0) return -1;
        for (int i = 0; i < n; ++i)
            if (lines[i].address >= addr) return first + i;
        first += n;
        if (first >= total) return -1;
    }
}

void DebuggerWindow::refreshDisasm()
{
    static a2600debug_line lines[DISASM_WINDOW];
    const int rows = visibleRows();

    /* the bank chooser follows the cartridge */
    const int banks = a2600debug_bank_count(m_dbg);
    if (m_bank->count() != banks + 1) {
        m_rebuildingBanks = true;
        m_bank->clear();
        m_bank->addItem(QStringLiteral("PC's bank"));
        for (int i = 0; i < banks; ++i) m_bank->addItem(QStringLiteral("Bank %1").arg(i));
        if (m_disasmBank >= banks) m_disasmBank = -1;
        m_bank->setCurrentIndex(m_disasmBank + 1);
        m_rebuildingBanks = false;
    }

    int total = 0, pcLine = -1;
    a2600debug_disassemble(m_dbg, m_disasmBank, 0, lines, 1, &total, &pcLine);
    if (m_followPc->isChecked() && pcLine >= 0) m_disasmFirst = qMax(0, pcLine - rows / 3);   /* the PC a third of the way down */
    m_disasmFirst = qBound(0, m_disasmFirst, qMax(total - 1, 0));
    int n = a2600debug_disassemble(m_dbg, m_disasmBank, m_disasmFirst, lines, rows, &total, &pcLine);
    /* the label column is as wide as the longest label shown, within reason */
    int labelWidth = 0;
    for (int i = 0; i < n; ++i) labelWidth = qMax(labelWidth, (int)strlen(lines[i].label));
    labelWidth = qMin(labelWidth, 24);

    m_lineAddr.clear();
    QString text;
    char buf[128];
    struct Span { int line, from, length; };
    std::vector<Span> grays;
    /* Stella's listing has an empty tag (address 0000) before an entry point: no line for it */
    int shown = 0;
    for (int i = 0, kept = 0; i < n; ++i)
        if (lines[i].disasm[0] || lines[i].is_pc) {
            if (kept != i) lines[kept] = lines[i];
            ++kept;
            shown = kept;
        }
    n = shown;
    pcLine = -1;
    for (int i = 0; i < n; ++i)
        if (lines[i].is_pc) pcLine = i;
    for (int i = 0; i < n; ++i) {
        m_lineAddr.push_back(lines[i].address);
        /* the gutter: a breakpoint dot (hollow when disabled), then the
         * PC's arrow */
        QString ln = lines[i].has_breakpoint ? QStringLiteral("●") : lines[i].bp_disabled ? QStringLiteral("○")
                                                                                          : QStringLiteral(" ");
        snprintf(buf, sizeof buf, "%c%04X  %-11.11s ", lines[i].is_pc ? '>' : ' ', lines[i].address, lines[i].bytes);
        ln += QString::fromUtf8(buf);
        const int labelFrom = (int)ln.size();
        char label[48];
        snprintf(label, sizeof label, "%-*.*s ", labelWidth, labelWidth, lines[i].label);
        ln += QString::fromUtf8(label);
        if (lines[i].label[0] && i != pcLine)
            grays.push_back({ i, labelFrom, (int)strlen(lines[i].label) < labelWidth ? (int)strlen(lines[i].label)
                                                                                      : labelWidth });
        snprintf(buf, sizeof buf, "%-22s", lines[i].disasm);
        ln += QString::fromUtf8(buf);
        if (lines[i].cycles[0]) {
            /* ";2", ";2/3", and " *" / " ~" for a hardware access */
            const QString cycles = QString::fromUtf8(lines[i].cycles);
            if (i != pcLine) grays.push_back({ i, (int)ln.size(), (int)cycles.size() });
            ln += cycles;
        }
        if (i) text += QLatin1Char('\n');
        text += ln;
    }
    if (n == 0) text = QStringLiteral("(no disassembly)");
    m_disasm->setPlainText(stripControl(text.toUtf8().constData()));
    m_disasm->verticalScrollBar()->setValue(0);

    /* The PC's line is the accent bar: the block's own background, not an
     * extra selection -- a full-width selection paints over the gutter's
     * red dot, a block background lies under it. */
    if (pcLine >= 0 && pcLine < n) {
        QTextCursor cur(m_disasm->document()->findBlockByNumber(pcLine));
        QTextBlockFormat bf;
        bf.setBackground(accentColor());
        cur.setBlockFormat(bf);
        cur.select(QTextCursor::LineUnderCursor);
        QTextCharFormat cf;
        cf.setForeground(accentInk());
        if (lines[pcLine].address == m_selected) {
            cf.setFontUnderline(true);
            cf.setFontWeight(QFont::Bold);
        }
        cur.mergeCharFormat(cf);
    }

    QList<QTextEdit::ExtraSelection> sel;
    /* labels and cycle counts in the system's grey */
    QTextCharFormat grey;
    grey.setForeground(m_disasm->palette().color(QPalette::Disabled, QPalette::Text));
    for (const Span &g : grays) sel << spanSelection(m_disasm, g.line, g.from, g.length, grey);
    for (int i = 0; i < n; ++i) {
        /* the gutter: a breakpoint red (dark on the PC's bar), a disabled one grey */
        if (lines[i].has_breakpoint || lines[i].bp_disabled) {
            QTextCharFormat f;
            if (lines[i].has_breakpoint) {
                f.setForeground(i == pcLine ? accentInk() : QColor(0xe0, 0x1b, 0x24));
                f.setFontWeight(QFont::Bold);
            } else {
                f.setForeground(i == pcLine ? accentInk() : QColor(0x9a, 0x99, 0x96));
            }
            sel << spanSelection(m_disasm, i, 0, 1, f);
        }
        /* the selected line underlined (the PC's own is done above) */
        if (lines[i].address == m_selected && i != pcLine) {
            QTextCharFormat f;
            f.setFontUnderline(true);
            f.setFontWeight(QFont::Bold);
            QTextEdit::ExtraSelection s;
            s.cursor = QTextCursor(m_disasm->document()->findBlockByNumber(i));
            s.cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
            s.format = f;
            sel << s;
        }
    }
    m_disasm->setExtraSelections(sel);
}

/* ---- BASIC tab -------------------------------------------------------------- */

namespace {
/* The current line: a bar in the accent, as in the Disassembly. The bar is the
 * block's own background (a full-width ExtraSelection would repaint the gutter's
 * dot); the ink is a selection over all but the gutter column. `prev` is the
 * block wearing the bar, so a refresh that kept the text can move it. */
void basicBar(QList<QTextEdit::ExtraSelection> &out, QPlainTextEdit *v, int &prev, int block)
{
    QTextDocument *doc = v->document();
    auto setBg = [doc](int n, const QBrush &brush) {
        const QTextBlock b = doc->findBlockByNumber(n);
        if (!b.isValid()) return;
        QTextCursor c(b);
        QTextBlockFormat f = c.blockFormat();
        f.setBackground(brush);
        c.setBlockFormat(f);
    };
    if (prev >= 0 && prev != block) setBg(prev, Qt::NoBrush);
    prev = -1;
    if (block < 0) return;
    setBg(block, accentColor());
    prev = block;
    QTextEdit::ExtraSelection ink;
    const QTextBlock b = doc->findBlockByNumber(block);
    ink.cursor = QTextCursor(b);
    ink.cursor.setPosition(b.position() + 1);
    ink.cursor.setPosition(b.position() + b.length() - 1, QTextCursor::KeepAnchor);
    ink.format.setForeground(accentInk());
    out.append(ink);
}
} // namespace

QWidget *DebuggerWindow::buildBasic()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    auto *bar = new QHBoxLayout;
    auto button = [&](const QString &label, const QString &tip, int kind) {
        auto *b = new QPushButton(label);
        b->setToolTip(tip);
        connect(b, &QPushButton::clicked, this, [this, kind] { basicPick(kind); });
        bar->addWidget(b);
    };
    button(QStringLiteral("Import File…"), QStringLiteral("Load a .bas file and everything it includes"), 0);
    button(QStringLiteral("Import Folder…"), QStringLiteral("Load every .bas file under a folder"), 1);
    button(QStringLiteral("Load Map…"), QStringLiteral("Load a bB listing (.lst)"), 2);
    m_basFiles = new QComboBox;
    m_basFiles->setToolTip(QStringLiteral("The source file shown"));
    m_basFiles->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    connect(m_basFiles, QOverload<int>::of(&QComboBox::activated), this, [this](int i) {
        m_basFile = i;
        m_basPcLine = 0;
        refreshBasic();
    });
    bar->addWidget(m_basFiles);
    bar->addStretch(1);
    layout->addLayout(bar);
    m_basNote = new QLabel;
    m_basNote->setWordWrap(true);
    m_basNote->setEnabled(false);       /* the dim, secondary text colour */
    layout->addWidget(m_basNote);
    m_basView = monoView();
    m_basView->setFocusPolicy(Qt::NoFocus);
    m_basView->viewport()->setCursor(Qt::ArrowCursor);
    m_basView->viewport()->installEventFilter(this);
    layout->addWidget(m_basView, 1);
    return page;
}

/* On screen, with the keys: the BASIC tab, or its window when that is the active one. */
bool DebuggerWindow::basicTab() const
{
    return currentTab() == TAB_BASIC || (m_popped[TAB_BASIC] && m_popped[TAB_BASIC]->isActiveWindow());
}

/* On the BASIC tab the three steppers go by BASIC line. */
void DebuggerWindow::doStep()
{
    if (basicTab()) a2600debug_step_line(m_dbg); else a2600debug_step(m_dbg);
}

void DebuggerWindow::doStepOver()
{
    if (basicTab()) a2600debug_step_line_over(m_dbg); else a2600debug_step_over(m_dbg);
}

void DebuggerWindow::doStepOut()
{
    if (basicTab()) a2600debug_step_line_out(m_dbg); else a2600debug_step_out(m_dbg);
}

void DebuggerWindow::basicToggleAt(int line)
{
    if (m_basFile < 0) return;
    if (a2600debug_line_break_toggle(m_dbg, m_basFile, line) < 0) {
        message(QStringLiteral("No code on that line"));
        return;
    }
    m_basGen = ~0u;                     /* redraw the dot now */
    refreshBasic();
    refreshBps();
}

void DebuggerWindow::basicScrollToPc()
{
    if (m_basPcLine < 1) return;
    const QTextBlock block = m_basView->document()->findBlockByNumber(m_basPcLine - 1);
    if (!block.isValid()) return;
    m_basView->setTextCursor(QTextCursor(block));
    m_basView->centerCursor();
}

/* A2600_BASIC_BREAK=<name.bas>:<line>: the breakpoint is set once the machine has a console
 * (the engine ignores a toggle before that), so a tick retries until it holds. */
void DebuggerWindow::applyEnvBreak()
{
    if (!m_envBreak.contains(QLatin1Char(':'))) return;
    const QString name = m_envBreak.section(QLatin1Char(':'), 0, 0);
    const int line = m_envBreak.section(QLatin1Char(':'), 1).toInt();
    a2600srcmap *map = a2600srcmap_shared();
    for (int f = 0; f < a2600srcmap_file_count(map); ++f) {
        const QString fn = QString::fromUtf8(a2600srcmap_file_name(map, f));
        if (fn != name && !fn.endsWith(QLatin1Char('/') + name)) continue;
        if (a2600debug_line_break_state(m_dbg, f, line) == 1 ||
            (a2600debug_line_break_toggle(m_dbg, f, line) == 1 && a2600debug_line_break_state(m_dbg, f, line) == 1))
            m_envBreak.clear();
    }
}

/* The note under the buttons, from an import's result. */
void DebuggerWindow::basicNote(const a2600srcmap_import_result &r, bool failed)
{
    if (failed) {
        m_basNote->setText(QStringLiteral("Could not read that file or folder."));
        return;
    }
    QString msg = QStringLiteral("%1 file%2, %3 line%4 with code")
                      .arg(r.files).arg(r.files == 1 ? QString() : QStringLiteral("s"))
                      .arg(r.mapped).arg(r.mapped == 1 ? QString() : QStringLiteral("s"));
    if (r.map_path[0]) {
        msg += QStringLiteral(" (from %1%2)").arg(QFileInfo(QString::fromUtf8(r.map_path)).fileName(),
                                                  r.old_format ? QStringLiteral(", old compiler: matched by text") : QString());
        if (r.sym_path[0]) msg += QStringLiteral(", symbols from %1").arg(QFileInfo(QString::fromUtf8(r.sym_path)).fileName());
    } else {
        msg += QStringLiteral(". No listing found beside the source: build with bB, then Load Map.");
    }
    m_basNote->setText(msg);
    m_basGen = ~0u;
}

void DebuggerWindow::basicImported(const QString &path, bool folder)
{
    Q_UNUSED(folder);
    a2600srcmap_import_result res;
    const int rc = a2600debug_basic_import(m_dbg, path.toUtf8().constData(), &res);
    basicNote(res, rc != 0);
    if (rc == 0) m_basFile = -1;        /* the first file with code is picked */
    if (m_basView) refreshBasic();
}

void DebuggerWindow::basicPick(int kind)
{
    const QString last = QString::fromUtf8(a2600session_get_str(m_session, "bas_dir", ""));
    QString path;
    if (kind == 1) path = QFileDialog::getExistingDirectory(this, QStringLiteral("Import Folder"), last);
    else if (kind == 2) path = QFileDialog::getOpenFileName(this, QStringLiteral("Load Map"), last,
                                                            QStringLiteral("bB listings (*.lst);;All files (*)"));
    else path = QFileDialog::getOpenFileName(this, QStringLiteral("Import File"), last,
                                             QStringLiteral("batari Basic source (*.bas);;All files (*)"));
    if (path.isEmpty()) return;
    a2600session_set_str(m_session, "bas_dir", (kind == 1 ? path : QFileInfo(path).absolutePath()).toUtf8().constData());
    a2600session_settings_flush(m_session);
    if (kind == 2) {
        const int n = a2600srcmap_load_map(a2600srcmap_shared(), path.toUtf8().constData());
        m_basNote->setText(n < 0 ? QStringLiteral("Could not read that listing.")
                                 : QStringLiteral("%1 line%2 with code, from %3.").arg(n).arg(n == 1 ? QString() : QStringLiteral("s"))
                                       .arg(QFileInfo(path).fileName()));
        m_basGen = ~0u;
        refreshBasic();
    } else {
        basicImported(path, kind == 1);
    }
}

/* Rebuilt when the map, a breakpoint or the stop changed; every new stop brings
 * the machine's line into view, even when the reader has scrolled away. */
void DebuggerWindow::refreshBasic()
{
    a2600srcmap *map = a2600srcmap_shared();
    const unsigned gen = a2600srcmap_generation(map), dgen = a2600debug_generation(m_dbg);
    const bool stopped = a2600debug_is_stopped(m_dbg) != 0;
    const uint64_t serial = a2600debug_stop_serial(m_dbg);
    const int files = a2600srcmap_file_count(map);

    int pf = -1, pl = 0;
    if (stopped && a2600debug_line_at_pc(m_dbg, &pf, &pl) != 1) pf = -1, pl = 0;
    const bool newStop = stopped && serial != m_basSerial;
    if (stopped) m_basSerial = serial;
    if (gen == m_basGen && dgen == m_basDbgGen && !newStop && pf == m_basPcFile && pl == m_basPcLine) return;

    if (gen != m_basGen || m_basFiles->count() != files) {
        const QSignalBlocker block(m_basFiles);
        m_basFiles->clear();
        for (int i = 0; i < files; ++i) m_basFiles->addItem(QString::fromUtf8(a2600srcmap_file_name(map, i)));
        if (m_basFile >= files) m_basFile = -1;
        if (m_basFile < 0 && files > 0) {       /* the first file with any code */
            m_basFile = 0;
            for (int i = 0; i < files && m_basFile == 0; ++i)
                for (int l = 1, n = a2600srcmap_line_count(map, i); l <= n; ++l)
                    if (a2600srcmap_line_has_code(map, i, l)) { m_basFile = i; break; }
        }
        m_basFiles->setCurrentIndex(m_basFile);
        if (files == 0)
            m_basNote->setText(QStringLiteral("Import a .bas file, or a folder of them: its listing (.lst) puts each line at its address."));
    }
    if (pf >= 0 && pf != m_basFile && newStop) {       /* followed into another file */
        m_basFile = pf;
        m_basFiles->setCurrentIndex(pf);
    }
    m_basGen = gen;
    m_basDbgGen = dgen;

    QString text;
    QList<int> on, off, dim;
    const int n = m_basFile >= 0 ? a2600srcmap_line_count(map, m_basFile) : 0;
    text.reserve(n * 48);
    for (int i = 1; i <= n; ++i) {
        const bool code = a2600srcmap_line_has_code(map, m_basFile, i) != 0;
        const int state = code ? a2600debug_line_break_state(m_dbg, m_basFile, i) : 0;
        if (state == 1) on << i - 1; else if (state == 2) off << i - 1;
        if (!code) dim << i - 1;
        text += QStringLiteral("%1 %2  %3\n").arg(dotFor(state)).arg(i, 5).arg(QString::fromUtf8(a2600srcmap_line_text(map, m_basFile, i)));
    }
    const int top = m_basView->verticalScrollBar()->value();
    if (m_basView->toPlainText() != text) {
        m_basBar = -1;                  /* the document is new: no block wears the bar */
        m_basView->setPlainText(text);
        m_basView->verticalScrollBar()->setValue(top);
    }
    const bool onPc = pf == m_basFile && pl >= 1 && pl <= n;
    QList<QTextEdit::ExtraSelection> sel;
    QTextCharFormat grey;
    grey.setForeground(m_basView->palette().color(QPalette::Disabled, QPalette::Text));
    for (int b : dim) {
        QTextEdit::ExtraSelection s;
        s.cursor = QTextCursor(m_basView->document()->findBlockByNumber(b));
        s.cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        s.format = grey;
        sel << s;
    }
    basicBar(sel, m_basView, m_basBar, onPc ? pl - 1 : -1);
    auto dot = [&](int b, const QColor &c) {
        QTextCharFormat f;
        f.setForeground(b == pl - 1 && onPc ? accentInk() : c);
        sel << spanSelection(m_basView, b, 0, 1, f);
    };
    for (int b : on) dot(b, QColor(0xe0, 0x1b, 0x24));
    for (int b : off) dot(b, QColor(0x9a, 0x99, 0x96));
    m_basView->setExtraSelections(sel);

    const int line = onPc ? pl : 0;
    if (line != m_basPcLine || m_basPcFile != pf || (newStop && line)) {
        m_basPcLine = line;
        m_basPcFile = pf;
        basicScrollToPc();
    }
}

/* The Video tab takes its own snapshot; a stopped machine redraws only when
 * the core says something changed. */
void DebuggerWindow::refreshVideo(bool force)
{
    m_video->refresh(force);
}

void DebuggerWindow::refreshBps()
{
    static a2600debug_breakpoint bps[BP_MAX];
    const int n = a2600debug_bp_list(m_dbg, bps, BP_MAX);

    /* the Execute bank chooser follows the cartridge */
    const int banks = a2600debug_bank_count(m_dbg);
    if (m_bpBank->count() != banks + 1) {
        const int was = m_bpBank->currentIndex();
        m_bpBank->clear();
        m_bpBank->addItem(QStringLiteral("Any bank"));
        for (int i = 0; i < banks; ++i) m_bpBank->addItem(QStringLiteral("Bank %1").arg(i));
        m_bpBank->setCurrentIndex(was < m_bpBank->count() ? qMax(was, 0) : 0);
    }

    if (a2600debug_is_stopped(m_dbg)) {
        char reason[160];
        int addr = -1;
        reason[0] = '\0';
        a2600debug_stop_reason(m_dbg, reason, sizeof reason, &addr);
        m_bpStop->setText(stoppedText(stripControl(reason)) + QLatin1Char('.'));
    } else {
        m_bpStop->setText(QStringLiteral("Running."));
    }

    /* rebuilt only when it changed (each step refreshes), and never while a
     * row's own button is being handled */
    QString sig;
    for (int i = 0; i < n; ++i)
        sig += QStringLiteral("%1 %2 %3 %4 %5 %6 %7 %8\n").arg(bps[i].id).arg(bps[i].enabled).arg(bps[i].hits)
                   .arg(bps[i].type).arg(bps[i].start).arg(bps[i].end).arg(bps[i].bank)
                   .arg(QString::fromUtf8(bps[i].condition));
    if (sig == m_bpSig && m_bpTable->rowCount() == n) return;
    m_bpSig = sig;

    QFont mono = m_bpTable->font();
    mono.setFamily(QStringLiteral("monospace"));
    mono.setStyleHint(QFont::TypeWriter);
    m_bpFilling = true;
    m_bpTable->setRowCount(n);
    for (int i = 0; i < n; ++i) {
        auto *on = new QTableWidgetItem;
        on->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
        on->setCheckState(bps[i].enabled ? Qt::Checked : Qt::Unchecked);
        on->setData(Qt::UserRole, bps[i].id);
        on->setToolTip(QStringLiteral("Enabled"));
        m_bpTable->setItem(i, 0, on);
        char desc[256];
        a2600bp_describe(m_dbg, &bps[i], desc, sizeof desc);
        m_bpTable->setItem(i, 1, new QTableWidgetItem(stripControl(desc)));
        m_bpTable->setItem(i, 2, new QTableWidgetItem(QString::number(bps[i].hits)));
        for (int col = 1; col < 3; ++col) m_bpTable->item(i, col)->setFont(mono);
        auto *rm = new QToolButton;
        rm->setIcon(QIcon::fromTheme(QStringLiteral("edit-delete"), QIcon::fromTheme(QStringLiteral("user-trash"))));
        if (rm->icon().isNull()) rm->setText(QStringLiteral("Remove"));
        rm->setAutoRaise(true);
        rm->setToolTip(QStringLiteral("Remove"));
        const int id = bps[i].id;
        connect(rm, &QToolButton::clicked, this, [this, id] {
            a2600debug_bp_remove(m_dbg, id);
            syncLater();
        });
        m_bpTable->setCellWidget(i, 3, rm);
    }
    m_bpFilling = false;
    m_bpEmpty->setVisible(n == 0);
}

/* The cartridge, the RIOT and the memory map as regions (drawn here once). */
void DebuggerWindow::refreshCart()
{
    static a2600debug_region reg[REGION_MAX];
    char info[256];
    a2600debug_cart_info(m_dbg, info, sizeof info);
    a2600debug_riot r;
    a2600debug_riot_get(m_dbg, &r);

    QString s = QStringLiteral("Cartridge\n  ") + stripControl(info) + QStringLiteral("\n\nRIOT\n");
    s += QStringLiteral("  SWCHA  $%1   SWACNT $%2     left: %3   right: %4\n  SWCHB  $%5   SWBCNT $%6\n")
             .arg(hx(r.swcha, 2), hx(r.swacnt, 2), QString::fromUtf8(r.dir_left), QString::fromUtf8(r.dir_right),
                  hx(r.swchb, 2), hx(r.swbcnt, 2));
    s += QStringLiteral("  INPT0-5 ");
    for (int i = 0; i < 6; ++i) s += QStringLiteral(" $") + hx(r.inpt[i], 2);
    s += QStringLiteral("\n  Timer  INTIM $%1   TIMINT $%2   clocks %3   divider %4\n")
             .arg(hx(r.intim, 2), hx(r.timint, 2)).arg(r.tim_clocks).arg(r.tim_divider);
    s += QStringLiteral("  Switches   Select %1   Reset %2   %3   Left difficulty %4   Right difficulty %5\n")
             .arg(r.select ? "pressed" : "up", r.reset ? "pressed" : "up", r.color ? "Color" : "B&W",
                  r.diff_left_a ? "A" : "B", r.diff_right_a ? "A" : "B");

    s += QStringLiteral("\nMemory map\n");
    const int n = a2600debug_memory_map(m_dbg, reg, REGION_MAX);
    for (int i = 0; i < n; ++i)
        s += QStringLiteral("  $%1-$%2  %3%4  %5\n").arg(hx(reg[i].lo, 4), hx(reg[i].hi, 4))
                 .arg(reg[i].readable ? QLatin1Char('r') : QLatin1Char('-'))
                 .arg(reg[i].writable ? QLatin1Char('w') : QLatin1Char('-'))
                 .arg(QString::fromUtf8(reg[i].name));
    setText(m_io, s);
}

/* The TIA's two channels, with Stella's frequency text. */
void DebuggerWindow::refreshSound()
{
    a2600debug_sound a;
    a2600debug_sound_get(m_dbg, &a);
    QString s = QStringLiteral("TIA\n");
    for (int ch = 0; ch < 2; ++ch)
        s += QStringLiteral("  Channel %1   AUDC $%2   AUDF $%3   AUDV $%4   %5%6\n").arg(ch)
                 .arg(hx(a.audc[ch], 2), hx(a.audf[ch], 2), hx(a.audv[ch], 2), QString::fromUtf8(a.freq[ch]),
                      (a.audv[ch] & 0x0F) == 0 ? QStringLiteral("  (silent)") : QString());
    setText(m_sound, s);
}

/* The labels the engine holds, filtered, in the order the filter gives
 * (address, then name). The table is rebuilt only when what it would show
 * changed: the engine's own index order is not stable, so the entries are
 * fetched anew each time and compared by content. */
void DebuggerWindow::refreshSymbols(bool force)
{
    const int total = a2600debug_symbols_count(m_dbg);
    const QByteArray filter = m_symFilter->text().trimmed().toUtf8();
    std::vector<int> indices((size_t)qMax(total, 0));
    const int shown = total > 0 ? a2600debug_symbols_filter(m_dbg, filter.constData(), indices.data(), total) : 0;

    struct Row { uint16_t addr; char name[96]; };
    std::vector<Row> rows;
    rows.reserve((size_t)shown);
    QString sig = QStringLiteral("%1/%2/%3:").arg(total).arg(shown).arg(QString::fromUtf8(filter));
    for (int i = 0; i < shown; ++i) {
        Row row = {};
        if (!a2600debug_symbols_get(m_dbg, indices[(size_t)i], &row.addr, row.name, (int)sizeof row.name)) continue;
        rows.push_back(row);
        sig += QStringLiteral("%1 %2;").arg(row.addr).arg(QString::fromUtf8(row.name));
    }
    if (!force && sig == m_symSig) return;
    m_symSig = sig;

    m_symCount->setText(QStringLiteral("%1 of %2 symbols").arg(rows.size()).arg(total));
    const int top = m_symTable->verticalScrollBar()->value();
    m_symTable->setUpdatesEnabled(false);
    m_symTable->setRowCount((int)rows.size());
    for (int i = 0; i < (int)rows.size(); ++i) {
        auto *addr = new QTableWidgetItem(QStringLiteral("$") + hx(rows[(size_t)i].addr, 4));
        addr->setData(Qt::UserRole, rows[(size_t)i].addr);
        m_symTable->setItem(i, 0, addr);
        m_symTable->setItem(i, 1, new QTableWidgetItem(QString::fromUtf8(rows[(size_t)i].name)));
    }
    m_symTable->setUpdatesEnabled(true);
    m_symTable->verticalScrollBar()->setValue(top);
}

void DebuggerWindow::addWatch(const QString &entry)
{
    const QByteArray text = entry.trimmed().toUtf8();
    if (text.isEmpty()) return;
    if (a2600watch_add(text.constData()) < 0) {
        message(QStringLiteral("The watch list is full"));
        return;
    }
    m_wtEntry->clear();
    refreshWatches(true);
}

/* Break on change: stop after a write over the watch that alters a byte. */
void DebuggerWindow::watchBreak(int index)
{
    uint16_t lo, hi;
    if (!a2600watch_range(m_dbg, index, &lo, &hi)) {
        message(QStringLiteral("That watch does not resolve yet"));
        return;
    }
    if (a2600debug_bp_add(m_dbg, A2600DEBUG_BP_WRITE, lo, hi, A2600DEBUG_ANY_BANK, "value != old") < 0)
        message(QStringLiteral("The breakpoint could not be added"));
    else
        message(QStringLiteral("Breaking when it changes"));
    resync();
}

/* One row a watch, its value live (a dash while the machine runs). The rows
 * are rebuilt when the list changed; otherwise only their text moves. */
void DebuggerWindow::refreshWatches(bool force)
{
    const int n = a2600watch_count();
    const unsigned gen = a2600watch_generation();
    QFont mono = m_wtTable->font();
    mono.setFamily(QStringLiteral("monospace"));
    mono.setStyleHint(QFont::TypeWriter);
    if (force || gen != m_wtGen || m_wtTable->rowCount() != n) {
        m_wtGen = gen;
        m_wtTable->setRowCount(n);
        for (int i = 0; i < n; ++i) {
            auto *item = new QTableWidgetItem;
            item->setFont(mono);
            m_wtTable->setItem(i, 0, item);
            auto *box = new QWidget;
            auto *h = new QHBoxLayout(box);
            h->setContentsMargins(2, 0, 2, 0);
            auto *mem = new QPushButton(QStringLiteral("Memory"));
            mem->setToolTip(QStringLiteral("Show it in the Memory tab"));
            connect(mem, &QPushButton::clicked, this, [this, i] {
                uint16_t lo, hi;
                if (!a2600watch_range(m_dbg, i, &lo, &hi)) {
                    message(QStringLiteral("That watch does not resolve yet"));
                    return;
                }
                if (m_popped[TAB_MEMORY]) m_popped[TAB_MEMORY]->activateWindow();
                else m_tabs->setCurrentWidget(m_pages[TAB_MEMORY]);
                showInMemory(lo, hi);
            });
            auto *brk = new QPushButton(QStringLiteral("Break on change"));
            brk->setToolTip(QStringLiteral("Stop after a write that alters it"));
            connect(brk, &QPushButton::clicked, this, [this, i] { watchBreak(i); });
            auto *rm = new QPushButton(QStringLiteral("Remove"));
            connect(rm, &QPushButton::clicked, this, [this, i] {
                a2600watch_remove(i);
                QTimer::singleShot(0, this, [this] { refreshWatches(true); });
            });
            for (QPushButton *b : { mem, brk, rm }) {
                b->setFocusPolicy(Qt::NoFocus);
                h->addWidget(b);
            }
            m_wtTable->setCellWidget(i, 1, box);
        }
    }
    for (int i = 0; i < n; ++i) {
        char line[256];
        a2600watch_format(m_dbg, i, line, sizeof line);
        const QString text = stripControl(line);
        if (m_wtTable->item(i, 0)->text() != text) m_wtTable->item(i, 0)->setText(text);
    }
}

/* Trace (live) and History (at a stop): the ring newest first, shown
 * oldest first, the newest HIST_SHOWN lines that pass the filter. */
void DebuggerWindow::refreshHist(int idx, bool force)
{
    static a2600debug_trace_entry buf[RING_MAX];
    const bool stopped = a2600debug_is_stopped(m_dbg) != 0;
    const bool on = a2600debug_trace_enabled(m_dbg) != 0;
    const int count = a2600debug_trace_count(m_dbg);

    m_hSync = true;                     /* both Record buttons follow the engine */
    for (int i = 0; i < 2; ++i)
        if (m_hRec[i]->isChecked() != on) m_hRec[i]->setChecked(on);
    m_hSync = false;

    if (force) m_hSeen[idx] = -1;
    /* History reads at a stop (it keeps what it showed while the machine
     * runs); at a stop neither view rebuilds for the same count */
    if (idx == 1 && !stopped && m_hSeen[1] >= 0) return;
    if (stopped && count == m_hSeen[idx]) return;
    m_hSeen[idx] = count;

    uint16_t lo = 0, hi = 0xFFFF;
    int filter = A2600DEBUG_TRACE_ALL;
    switch (m_hFilter[idx]->currentIndex()) {
    case 1: filter = A2600DEBUG_TRACE_BRANCHES; break;
    case 2:
        if (a2600sym_parse_range(m_dbg, m_hRange[idx]->text().toUtf8().constData(), &lo, &hi))
            filter = A2600DEBUG_TRACE_RANGE;
        break;
    default: break;
    }
    const int n = a2600debug_trace_read(m_dbg, buf, RING_MAX);
    QList<int> hit;
    for (int i = 0; i < n && hit.size() < HIST_SHOWN; ++i)
        if (a2600debug_trace_match(&buf[i], filter, lo, hi)) hit.append(i);
    QString t;
    if (!on && n == 0) t = QStringLiteral("Recording is off. Press Record, then let the machine run.");
    char line[256];
    for (int k = (int)hit.size() - 1; k >= 0; --k) {
        a2600debug_trace_format(&buf[hit[k]], line, sizeof line);
        t += QString::fromUtf8(line);
        if (k) t += QLatin1Char('\n');
    }
    setText(m_hText[idx], t, true);
    m_hNote[idx]->setText(on ? QStringLiteral("recording, %1 instructions").arg(n)
                             : n ? QStringLiteral("recording off, %1 instructions").arg(n)
                                 : QStringLiteral("recording off"));
}

/* The packet log is rebuilt only when it changed: a new packet, or a
 * repeat (which moves the newest serial on). */
void DebuggerWindow::refreshFujinet(bool force)
{
    static a2600_fuji_packet p[TRACE_MAX];
    static char state[2048];
    const int n = a2600debug_fujinet_trace(m_dbg, p, TRACE_MAX);
    if (a2600session_fujinet_external(m_session)) {
        const int link = a2600session_cart_link_up(m_session);
        m_fujiNote->setText(QStringLiteral("External FujiNet at %1%2")
                                .arg(QString::fromUtf8(a2600session_boip_endpoint(m_session)))
                                .arg(link > 0 ? QStringLiteral(", connected.")
                                     : link == 0 ? QStringLiteral(", not connected.")
                                                 : QStringLiteral("; a local cartridge has no link.")));
    } else if (!a2600session_fujinet_running(m_session)) {
        m_fujiNote->setText(QStringLiteral("FujiNet is off (Preferences ▸ Host ▸ Enable FujiNet)."));
    } else {
        const int link = a2600session_cart_link_up(m_session);
        m_fujiNote->setText(QStringLiteral("FujiNet is on BoIP %1%2").arg(A2600SESSION_BOIP_PORT)
                                .arg(link > 0 ? QStringLiteral(", connected.")
                                     : link == 0 ? QStringLiteral(", not connected.")
                                                 : QStringLiteral("; a local cartridge has no link.")));
    }
    state[0] = '\0';
    a2600debug_fujinet_state(m_dbg, state, int(sizeof state));
    setText(m_fujiState, stripControl(state));

    if (!force && n == m_fujiCount && (n == 0 || p[n - 1].serial == m_fujiLast)) return;
    const bool follow = force || m_fujiCount < 0 || atEnd(m_fujiText);
    m_fujiCount = n;
    m_fujiLast = n > 0 ? p[n - 1].serial : 0;
    QString t;
    for (int i = 0; i < n; ++i) {
        if (i) t += QLatin1Char('\n');
        t += QString::fromUtf8(p[i].text);
        if (p[i].repeat > 1) t += QStringLiteral(" ×%1").arg(p[i].repeat);
        t += QStringLiteral("\n     ");
        for (int k = 0; k < p[i].nbytes && k < int(sizeof p[i].data); k++) t += hx(p[i].data[k], 2) + QLatin1Char(' ');
        if (p[i].nbytes > int(sizeof p[i].data)) t += QStringLiteral("…");
    }
    if (n == 0) t = QStringLiteral("(no FujiBus packets since the trace was cleared)");
    setText(m_fujiText, t, follow);
}

/* One tab; live is the twice-a-second refresh while the machine runs, which
 * leaves out what only changes at a stop. */
void DebuggerWindow::refreshPage(int page, bool live)
{
    switch (page) {
    case TAB_DISASM: if (!live) refreshDisasm(); break;
    case TAB_BASIC: refreshBasic(); break;
    case TAB_MEMORY: refreshMem(); break;
    case TAB_VIDEO: refreshVideo(std::exchange(m_videoForce, false)); break;
    case TAB_BREAKS: refreshBps(); break;
    case TAB_WATCHES: refreshWatches(false); break;
    case TAB_CART: refreshCart(); break;
    case TAB_TRACE: refreshHist(0, false); break;
    case TAB_HISTORY: if (!live) refreshHist(1, false); break;
    case TAB_SOUND: refreshSound(); break;
    case TAB_SYMBOLS: refreshSymbols(false); break;
    case TAB_FUJINET: refreshFujinet(false); break;
    default: break;
    }
}

/* The registers show everywhere; then the tab showing and any in a window of
 * its own. */
void DebuggerWindow::refreshAll()
{
    refreshStatus();
    refreshCpu();
    refreshPage(currentTab(), false);
    for (int t = 0; t < TAB_COUNT; ++t)
        if (m_popped[t]) refreshPage(t, false);
}

/* A page's number, whichever place it holds in the tab widget now. */
int DebuggerWindow::currentTab() const
{
    QWidget *w = m_tabs->currentWidget();
    return w ? w->property("tab").toInt() : -1;
}

/* On screen: the tab widget's page, or a window of its own. */
bool DebuggerWindow::tabVisible(int tab) const
{
    return currentTab() == tab || m_popped[tab] != nullptr;
}

/* A new stop brings every view to the spot, whatever the reader had
 * scrolled to: the Disassembly to the PC, Memory to the range it marks (or
 * the PC), the Trace and History to their newest line, the Video tab to a
 * redraw. */
void DebuggerWindow::recentreOnStop()
{
    m_followPc->setChecked(true);
    m_disasm->verticalScrollBar()->setValue(0);
    m_hSeen[0] = m_hSeen[1] = -1;
    m_videoForce = true;
    refreshAll();
    a2600debug_cpu c;
    a2600debug_cpu_get(m_dbg, &c);
    scrollMemory(m_hl ? m_hlLo : (uint16_t)c.pc);
    for (QPlainTextEdit *view : m_hText)
        view->verticalScrollBar()->setValue(view->verticalScrollBar()->maximum());
}

/* After anything that may have moved the engine: the console notes a new
 * stop, then every view follows it. */
void DebuggerWindow::resync()
{
    const bool stopped = a2600debug_is_stopped(m_dbg) != 0;
    const uint64_t serial = a2600debug_stop_serial(m_dbg);
    const bool newStop = stopped && serial != m_seenSerial;
    if (newStop) {
        char reason[160];
        int addr;
        reason[0] = '\0';
        a2600debug_stop_reason(m_dbg, reason, sizeof reason, &addr);
        /* a step already says where it is */
        if (reason[0] && strncmp(reason, "step", 4) != 0)
            consoleAppend(QStringLiteral("— %1\n").arg(stripControl(reason)));
    }
    m_seenSerial = serial;
    m_wasStopped = stopped;
    m_seenGen = a2600debug_generation(m_dbg);
    m_runningTicks = 0;
    if (newStop) recentreOnStop();
    else refreshAll();
}

/* From inside a breakpoint row's own signal: the table rebuilds after. */
void DebuggerWindow::syncLater()
{
    QTimer::singleShot(0, this, [this] { resync(); });
}

/* A step takes microseconds: a moment's wait for the stop keeps the window
 * from flashing "Running" in between. */
void DebuggerWindow::settleAndSync()
{
    for (int i = 0; i < 40 && !a2600debug_is_stopped(m_dbg); ++i) QThread::msleep(1);
    resync();
}

/* The engine reports a stop through the generation and the stop serial, so
 * this polls them: a refresh when they moved, and the live values twice a
 * second while the machine runs. */
void DebuggerWindow::tick()
{
    if (!isVisible()) return;
    applyEnvBreak();
    {
        a2600srcmap_import_result res;
        if (a2600debug_basic_auto_import(m_dbg, &res) == 1) basicNote(res, false);
    }
    if (a2600srcmap_generation(a2600srcmap_shared()) != m_basGen && tabVisible(TAB_BASIC)) refreshBasic();
    const unsigned gen = a2600debug_generation(m_dbg);
    const bool stopped = a2600debug_is_stopped(m_dbg) != 0;
    if (gen != m_seenGen || stopped != m_wasStopped || a2600debug_stop_serial(m_dbg) != m_seenSerial) {
        resync();
    } else if (tabVisible(TAB_WATCHES) && a2600watch_generation() != m_wtGen) {
        refreshWatches(false);          /* the list changed elsewhere (the console) */
    } else if (!stopped && ++m_runningTicks >= 4) {
        m_runningTicks = 0;
        refreshStatus();
        refreshCpu();
        refreshPage(currentTab(), true);
        for (int t = 0; t < TAB_COUNT; ++t)
            if (m_popped[t]) refreshPage(t, true);
    }
}

/* ---- the console ---------------------------------------------------------- */

/* Onto the transcript, which follows its end while the reader is there. */
void DebuggerWindow::consoleAppend(const QString &text)
{
    const bool follow = atEnd(m_conOut);
    QTextCursor cur(m_conOut->document());
    cur.movePosition(QTextCursor::End);
    cur.insertText(text);
    if (follow) m_conOut->verticalScrollBar()->setValue(m_conOut->verticalScrollBar()->maximum());
}

void DebuggerWindow::runConsole()
{
    static char out[65536];
    const QString cmd = m_conIn->text().trimmed();
    if (cmd.isEmpty()) return;
    if (m_history.isEmpty() || m_history.last() != cmd) m_history << cmd;
    m_historyPos = (int)m_history.size();
    /* a command brings the reader back down */
    m_conOut->verticalScrollBar()->setValue(m_conOut->verticalScrollBar()->maximum());
    consoleAppend(QStringLiteral("> %1\n").arg(cmd));
    m_conIn->clear();

    out[0] = '\0';
    a2600debug_command(m_dbg, cmd.toUtf8().constData(), out, sizeof out);
    QString text = stripControl(out);
    if (!text.isEmpty()) consoleAppend(text.endsWith(QLatin1Char('\n')) ? text : text + QLatin1Char('\n'));
    settleAndSync();
}

/* Tab: the engine completes the word at the end of the line. One match
 * replaces it; several are listed and the line extends to what they share. */
void DebuggerWindow::completeConsole()
{
    char comps[4096];
    const QString text = m_conIn->text();
    comps[0] = '\0';
    const int n = a2600debug_completions(m_dbg, text.toUtf8().constData(), comps, sizeof comps);
    if (n <= 0) return;
    const QStringList words = stripControl(comps).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (words.isEmpty()) return;
    const int sp = text.lastIndexOf(QLatin1Char(' '));
    const QString head = text.left(sp + 1);
    if (words.size() == 1) {
        m_conIn->setText(head + words[0] + QLatin1Char(' '));
        return;
    }
    QString common = words[0];
    for (const QString &w : words)
        while (!w.startsWith(common)) common.chop(1);
    consoleAppend(words.join(QStringLiteral("  ")) + QLatin1Char('\n'));
    if (common.size() > text.size() - sp - 1) m_conIn->setText(head + common);
}

/* Messages land in the console and, since it may not be the tab showing,
 * the status bar. */
void DebuggerWindow::say(const QString &msg)
{
    consoleAppend(msg + QLatin1Char('\n'));
    message(msg);
}

void DebuggerWindow::message(const QString &msg)
{
    statusBar()->showMessage(msg, 6000);
}

/* ---- the rest of the handlers ----------------------------------------------- */

void DebuggerWindow::toggleRun()
{
    if (a2600debug_is_stopped(m_dbg)) {
        a2600debug_resume(m_dbg);
        resync();
    } else {
        a2600debug_stop(m_dbg);
        settleAndSync();
    }
}

void DebuggerWindow::stepAnd(const std::function<void()> &fn)
{
    if (!a2600debug_is_stopped(m_dbg)) {
        message(QStringLiteral("Stop the machine first"));
        return;
    }
    fn();
    settleAndSync();
}

void DebuggerWindow::runToCursor()
{
    if (m_selected < 0) {
        message(QStringLiteral("Select a line in the Disassembly to run to"));
        return;
    }
    stepAnd([this] { a2600debug_run_to(m_dbg, (uint16_t)m_selected); });
}

void DebuggerWindow::toggleBreakpoint(int addr)
{
    a2600debug_breakpoint_toggle(m_dbg, (uint16_t)addr, disasmBank());
    resync();
}

void DebuggerWindow::toggleSelected()
{
    if (m_selected < 0) {
        message(QStringLiteral("Select a line in the Disassembly first"));
        return;
    }
    toggleBreakpoint(m_selected);
}

/* The toolbar's Break at: a symbol or a hex address, an execute breakpoint
 * there (one already set stays). */
void DebuggerWindow::breakAt()
{
    const QString text = m_breakAt->text().trimmed();
    if (text.isEmpty()) return;
    long addr;
    QString why;
    if (!addressOf(text, &addr, &why)) {
        message(why);
        return;
    }
    if (a2600debug_breakpoint_check(m_dbg, (uint16_t)addr, A2600DEBUG_ANY_BANK))
        message(QStringLiteral("A breakpoint is already set at $%1").arg(hx((unsigned)addr, 4)));
    else {
        a2600debug_breakpoint_toggle(m_dbg, (uint16_t)addr, A2600DEBUG_ANY_BANK);
        message(QStringLiteral("Breakpoint at $%1").arg(hx((unsigned)addr, 4)));
    }
    m_breakAt->clear();
    resync();
}

/* A number typed into an entry: a label first, then bare digits as hex (as in
 * the sibling debuggers), then whatever the engine evaluates ($4000, 0x4000,
 * label+2).
 * When it is none of those, the reason goes to *why, or to the status bar
 * without. */
bool DebuggerWindow::numberOf(const QString &text, long *out, QString *why)
{
    static const QRegularExpression hexOnly(QStringLiteral("^[0-9A-Fa-f]+$"));
    const QString t = text.trimmed();
    const QByteArray utf8 = t.toUtf8();
    long v = 0;
    QString reason;
    uint16_t lo, hi;
    const int label = a2600debug_label_address(m_dbg, utf8.constData());
    if (label >= 0) {
        v = label;
    } else if (hexOnly.match(t).hasMatch()) {
        bool ok;
        v = t.toLong(&ok, 16);
        if (!ok) reason = QStringLiteral("Not a number");
    } else if (a2600sym_parse_range(m_dbg, utf8.constData(), &lo, &hi) && lo == hi) {
        v = lo;
    } else {
        reason = QStringLiteral("Not a number ($4000, or a label): %1").arg(t);
    }
    if (reason.isEmpty()) {
        *out = v;
        return true;
    }
    if (why) *why = reason;
    else message(reason);
    return false;
}

/* The same, as an address: 0 to $FFFF. */
bool DebuggerWindow::addressOf(const QString &text, long *out, QString *why)
{
    long v;
    if (!numberOf(text, &v, why)) return false;
    if (v < 0 || v > 0xFFFF) {
        const QString reason = QStringLiteral("Out of range ($0000-$FFFF)");
        if (why) *why = reason;
        else message(reason);
        return false;
    }
    *out = v;
    return true;
}

/* A breakpoint's range from its two entries. The start may itself be a
 * range (lo-hi); the end is optional. */
bool DebuggerWindow::rangeOf(const QString &startText, const QString &endText, uint16_t *lo, uint16_t *hi,
                             QString *why)
{
    long first = 0, last = 0;
    const QString start = startText.trimmed(), end = endText.trimmed();
    if (end.isEmpty()) {
        uint16_t from, to;
        if (!a2600sym_parse_range(m_dbg, start.toUtf8().constData(), &from, &to)) {
            *why = QStringLiteral("Not an address, label or range: %1").arg(start);
            return false;
        }
        first = from;
        last = to;
    } else {
        if (!addressOf(start, &first, why) || !addressOf(end, &last, why)) return false;
    }
    if (last < first) {
        *why = QStringLiteral("The end is before the start");
        return false;
    }
    *lo = (uint16_t)first;
    *hi = (uint16_t)last;
    return true;
}

/* Edits go in only while the machine is stopped. A register entry shows hex
 * digits, so bare digits there are hex; anything else is an expression. */
void DebuggerWindow::setReg(int i)
{
    QLineEdit *e = m_reg[i];
    long v = 0;
    QString why;
    const bool ok = numberOf(e->text(), &v, &why);
    if (!a2600debug_is_stopped(m_dbg)) message(QStringLiteral("Stop the machine first"));
    else if (!ok) message(why);
    else a2600debug_cpu_set(m_dbg, kRegs[i].id, (int)v);
    e->clearFocus();
    resync();
}

void DebuggerWindow::setFlag(int id, bool on)
{
    if (!a2600debug_is_stopped(m_dbg)) message(QStringLiteral("Stop the machine first"));
    else a2600debug_cpu_set(m_dbg, id, on ? 1 : 0);
    resync();
}

/* Shows a range in the dump, from just above it, marked and scrolled to. */
void DebuggerWindow::showInMemory(uint16_t lo, uint16_t hi)
{
    m_hl = true;
    m_hlLo = lo;
    m_hlHi = hi;
    refreshMem();
    scrollMemory(lo);
}

/* Opens an address in the Disassembly (the tab, or its window), the PC no
 * longer followed, the line selected. */
void DebuggerWindow::showInDisassembly(uint16_t addr)
{
    if (m_popped[TAB_DISASM]) m_popped[TAB_DISASM]->activateWindow();
    else m_tabs->setCurrentWidget(m_pages[TAB_DISASM]);
    m_followPc->setChecked(false);
    m_selected = addr;
    const int line = lineForAddress(addr);
    if (line >= 0) m_disasmFirst = qMax(0, line - visibleRows() / 3);
    refreshDisasm();
}

/* Go to: a label, an address, an expression or a range (lo-hi), which is
 * marked. Bare digits are hex, as everywhere in the entries. */
void DebuggerWindow::memGoto()
{
    const QString text = m_memGoto->text().trimmed();
    if (text.isEmpty()) return;
    uint16_t lo, hi;
    if (!a2600sym_parse_range(m_dbg, text.toUtf8().constData(), &lo, &hi)) {
        m_hl = false;
        refreshMem();
        message(QStringLiteral("Not an address, label or range: %1").arg(text));
        return;
    }
    if (lo > 0x1FFF || hi > 0x1FFF) {       /* the 6507's 13-bit bus: $F000 is $1000 */
        lo &= 0x1FFF;
        hi &= 0x1FFF;
        if (hi < lo) hi = lo;
    }
    showInMemory(lo, hi);
}

/* Find Next / Previous from the marked range (or the top of the view); the
 * hit is marked. */
void DebuggerWindow::memSearch(bool backward)
{
    uint8_t pat[A2600SEARCH_MAX], mask = 0xFF;
    int n = 0, flags = 0;
    if (!a2600search_parse(m_srchKind->currentIndex(), m_srchEntry->text().toUtf8().constData(), pat, &n, &mask,
                         &flags) || n <= 0) {
        m_srchNote->setText(QStringLiteral("Type hex bytes (3E 5A) or some text."));
        return;
    }
    uint16_t from = 0, found = 0;
    if (m_hl) from = backward ? m_hlLo : (uint16_t)(m_hlLo + 1);
    const int r = a2600debug_search_next(m_dbg, pat, n, mask, flags, from, backward ? 1 : 0, &found);
    if (r < 0) {
        m_srchNote->setText(QStringLiteral("Stop the machine first"));
        message(QStringLiteral("Stop the machine first"));
    } else if (r == 0) {
        m_srchNote->setText(QStringLiteral("Not found"));
        message(QStringLiteral("Not found"));
    } else {
        m_srchNote->setText(QStringLiteral("Found at $%1").arg(hx(found, 4)));
        showInMemory(found, (uint16_t)(found + n - 1));
    }
}

void DebuggerWindow::memWrite()
{
    long a, v;
    if (!addressOf(m_memAddr->text(), &a)) return;
    if (!numberOf(m_memVal->text(), &v)) return;
    if (v < 0 || v > 0xFF) {
        message(QStringLiteral("The value is a byte ($00-$FF)"));
        return;
    }
    a2600debug_write(m_dbg, (uint16_t)(a & 0x1FFF), (uint8_t)v);
    resync();
}

void DebuggerWindow::jumpTo()
{
    long a;
    if (!addressOf(m_jump->text(), &a)) return;
    m_followPc->setChecked(false);
    m_selected = (int)a;
    const int line = lineForAddress((uint16_t)a);
    if (line >= 0) m_disasmFirst = qMax(0, line - visibleRows() / 3);
    refreshDisasm();
}

/* The start takes an address, range or label; the end is optional; the
 * condition is an expression the engine checks. */
void DebuggerWindow::addBreakpoint()
{
    const int type = bpTypeAt(m_bpType->currentIndex());
    const QString start = m_bpStart->text().trimmed(), end = m_bpEnd->text().trimmed();
    if (start.isEmpty()) return;
    uint16_t lo = 0, hi = 0;
    QString why;
    if (!rangeOf(start, end, &lo, &hi, &why)) {
        m_bpMsg->setText(why);
        return;
    }
    const QByteArray cond = m_bpCond->text().trimmed().toUtf8();
    const int bank = m_bpBank->currentIndex() <= 0 ? A2600DEBUG_ANY_BANK : m_bpBank->currentIndex() - 1;
    if (a2600debug_bp_add(m_dbg, type, lo, hi, bank, cond.constData()) < 0) {
        m_bpMsg->setText(QStringLiteral("The condition does not parse"));
        return;
    }
    m_bpMsg->clear();
    m_bpStart->clear();
    m_bpEnd->clear();
    m_bpCond->clear();
    resync();
}

/* The DASM .sym or .lst beside the opened cartridge, as Stella reads it. */
void DebuggerWindow::loadSymbols()
{
    char msg[512] = "";
    a2600debug_load_symbols(m_dbg, msg, sizeof msg);
    say(msg[0] ? stripControl(msg) : QStringLiteral("No symbols found"));
    refreshSymbols(true);
    resync();
}

/* The Symbols tab's Define: a label for an address. */
void DebuggerWindow::defineSymbol()
{
    long addr;
    const QString name = m_symName->text().trimmed();
    if (name.isEmpty() || !addressOf(m_symAddr->text(), &addr)) return;
    if (a2600debug_set_label(m_dbg, (uint16_t)addr, name.toUtf8().constData()) < 0) {
        message(QStringLiteral("That label was refused"));
        return;
    }
    m_symAddr->clear();
    m_symName->clear();
    refreshSymbols(true);
    resync();
}

void DebuggerWindow::clearSymbols()
{
    a2600debug_clear_symbols(m_dbg);
    say(QStringLiteral("Symbols cleared"));
    refreshSymbols(true);
    resync();
}

void DebuggerWindow::save(const char *kind, const QString &title, const QString &file)
{
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save %1").arg(title), file);
    if (path.isEmpty()) return;
    char msg[512] = "";
    a2600debug_save(m_dbg, kind, path.toLocal8Bit().constData(), msg, sizeof msg);
    say(msg[0] ? stripControl(msg) : QStringLiteral("Saved %1").arg(path));
}

/* ---- tabs in windows of their own ----------------------------------------- */

namespace {
/* A tab's window: closing it docks the tab again. */
class PopWindow : public QWidget {
public:
    std::function<void()> onClose;
protected:
    void closeEvent(QCloseEvent *e) override
    {
        if (onClose) onClose();
        QWidget::closeEvent(e);
    }
};
} // namespace

void DebuggerWindow::popOut(int tab)
{
    if (tab < 0 || tab >= TAB_COUNT || m_popped[tab] || m_tabs->count() <= 1) return;     /* one tab always stays */
    QWidget *page = m_pages[tab];
    m_tabs->removeTab(m_tabs->indexOf(page));

    auto *win = new PopWindow;
    win->setAttribute(Qt::WA_DeleteOnClose);
    win->setObjectName(QStringLiteral("debugger-pop-%1").arg(QLatin1String(kTabs[tab].name)));
    win->setWindowTitle(QStringLiteral("Atari 2600 Debugger - %1").arg(QLatin1String(kTabs[tab].label)));
    win->setProperty("a2600HelpContext", QStringLiteral("debugger/%1").arg(QLatin1String(kTabs[tab].help)));
    auto *layout = new QVBoxLayout(win);
    auto *bar = new QHBoxLayout;
    auto *dockBtn = new QPushButton(QStringLiteral("Dock"));
    dockBtn->setToolTip(QStringLiteral("Put this tab back in the debugger"));
    bar->addWidget(dockBtn);
    bar->addStretch(1);
    bar->addWidget(a2600HelpButton(QString()));
    layout->addLayout(bar);
    layout->addWidget(page, 1);
    page->show();
    win->resize(760, 520);
    m_popped[tab] = win;
    static_cast<PopWindow *>(win)->onClose = [this, tab] {
        if (!m_closing) dock(tab);
    };
    connect(dockBtn, &QPushButton::clicked, win, &QWidget::close);
    /* the stepping keys work in the window too */
    new QShortcut(QKeySequence(Qt::Key_F1), win, [win] { a2600HelpFor(win); });
    new QShortcut(QKeySequence(Qt::Key_F5), win, [this] { toggleRun(); });
    new QShortcut(QKeySequence(Qt::Key_F7), win, [this] { stepAnd([this] { doStep(); }); });
    new QShortcut(QKeySequence(Qt::Key_F8), win, [this] { stepAnd([this] { doStepOver(); }); });
    new QShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F8), win, [this] { stepAnd([this] { doStepOut(); }); });
    if (isVisible()) win->show();
    if (tab == TAB_VIDEO) m_videoForce = true;
    if (tab == TAB_HISTORY) m_hSeen[1] = -1;
    refreshPage(tab, false);
}

void DebuggerWindow::dock(int tab)
{
    QWidget *win = m_popped[tab];
    if (!win) return;
    QWidget *page = m_pages[tab];
    m_popped[tab] = nullptr;
    int pos = 0;
    for (int t = 0; t < tab; ++t)       /* back where it was in the order */
        if (!m_popped[t]) ++pos;
    page->setParent(nullptr);
    m_tabs->insertTab(pos, page, QString::fromUtf8(kTabs[tab].label).replace(QLatin1Char('&'), QLatin1String("&&")));
    m_tabs->setCurrentIndex(pos);
    if (tab == TAB_VIDEO) m_videoForce = true;
    if (tab == TAB_HISTORY) m_hSeen[1] = -1;
    refreshPage(tab, false);
}

/* ---- events --------------------------------------------------------------- */

bool DebuggerWindow::eventFilter(QObject *obj, QEvent *e)
{
    /* Tab completes; Up and Down walk the history */
    if (obj == m_conIn && e->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(e);
        if (ke->key() == Qt::Key_Tab) {
            completeConsole();
            return true;
        }
        if (ke->key() == Qt::Key_Up || ke->key() == Qt::Key_Down) {
            if (ke->key() == Qt::Key_Up && m_historyPos > 0) --m_historyPos;
            else if (ke->key() == Qt::Key_Down && m_historyPos < m_history.size()) ++m_historyPos;
            m_conIn->setText(m_historyPos < m_history.size() ? m_history.at(m_historyPos) : QString());
            return true;
        }
    }
    if (m_basView && obj == m_basView->viewport()) {
        /* A click on a line toggles its breakpoint; nothing else moves the cursor */
        if (e->type() == QEvent::MouseButtonPress || e->type() == QEvent::MouseButtonDblClick) {
            auto *me = static_cast<QMouseEvent *>(e);
            if (me->button() == Qt::LeftButton && e->type() == QEvent::MouseButtonPress)
                basicToggleAt(m_basView->cursorForPosition(me->position().toPoint()).blockNumber() + 1);
            return true;
        }
        if (e->type() == QEvent::MouseButtonRelease || e->type() == QEvent::MouseMove) return true;
        return QMainWindow::eventFilter(obj, e);
    }
    if (m_disasm && obj == m_disasm->viewport()) {
        /* A click selects a line (Run to Cursor); a click on its left edge,
         * or a double-click, toggles its breakpoint. A label has no line of
         * its own, so there is nothing else to hit. */
        if (e->type() == QEvent::MouseButtonPress || e->type() == QEvent::MouseButtonDblClick) {
            auto *me = static_cast<QMouseEvent *>(e);
            if (me->button() != Qt::LeftButton) return true;
            const QTextCursor cur = m_disasm->cursorForPosition(me->position().toPoint());
            const int line = cur.blockNumber();
            if (line < 0 || line >= (int)m_lineAddr.size()) return true;
            const bool gutter = cur.positionInBlock() <= 2;
            const bool dbl = e->type() == QEvent::MouseButtonDblClick;
            m_selected = m_lineAddr[(size_t)line];
            if (gutter ? !dbl : dbl) toggleBreakpoint(m_lineAddr[(size_t)line]);
            else refreshDisasm();
            return true;
        }
        if (e->type() == QEvent::MouseButtonRelease || e->type() == QEvent::MouseMove) return true;
        if (e->type() == QEvent::Wheel) {
            m_wheel += static_cast<QWheelEvent *>(e)->angleDelta().y();
            const int rows = -m_wheel / 40;     /* a notch is three rows */
            if (rows != 0) {
                m_wheel += rows * 40;
                m_followPc->setChecked(false);
                m_disasmFirst += rows;
                refreshDisasm();
            }
            return true;
        }
        if (e->type() == QEvent::Resize && isVisible()) refreshDisasm();
    }
    return QMainWindow::eventFilter(obj, e);
}

/* These keys are the debugger's here, taken as plain key presses ahead of
 * any application-wide shortcut. */
bool DebuggerWindow::event(QEvent *e)
{
    if (e->type() == QEvent::ShortcutOverride && isDebuggerKey(static_cast<QKeyEvent *>(e))) {
        e->accept();
        return true;
    }
    return QMainWindow::event(e);
}

void DebuggerWindow::keyPressEvent(QKeyEvent *e)
{
    if (!isDebuggerKey(e)) {
        QMainWindow::keyPressEvent(e);
        return;
    }
    const bool shift = e->modifiers() & Qt::ShiftModifier;
    /* held, the steps repeat; the others do not */
    if (e->isAutoRepeat() && (e->key() == Qt::Key_F5 || e->key() == Qt::Key_F9 || e->key() == Qt::Key_F12)) return;
    switch (e->key()) {
    case Qt::Key_F5: toggleRun(); break;
    case Qt::Key_F7: stepAnd([this] { doStep(); }); break;
    case Qt::Key_F8: stepAnd(shift ? std::function<void()>([this] { doStepOut(); })
                                   : std::function<void()>([this] { doStepOver(); })); break;
    case Qt::Key_F9: toggleSelected(); break;
    default: hide(); break;     /* F12 */
    }
}

/* Shown: the machine stops, as on every sibling.
 * The tabs have no size until the window is up, so the first refresh waits
 * for the event loop. A restore from the taskbar is not a show. */
void DebuggerWindow::showEvent(QShowEvent *e)
{
    QMainWindow::showEvent(e);
    m_timer.start(100);
    if (e->spontaneous()) return;
    for (QWidget *win : m_popped)
        if (win) win->show();
    m_resumeOnHide = !a2600debug_is_stopped(m_dbg);
    a2600debug_stop(m_dbg);
    static bool historyOnce = true;     /* A2600_DEBUGGER_HISTORY: record from the first show */
    if (historyOnce && qEnvironmentVariableIsSet("A2600_DEBUGGER_HISTORY")) a2600debug_trace_enable(m_dbg, 1);
    historyOnce = false;
    m_wasStopped = false;
    QTimer::singleShot(0, this, [this] {
        if (!isVisible()) return;
        /* shown before the session started (A2600_OPEN_DEBUGGER): stop it now it has */
        if (!a2600debug_is_stopped(m_dbg)) a2600debug_stop(m_dbg);
        if (m_tabs->currentIndex() == TAB_CONSOLE) m_conIn->setFocus();
        settleAndSync();
    });
}

/* Hidden by F12, the close button or the main window: the machine runs on if
 * it was running when the window came up. The close button is a plain close, which only
 * hides the window (it is not deleted, so it comes back as it was);
 * ignoring the close instead would make QApplication's quit, which closes
 * every window first, give up whenever it reached this one. A minimised
 * window's spontaneous hide is not a detach. */
void DebuggerWindow::hideEvent(QHideEvent *e)
{
    m_timer.stop();
    if (!e->spontaneous()) {
        for (QWidget *win : m_popped)
            if (win) win->hide();
        if (m_resumeOnHide) a2600debug_resume(m_dbg);
        m_resumeOnHide = false;
    }
    QMainWindow::hideEvent(e);
}
