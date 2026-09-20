/*
 * DebuggerWindow -- see DebuggerWindow.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "DebuggerWindow.h"

#include <QFileDialog>
#include <QFont>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QKeyEvent>
#include <QPixmap>
#include <QPointer>
#include <QScrollBar>
#include <QTabWidget>
#include <QTextCursor>
#include <QTextBlock>
#include <QVBoxLayout>
#include <QWheelEvent>

#include "../FujiNetWindows.h"

#define DISASM_WINDOW 48

namespace {

QPlainTextEdit *monoView(bool editable)
{
    auto *v = new QPlainTextEdit;
    v->setReadOnly(!editable);
    QFont f = v->font();
    f.setFamily(QStringLiteral("monospace"));
    f.setStyleHint(QFont::TypeWriter);
    v->setFont(f);
    v->setLineWrapMode(QPlainTextEdit::NoWrap);
    return v;
}

QString stripControl(const char *s)
{
    QString out;
    for (; *s; ++s)
        if ((unsigned char)*s >= 0x20 || *s == '\n' || *s == '\t') out += QChar(*s);
    return out;
}

bool parseNum(const QString &t, long *out)
{
    QString s = t.trimmed();
    bool ok = false;
    long v = 0;
    if (s.startsWith('$')) v = s.mid(1).toLong(&ok, 16);
    else if (s.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) v = s.mid(2).toLong(&ok, 16);
    else if (s.startsWith('#')) v = s.mid(1).toLong(&ok, 10);
    else v = s.toLong(&ok, 16);
    if (ok) *out = v;
    return ok;
}

} // namespace

void DebuggerWindow::showFor(QWidget *parent, a2600session *session)
{
    static QPointer<DebuggerWindow> win;
    if (!win) win = new DebuggerWindow(session, parent);
    win->show();
    win->raise();
    win->activateWindow();
    a2600debug_stop(win->m_dbg);
    win->refreshAll();
}

DebuggerWindow::DebuggerWindow(a2600session *session, QWidget *parent)
    : QMainWindow(parent, Qt::Window), m_session(session), m_dbg(a2600session_debugger(session))
{
    setWindowTitle(QStringLiteral("Debugger"));
    resize(1100, 760);
    m_tiaPx.resize(A2600SESSION_FB_WIDTH * A2600SESSION_FB_MAX_HEIGHT);

    auto *central = new QWidget;
    auto *root = new QVBoxLayout(central);
    root->addWidget(buildToolbar());
    auto *tabs = new QTabWidget;
    tabs->addTab(buildPrompt(), QStringLiteral("Prompt"));
    tabs->addTab(buildCpu(), QStringLiteral("CPU && RAM"));
    tabs->addTab(buildDisasm(), QStringLiteral("Disassembly"));
    tabs->addTab(buildTia(), QStringLiteral("TIA"));
    tabs->addTab(buildRiot(), QStringLiteral("I/O"));
    tabs->addTab(buildBreaks(), QStringLiteral("Breaks && Traps"));
    tabs->addTab(buildStates(), QStringLiteral("States && Cart"));
    if (qEnvironmentVariableIsSet("A2600_DEBUGGER_TAB"))
        tabs->setCurrentIndex(qEnvironmentVariableIntValue("A2600_DEBUGGER_TAB"));
    root->addWidget(tabs, 1);
    setCentralWidget(central);

    connect(&m_timer, &QTimer::timeout, this, &DebuggerWindow::tick);
    m_timer.start(100);
}

/* ---- building ------------------------------------------------------------- */

QWidget *DebuggerWindow::buildToolbar()
{
    auto *bar = new QWidget;
    auto *h = new QHBoxLayout(bar);
    h->setContentsMargins(0, 0, 0, 0);
    auto add = [&](const QString &label, auto fn) {
        auto *b = new QPushButton(label);
        b->setFocusPolicy(Qt::NoFocus);
        connect(b, &QPushButton::clicked, this, fn);
        h->addWidget(b);
        return b;
    };
    m_runBtn = add(QStringLiteral("Stop (F5)"), [this] {
        if (a2600debug_is_stopped(m_dbg)) a2600debug_resume(m_dbg); else a2600debug_stop(m_dbg);
        refreshAll();
    });
    add(QStringLiteral("Step (F7)"), [this] { a2600debug_step(m_dbg); refreshAll(); });
    add(QStringLiteral("Trace (F8)"), [this] { a2600debug_trace(m_dbg); refreshAll(); });
    add(QStringLiteral("Scan+1"), [this] { a2600debug_scanline(m_dbg, 1); refreshAll(); });
    add(QStringLiteral("Frame+1 (⇧F8)"), [this] { a2600debug_frame(m_dbg, 1); refreshAll(); });
    add(QStringLiteral("Rewind"), [this] { a2600debug_rewind(m_dbg, 1); refreshAll(); });
    add(QStringLiteral("Unwind"), [this] { a2600debug_unwind(m_dbg, 1); refreshAll(); });
    m_status = new QLabel;
    m_status->setStyleSheet(QStringLiteral("color: gray;"));
    m_status->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    h->addWidget(m_status, 1);
    return bar;
}

QWidget *DebuggerWindow::buildPrompt()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    m_promptOut = monoView(false);
    m_promptOut->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_promptOut->setPlainText(QStringLiteral("Stella debugger prompt. Type 'help' for every command.\n"));
    auto *row = new QHBoxLayout;
    m_promptIn = new QLineEdit;
    m_promptIn->setPlaceholderText(QStringLiteral("Stella debugger command (help, break, breakIf, trap, watch, frame, tia, ...) — Tab completes"));
    m_promptIn->installEventFilter(this);
    connect(m_promptIn, &QLineEdit::returnPressed, this, &DebuggerWindow::runPrompt);
    auto *sym = new QPushButton(QStringLiteral("Load symbols"));
    connect(sym, &QPushButton::clicked, this, [this] {
        char msg[512];
        a2600debug_load_symbols(m_dbg, msg, sizeof msg);
        appendPrompt(QString::fromUtf8(msg) + QLatin1Char('\n'));
        refreshAll();
    });
    row->addWidget(m_promptIn, 1);
    row->addWidget(sym);
    v->addWidget(m_promptOut, 1);
    v->addLayout(row);
    return w;
}

QWidget *DebuggerWindow::buildCpu()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    auto *regs = new QHBoxLayout;
    static const char *const names[6] = { "PC", "SP", "A", "X", "Y", "PS" };
    static const int regIds[6] = { A2600_REG_PC, A2600_REG_SP, A2600_REG_A, A2600_REG_X, A2600_REG_Y, A2600_REG_PS };
    for (int i = 0; i < 6; ++i) {
        regs->addWidget(new QLabel(QString::fromUtf8(names[i])));
        m_reg[i] = new QLineEdit;
        m_reg[i]->setMaxLength(4);
        m_reg[i]->setMaximumWidth(64);
        connect(m_reg[i], &QLineEdit::returnPressed, this, [this, i] {
            long val;
            if (parseNum(m_reg[i]->text(), &val)) a2600debug_cpu_set(m_dbg, regIds[i], (int)val);
            refreshAll();
        });
        regs->addWidget(m_reg[i]);
    }
    regs->addStretch();
    v->addLayout(regs);
    auto *flags = new QHBoxLayout;
    static const char *const fnames[7] = { "N", "V", "B", "D", "I", "Z", "C" };
    static const int flagIds[7] = { A2600_FLAG_N, A2600_FLAG_V, A2600_FLAG_B, A2600_FLAG_D, A2600_FLAG_I, A2600_FLAG_Z, A2600_FLAG_C };
    for (int i = 0; i < 7; ++i) {
        m_flag[i] = new QCheckBox(QString::fromUtf8(fnames[i]));
        connect(m_flag[i], &QCheckBox::clicked, this, [this, i](bool on) {
            if (a2600debug_is_stopped(m_dbg)) a2600debug_cpu_set(m_dbg, flagIds[i], on);
        });
        flags->addWidget(m_flag[i]);
    }
    m_cycles = new QLabel;
    m_cycles->setStyleSheet(QStringLiteral("color: gray;"));
    flags->addWidget(m_cycles, 1);
    v->addLayout(flags);
    v->addWidget(new QLabel(QStringLiteral("Zero-page RAM ($80-$FF)")));
    m_ram = monoView(false);
    v->addWidget(m_ram, 1);
    auto *edit = new QHBoxLayout;
    m_ramAddr = new QLineEdit; m_ramAddr->setPlaceholderText(QStringLiteral("$80")); m_ramAddr->setMaximumWidth(80);
    m_ramVal = new QLineEdit; m_ramVal->setPlaceholderText(QStringLiteral("$00")); m_ramVal->setMaximumWidth(60);
    connect(m_ramVal, &QLineEdit::returnPressed, this, [this] {
        long a, val;
        if (parseNum(m_ramAddr->text(), &a) && parseNum(m_ramVal->text(), &val))
            a2600debug_write(m_dbg, (uint16_t)a, (uint8_t)val);
        refreshAll();
    });
    edit->addWidget(new QLabel(QStringLiteral("Write address")));
    edit->addWidget(m_ramAddr);
    edit->addWidget(new QLabel(QStringLiteral("value")));
    edit->addWidget(m_ramVal);
    edit->addStretch();
    v->addLayout(edit);
    return w;
}

QWidget *DebuggerWindow::buildDisasm()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    auto *row = new QHBoxLayout;
    m_bank = new QComboBox;
    m_bank->addItem(QStringLiteral("PC's bank"));
    const int n = a2600debug_bank_count(m_dbg);
    for (int i = 0; i < n; ++i) m_bank->addItem(QStringLiteral("Bank %1").arg(i));
    connect(m_bank, &QComboBox::currentIndexChanged, this, [this](int sel) {
        m_disasmBank = sel - 1;
        if (sel > 0) m_followPc->setChecked(false);
        m_disasmFirst = 0;
        refreshDisasm();
    });
    m_followPc = new QCheckBox(QStringLiteral("Follow PC"));
    m_followPc->setChecked(true);
    m_jump = new QLineEdit;
    m_jump->setPlaceholderText(QStringLiteral("address or label"));
    m_jump->setMaximumWidth(160);
    connect(m_jump, &QLineEdit::returnPressed, this, [this] { jumpTo(m_jump->text()); });
    row->addWidget(m_bank);
    row->addWidget(m_followPc);
    row->addWidget(new QLabel(QStringLiteral("Jump to")));
    row->addWidget(m_jump);
    row->addWidget(new QLabel(QStringLiteral("Click a line to toggle its breakpoint; scroll to browse")));
    row->addStretch();
    v->addLayout(row);
    m_disasm = monoView(false);
    m_disasm->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_disasm->viewport()->installEventFilter(this);
    v->addWidget(m_disasm, 1);
    return w;
}

QWidget *DebuggerWindow::buildTia()
{
    auto *w = new QWidget;
    auto *h = new QHBoxLayout(w);
    auto *left = new QVBoxLayout;
    m_tiaText = monoView(false);
    m_tiaText->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    left->addWidget(m_tiaText, 1);
    auto *edit = new QHBoxLayout;
    m_tiaReg = new QLineEdit; m_tiaReg->setPlaceholderText(QStringLiteral("colup0")); m_tiaReg->setMaximumWidth(100);
    m_tiaVal = new QLineEdit; m_tiaVal->setPlaceholderText(QStringLiteral("$1E")); m_tiaVal->setMaximumWidth(70);
    auto apply = [this] {
        const QByteArray reg = m_tiaReg->text().trimmed().toLower().toUtf8();
        if (a2600debug_tia_strobe(m_dbg, reg.constData()) == 0) { refreshAll(); return; }
        long val;
        if (parseNum(m_tiaVal->text(), &val)) a2600debug_tia_set(m_dbg, reg.constData(), (int)val);
        refreshAll();
    };
    connect(m_tiaReg, &QLineEdit::returnPressed, this, apply);
    connect(m_tiaVal, &QLineEdit::returnPressed, this, apply);
    edit->addWidget(new QLabel(QStringLiteral("Register / strobe")));
    edit->addWidget(m_tiaReg);
    edit->addWidget(new QLabel(QStringLiteral("value")));
    edit->addWidget(m_tiaVal);
    edit->addStretch();
    left->addLayout(edit);
    h->addLayout(left, 1);
    auto *right = new QVBoxLayout;
    m_tiaPic = new QLabel;
    m_tiaPic->setMinimumSize(320, 240);
    m_tiaPic->setScaledContents(true);
    m_tiaPartial = new QCheckBox(QStringLiteral("Frame in progress (to the beam)"));
    right->addWidget(m_tiaPic, 1);
    right->addWidget(m_tiaPartial);
    h->addLayout(right);
    return w;
}

QWidget *DebuggerWindow::buildRiot()
{
    m_riot = monoView(false);
    m_riot->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    return m_riot;
}

QWidget *DebuggerWindow::buildBreaks()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    auto *row = new QHBoxLayout;
    m_bpEntry = new QLineEdit;
    m_bpEntry->setPlaceholderText(QStringLiteral("address/label to toggle, or breakIf {..}, trap $80, trapWrite $80 $ff, watch a ..."));
    connect(m_bpEntry, &QLineEdit::returnPressed, this, [this] {
        const QString text = m_bpEntry->text().trimmed();
        if (text.isEmpty()) return;
        long a;
        int addr = a2600debug_label_address(m_dbg, text.toUtf8().constData());
        if (addr < 0 && parseNum(text, &a) && QStringLiteral("$#0123456789abcdefABCDEF").contains(text.at(0))) addr = (int)a;
        if (addr >= 0) {
            a2600debug_breakpoint_toggle(m_dbg, (uint16_t)addr, A2600DEBUG_ANY_BANK);
        } else {
            char out[2048];
            a2600debug_command(m_dbg, text.toUtf8().constData(), out, sizeof out);
        }
        m_bpEntry->clear();
        refreshAll();
    });
    auto *clear = new QPushButton(QStringLiteral("Clear all"));
    connect(clear, &QPushButton::clicked, this, [this] {
        char out[512];
        a2600debug_breakpoint_clear(m_dbg);
        a2600debug_command(m_dbg, "clearTraps", out, sizeof out);
        refreshAll();
    });
    row->addWidget(m_bpEntry, 1);
    row->addWidget(clear);
    v->addLayout(row);
    m_bps = monoView(false);
    v->addWidget(m_bps, 1);
    return w;
}

QWidget *DebuggerWindow::buildStates()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    v->addWidget(new QLabel(QStringLiteral("Emulator states (Stella's saveState / loadState slots)")));
    auto *grid = new QGridLayout;
    for (int i = 0; i < 10; ++i) {
        auto *s = new QPushButton(QStringLiteral("Save %1").arg(i));
        auto *l = new QPushButton(QStringLiteral("Load %1").arg(i));
        connect(s, &QPushButton::clicked, this, [this, i] { a2600debug_state_save(m_dbg, i); refreshAll(); });
        connect(l, &QPushButton::clicked, this, [this, i] { a2600debug_state_load(m_dbg, i); refreshAll(); });
        grid->addWidget(s, (i / 5) * 2, i % 5);
        grid->addWidget(l, (i / 5) * 2 + 1, i % 5);
    }
    v->addLayout(grid);
    v->addWidget(new QLabel(QStringLiteral("Save to a file")));
    auto *files = new QHBoxLayout;
    static const struct { const char *kind, *title; } saves[] = {
        { "dis", "disassembly" }, { "rom", "ROM (patched)" }, { "access", "access counters" },
        { "ses", "session" }, { "snap", "TIA snapshot" } };
    for (const auto &s : saves) {
        auto *b = new QPushButton(QString::fromUtf8(s.title));
        const char *kind = s.kind;
        const char *title = s.title;
        connect(b, &QPushButton::clicked, this, [this, kind, title] {
            const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save %1").arg(QString::fromUtf8(title)));
            if (path.isEmpty()) return;
            char msg[512];
            a2600debug_save(m_dbg, kind, path.toLocal8Bit().constData(), msg, sizeof msg);
            appendPrompt(stripControl(msg) + QLatin1Char('\n'));
        });
        files->addWidget(b);
    }
    files->addStretch();
    v->addLayout(files);
    v->addWidget(new QLabel(QStringLiteral("Cartridge")));
    m_cartInfo = new QLabel;
    m_cartInfo->setWordWrap(true);
    m_cartInfo->setStyleSheet(QStringLiteral("color: gray;"));
    v->addWidget(m_cartInfo);
    v->addStretch();
    return w;
}

/* ---- refresh -------------------------------------------------------------- */

void DebuggerWindow::refreshStatus()
{
    char reason[160], info[256];
    int addr;
    const bool stopped = a2600debug_is_stopped(m_dbg) != 0;
    a2600debug_stop_reason(m_dbg, reason, sizeof reason, &addr);
    a2600debug_cart_info(m_dbg, info, sizeof info);
    m_status->setText(stopped ? QStringLiteral("Stopped%1%2").arg(reason[0] ? ": " : "", QString::fromUtf8(reason))
                              : QStringLiteral("Running"));
    m_cartInfo->setText(QString::fromUtf8(info));
    m_runBtn->setText(stopped ? QStringLiteral("Run (F5)") : QStringLiteral("Stop (F5)"));
    m_runBtn->setStyleSheet(stopped ? QStringLiteral("background: %1; color: black;").arg(a2600AccentColor().name()) : QString());
}

void DebuggerWindow::refreshCpu()
{
    a2600debug_cpu c;
    a2600debug_cpu_get(m_dbg, &c);
    const int vals[6] = { c.pc, c.sp, c.a, c.x, c.y, c.ps };
    for (int i = 0; i < 6; ++i)
        if (!m_reg[i]->hasFocus())
            m_reg[i]->setText(QStringLiteral("%1").arg(vals[i], i == 0 ? 4 : 2, 16, QLatin1Char('0')).toUpper());
    const int flags[7] = { c.n, c.v, c.b, c.d, c.i, c.z, c.c };
    for (int i = 0; i < 7; ++i) m_flag[i]->setChecked(flags[i] != 0);
    m_cycles->setText(QStringLiteral("last instruction: %1 cycles, total %2").arg(c.cycles).arg(c.total_cycles));
}

void DebuggerWindow::refreshRam()
{
    uint8_t ram[128];
    a2600debug_ram_get(m_dbg, ram);
    QString text = QStringLiteral("      0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F\n");
    for (int row = 0; row < 8; ++row) {
        text += QStringLiteral("$%1: ").arg(0x80 + row * 16, 2, 16, QLatin1Char('0')).toUpper();
        for (int col = 0; col < 16; ++col)
            text += QStringLiteral("%1 ").arg(ram[row * 16 + col], 2, 16, QLatin1Char('0')).toUpper();
        text += QLatin1Char('\n');
    }
    m_ram->setPlainText(text);
}

void DebuggerWindow::refreshDisasm()
{
    static a2600debug_line lines[DISASM_WINDOW];
    int total = 0, pcLine = -1;
    a2600debug_disassemble(m_dbg, m_disasmBank, 0, lines, 1, &total, &pcLine);
    if (m_followPc->isChecked() && pcLine >= 0)
        m_disasmFirst = pcLine > DISASM_WINDOW / 3 ? pcLine - DISASM_WINDOW / 3 : 0;
    if (m_disasmFirst > total - 1) m_disasmFirst = total > 0 ? total - 1 : 0;
    if (m_disasmFirst < 0) m_disasmFirst = 0;
    const int n = a2600debug_disassemble(m_dbg, m_disasmBank, m_disasmFirst, lines, DISASM_WINDOW, &total, &pcLine);
    m_lineAddr.clear();
    QString text;
    int pcRow = -1;
    for (int i = 0; i < n; ++i) {
        m_lineAddr.push_back(lines[i].address);
        if (lines[i].is_pc) pcRow = i;
        text += QStringLiteral("%1%2 %3  %4 %5 %6 %7\n")
            .arg(lines[i].has_breakpoint ? '*' : ' ').arg(lines[i].is_pc ? '>' : ' ')
            .arg(lines[i].address, 4, 16, QLatin1Char('0')).toUpper()
            .arg(QString::fromUtf8(lines[i].bytes), -10)
            .arg(QString::fromUtf8(lines[i].label), -14)
            .arg(QString::fromUtf8(lines[i].disasm), -22)
            .arg(QString::fromUtf8(lines[i].cycles));
    }
    if (n == 0) text = QStringLiteral("(no disassembly)\n");
    m_disasm->setPlainText(text);
    if (pcRow >= 0) {
        QTextCursor cur(m_disasm->document()->findBlockByNumber(pcRow));
        cur.select(QTextCursor::LineUnderCursor);
        QTextCharFormat fmt;
        fmt.setBackground(a2600AccentColor());
        fmt.setForeground(Qt::black);
        cur.setCharFormat(fmt);
    }
}

void DebuggerWindow::refreshTia()
{
    a2600debug_tia t;
    a2600debug_tia_get(m_dbg, &t);
    static const char *const coll[15] = { "M0-P1", "M0-P0", "M1-P0", "M1-P1", "P0-PF", "P0-BL", "P1-PF",
        "P1-BL", "M0-PF", "M0-BL", "M1-PF", "M1-BL", "BL-PF", "P0-P1", "M0-M1" };
    QString s;
    s += QStringLiteral("Frame %1   scanline %2 (last frame %3)   frame cycles %4 (WSYNC %5)\n")
        .arg(t.frame_count).arg(t.scanlines).arg(t.scanlines_last).arg(t.frame_cycles).arg(t.wsync_cycles);
    s += QStringLiteral("clocks this line %1   cycles this line %2   beam %3,%4\nVSYNC %5   VBLANK %6\n\n")
        .arg(t.clocks_this_line).arg(t.cycles_this_line).arg(t.beam_x).arg(t.beam_y).arg(t.vsync).arg(t.vblank);
    auto hx = [](int v) { return QStringLiteral("%1").arg(v, 2, 16, QLatin1Char('0')).toUpper(); };
    s += QStringLiteral("Colours   COLUP0 %1   COLUP1 %2   COLUPF %3   COLUBK %4\n").arg(hx(t.colup0), hx(t.colup1), hx(t.colupf), hx(t.colubk));
    s += QStringLiteral("Players   GRP0 %1  GRP1 %2   NUSIZ0 %3  NUSIZ1 %4   REFP0 %5  REFP1 %6   VDELP0 %7  VDELP1 %8\n")
        .arg(hx(t.grp0), hx(t.grp1), hx(t.nusiz0), hx(t.nusiz1)).arg(t.refp0).arg(t.refp1).arg(t.vdelp0).arg(t.vdelp1);
    s += QStringLiteral("Missiles  ENAM0 %1  ENAM1 %2   RESMP0 %3  RESMP1 %4\nBall      ENABL %5   VDELBL %6\n")
        .arg(t.enam0).arg(t.enam1).arg(t.resmp0).arg(t.resmp1).arg(t.enabl).arg(t.vdelbl);
    s += QStringLiteral("Playfield PF0 %1  PF1 %2  PF2 %3   CTRLPF %4   REF %5  SCORE %6  PRIORITY %7\n\n")
        .arg(hx(t.pf0), hx(t.pf1), hx(t.pf2), hx(t.ctrlpf)).arg(t.refpf).arg(t.scorepf).arg(t.pripf);
    s += QStringLiteral("Positions      P0 %1   P1 %2   M0 %3   M1 %4   BL %5\n").arg(t.pos_p0, 3).arg(t.pos_p1, 3).arg(t.pos_m0, 3).arg(t.pos_m1, 3).arg(t.pos_bl, 3);
    s += QStringLiteral("Motion (HM)    P0 %1    P1 %2    M0 %3    M1 %4    BL %5\n\n").arg(hx(t.hm_p0), hx(t.hm_p1), hx(t.hm_m0), hx(t.hm_m1), hx(t.hm_bl));
    s += QStringLiteral("Audio     AUDC0 %1  AUDF0 %2  AUDV0 %3  (%4)\n          AUDC1 %5  AUDF1 %6  AUDV1 %7  (%8)\n\nCollisions:")
        .arg(hx(t.audc0), hx(t.audf0), hx(t.audv0), QString::fromUtf8(t.aud_freq0), hx(t.audc1), hx(t.audf1), hx(t.audv1), QString::fromUtf8(t.aud_freq1));
    bool any = false;
    for (int i = 0; i < 15; ++i) if (t.collisions & (1u << i)) { s += QLatin1Char(' ') + QString::fromUtf8(coll[i]); any = true; }
    if (!any) s += QStringLiteral(" none");
    s += QStringLiteral("\n\nSet a register: name and value below (colup0, pf1, posp0, refp0, vsync ...); "
                        "strobes: wsync rsync resp0 resp1 resm0 resm1 resbl hmove hmclr cxclr\n");
    m_tiaText->setPlainText(s);

    const int h = a2600debug_frame_snapshot(m_dbg, m_tiaPx.data(), m_tiaPartial->isChecked());
    if (h > 0) {
        QImage img(reinterpret_cast<const uchar *>(m_tiaPx.data()), A2600SESSION_FB_WIDTH, h,
                   A2600SESSION_FB_WIDTH * 4, QImage::Format_RGB32);
        m_tiaPic->setPixmap(QPixmap::fromImage(img.copy()));
    }
}

void DebuggerWindow::refreshRiot()
{
    a2600debug_riot r;
    a2600debug_riot_get(m_dbg, &r);
    auto hx = [](int v) { return QStringLiteral("%1").arg(v, 2, 16, QLatin1Char('0')).toUpper(); };
    QString s;
    s += QStringLiteral("SWCHA  %1   SWACNT %2     left: %3   right: %4\nSWCHB  %5   SWBCNT %6\n")
        .arg(hx(r.swcha), hx(r.swacnt), QString::fromUtf8(r.dir_left), QString::fromUtf8(r.dir_right), hx(r.swchb), hx(r.swbcnt));
    s += QStringLiteral("INPT0-5  ");
    for (int i = 0; i < 6; ++i) s += hx(r.inpt[i]) + QLatin1Char(' ');
    s += QStringLiteral("\n\nTimer  INTIM %1   TIMINT %2   clocks %3   divider %4\n\n").arg(hx(r.intim), hx(r.timint)).arg(r.tim_clocks).arg(r.tim_divider);
    s += QStringLiteral("Switches   Select %1   Reset %2   %3   Left difficulty %4   Right difficulty %5\n\n")
        .arg(r.select ? "pressed" : "up", r.reset ? "pressed" : "up", r.color ? "Color" : "B&W",
             r.diff_left_a ? "A" : "B", r.diff_right_a ? "A" : "B");
    s += QStringLiteral("The switches can be flipped from the menu, the keypad window or the prompt (swchb $xx); "
                        "the ports with joy0up, joy0fire ...");
    m_riot->setPlainText(s);
}

void DebuggerWindow::refreshBps()
{
    uint32_t bps[64];
    const int n = a2600debug_breakpoint_list(m_dbg, bps, 64);
    QString s = QStringLiteral("Breakpoints (%1):\n").arg(n);
    for (int i = 0; i < n; ++i) {
        const unsigned bank = bps[i] >> 16;
        s += QStringLiteral("  $%1  %2\n").arg(bps[i] & 0xffff, 4, 16, QLatin1Char('0')).toUpper()
                 .arg(bank == A2600DEBUG_ANY_BANK ? QStringLiteral("any bank") : QStringLiteral("bank %1").arg(bank));
    }
    char out[8192];
    a2600debug_command(m_dbg, "listTraps", out, sizeof out);
    s += QStringLiteral("\nTraps:\n") + stripControl(out) + QLatin1Char('\n');
    a2600debug_command(m_dbg, "listBreaks", out, sizeof out);
    s += QStringLiteral("\nConditional (breakIf):\n") + stripControl(out) + QLatin1Char('\n');
    m_bps->setPlainText(s);
}

void DebuggerWindow::refreshAll()
{
    refreshStatus(); refreshCpu(); refreshRam(); refreshDisasm(); refreshTia(); refreshRiot(); refreshBps();
}

void DebuggerWindow::tick()
{
    if (!isVisible()) return;
    const unsigned gen = a2600debug_generation(m_dbg);
    const bool stopped = a2600debug_is_stopped(m_dbg) != 0;
    if (gen != m_seenGen || stopped != m_wasStopped) {
        m_seenGen = gen; m_wasStopped = stopped;
        refreshAll();
    } else if (!stopped && ++m_runningTicks >= 5) {
        m_runningTicks = 0;
        refreshStatus(); refreshCpu(); refreshTia(); refreshRiot();
    }
}

/* ---- the prompt and the rest ----------------------------------------------- */

void DebuggerWindow::appendPrompt(const QString &text)
{
    m_promptOut->moveCursor(QTextCursor::End);
    m_promptOut->insertPlainText(text);
    m_promptOut->verticalScrollBar()->setValue(m_promptOut->verticalScrollBar()->maximum());
}

void DebuggerWindow::runPrompt()
{
    static char out[65536];
    const QString cmd = m_promptIn->text().trimmed();
    if (cmd.isEmpty()) return;
    appendPrompt(QStringLiteral("> %1\n").arg(cmd));
    a2600debug_command(m_dbg, cmd.toUtf8().constData(), out, sizeof out);
    appendPrompt(stripControl(out) + QLatin1Char('\n'));
    m_promptIn->clear();
    refreshAll();
}

void DebuggerWindow::jumpTo(const QString &text)
{
    long a;
    int addr = a2600debug_label_address(m_dbg, text.toUtf8().constData());
    if (addr < 0 && parseNum(text, &a)) addr = (int)a;
    if (addr < 0) return;
    static a2600debug_line lines[256];
    int total = 0, pcLine = -1, first = 0;
    m_followPc->setChecked(false);
    for (;;) {
        const int n = a2600debug_disassemble(m_dbg, m_disasmBank, first, lines, 256, &total, &pcLine);
        if (n == 0) break;
        for (int i = 0; i < n; ++i)
            if (lines[i].address >= addr) { m_disasmFirst = first + i; refreshDisasm(); return; }
        first += n;
        if (first >= total) break;
    }
}

bool DebuggerWindow::eventFilter(QObject *obj, QEvent *e)
{
    if (obj == m_promptIn && e->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(e);
        if (ke->key() == Qt::Key_Tab) {
            const QString text = m_promptIn->text();
            const int sp = text.lastIndexOf(QLatin1Char(' '));
            const QString word = sp >= 0 ? text.mid(sp + 1) : text;
            char comps[4096];
            const int n = a2600debug_completions(m_dbg, word.toUtf8().constData(), comps, sizeof comps);
            if (n == 1) {
                QString c = QString::fromUtf8(comps).section(QLatin1Char('\n'), 0, 0);
                m_promptIn->setText(text.left(sp + 1) + c + QLatin1Char(' '));
            } else if (n > 1) {
                appendPrompt(QString::fromUtf8(comps));
            }
            return true;
        }
    }
    if (m_disasm && obj == m_disasm->viewport()) {
        if (e->type() == QEvent::MouseButtonPress) {
            auto *me = static_cast<QMouseEvent *>(e);
            const int line = m_disasm->cursorForPosition(me->pos()).blockNumber();
            if (line >= 0 && line < (int)m_lineAddr.size()) {
                const int bank = m_disasmBank < 0 ? a2600debug_current_bank(m_dbg) : m_disasmBank;
                a2600debug_breakpoint_toggle(m_dbg, m_lineAddr[line], bank);
                refreshDisasm();
                refreshBps();
            }
            return true;
        }
        if (e->type() == QEvent::Wheel) {
            auto *we = static_cast<QWheelEvent *>(e);
            m_followPc->setChecked(false);
            m_disasmFirst -= we->angleDelta().y() / 40;
            refreshDisasm();
            return true;
        }
    }
    return QMainWindow::eventFilter(obj, e);
}

void DebuggerWindow::keyPressEvent(QKeyEvent *e)
{
    switch (e->key()) {
    case Qt::Key_F5:
        if (a2600debug_is_stopped(m_dbg)) a2600debug_resume(m_dbg); else a2600debug_stop(m_dbg);
        refreshAll(); return;
    case Qt::Key_F7: a2600debug_step(m_dbg); refreshAll(); return;
    case Qt::Key_F8:
        if (e->modifiers() & Qt::ShiftModifier) a2600debug_frame(m_dbg, 1); else a2600debug_trace(m_dbg);
        refreshAll(); return;
    case Qt::Key_F12: hide(); return;
    default: QMainWindow::keyPressEvent(e);
    }
}

void DebuggerWindow::closeEvent(QCloseEvent *e)
{
    hide();
    e->ignore();
}
