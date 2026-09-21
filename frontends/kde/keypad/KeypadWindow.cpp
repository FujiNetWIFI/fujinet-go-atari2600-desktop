/*
 * KeypadWindow -- see KeypadWindow.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KeypadWindow.h"

#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QVBoxLayout>

#include "../FujiNetWindows.h"
#include "../KeyForward.h"

/* ---- PadButton ------------------------------------------------------------ */

PadButton::PadButton(const QString &face, int target, QWidget *parent)
    : QPushButton(face, parent), m_target(target), m_face(face)
{
    setFocusPolicy(Qt::NoFocus);
}

void PadButton::setHeld(bool held)
{
    setStyleSheet(held ? QStringLiteral("background: %1; color: black;").arg(a2600AccentColor().name())
                       : QString());
}

void PadButton::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton && !m_down) {
        m_down = true;
        emit pressedTarget(m_target);
    }
    QPushButton::mousePressEvent(e);
}

void PadButton::mouseReleaseEvent(QMouseEvent *e)
{
    if (m_down) {
        m_down = false;
        emit releasedTarget(m_target);
    }
    QPushButton::mouseReleaseEvent(e);
}

/* Dragging off a button must release it. */
void PadButton::leaveEvent(QEvent *e)
{
    if (m_down) {
        m_down = false;
        emit releasedTarget(m_target);
    }
    QPushButton::leaveEvent(e);
}

/* ---- KeypadWindow ---------------------------------------------------------- */

/* Key and wide-button sizes, in pixels: fixed, so the panel is the same
 * shape everywhere (the numeric keys are what the user is looking at). */
static constexpr int kWideWidth = 130;

KeypadWindow::KeypadWindow(a2600session *session, QWidget *parent)
    /* A dialog-type window, so a tiling compositor floats it: it is a
     * panel of fixed-size buttons, and a keypad stretched across half a
     * monitor is not a keypad any more. */
    : QWidget(parent, Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint
                      | Qt::CustomizeWindowHint), m_session(session)
{
    setWindowTitle(QStringLiteral("Keypads"));
    setFocusPolicy(Qt::StrongFocus);

    auto *root = new QVBoxLayout(this);
    auto *ports = new QHBoxLayout;
    ports->addWidget(buildController(0));
    ports->addWidget(buildController(1));
    root->addLayout(ports);

    /* The console switches: one row of six at the standard width when the
     * font lets their labels fit it, otherwise two rows of three at a
     * width the labels need. Either way nothing is elided and the window
     * stays the width of the two controller boxes. */
    auto *console = new QGroupBox(QStringLiteral("Console"));
    auto *cgrid = new QGridLayout(console);
    cgrid->setSpacing(6);
    const int consoleWidth = qMax(kWideWidth, console->fontMetrics().horizontalAdvance(QStringLiteral("Reboot to CONFIG")) + 24);
    const int perRow = consoleWidth > kWideWidth ? 3 : 6;
    static const struct { const char *face; int target; } switches[6] = {
        { "Select", A2600_TARGET_SWITCH(A2600_SW_SELECT) },
        { "Reset", A2600_TARGET_SWITCH(A2600_SW_RESET) },
        { "Color / B&&W", A2600_TARGET_SWITCH(A2600_SW_COLOR_BW) },
        { "Left Diff", A2600_TARGET_SWITCH(A2600_SW_LEFT_DIFF) },
        { "Right Diff", A2600_TARGET_SWITCH(A2600_SW_RIGHT_DIFF) },
        { "Reboot to CONFIG", A2600_TARGET_SYSACT(A2600_SYSACT_REBOOT_CONFIG) },
    };
    cgrid->setColumnStretch(0, 1);
    cgrid->setColumnStretch(perRow + 1, 1);
    for (int i = 0; i < 6; ++i) {
        PadButton *b = control(QString::fromUtf8(switches[i].face), switches[i].target, true);
        b->setFixedWidth(consoleWidth);
        cgrid->addWidget(b, i / perRow, 1 + i % perRow, Qt::AlignCenter);
    }
    root->addWidget(console);

    auto *maprow = new QHBoxLayout;
    m_mapButton = new QPushButton(QStringLiteral("Map"));
    m_mapButton->setFocusPolicy(Qt::NoFocus);
    connect(m_mapButton, &QPushButton::clicked, this, [this] { setMapState(m_mapState == -2 ? -1 : -2); });
    auto *defaults = new QPushButton(QStringLiteral("Defaults"));
    defaults->setFocusPolicy(Qt::NoFocus);
    connect(defaults, &QPushButton::clicked, this, [this] {
        a2600session_bindings_reset(m_session);
        refreshLabels();
    });
    m_hint = new QLabel;
    m_hint->setStyleSheet(QStringLiteral("color: gray;"));
    maprow->addWidget(m_mapButton);
    maprow->addWidget(defaults);
    maprow->addWidget(m_hint, 1);
    root->addLayout(maprow);

    connect(&m_captureTimer, &QTimer::timeout, this, &KeypadWindow::pollCapture);
    connect(&m_typeTimer, &QTimer::timeout, this, &KeypadWindow::refreshTypes);
    setMapState(-2);

    /* Fixed size: min == max is also what tells a Wayland compositor the
     * window is not to be tiled or resized. Sized for the Map-mode labels,
     * which are the widest the buttons ever get. */
    root->setSizeConstraint(QLayout::SetFixedSize);
}

PadButton *KeypadWindow::control(const QString &face, int target, bool wide)
{
    auto *b = new PadButton(face, target);
    b->setFixedSize(wide ? kWideWidth : 72, 44);
    connect(b, &PadButton::pressedTarget, this, &KeypadWindow::onPressed);
    connect(b, &PadButton::releasedTarget, this, &KeypadWindow::onReleased);
    m_controls.push_back(b);
    return b;
}

QWidget *KeypadWindow::buildController(int port)
{
    auto *box = new QGroupBox(port ? QStringLiteral("Right Port") : QStringLiteral("Left Port"));
    auto *v = new QVBoxLayout(box);
    m_typeLabel[port] = new QLabel;
    m_typeLabel[port]->setAlignment(Qt::AlignHCenter);
    v->addWidget(m_typeLabel[port]);

    /* The 3x4 pad is a tight block of fixed-size keys, centred: the grid
     * must not spread to the width of the fire-button row beneath it, or the
     * digits drift apart. Centring is done by flanking stretch, and the
     * grid's own spacing is the only gap between keys. */
    static const char *const faces[12] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "*", "0", "#" };
    auto *grid = new QGridLayout;
    grid->setSpacing(6);
    grid->setSizeConstraint(QLayout::SetFixedSize);
    for (int i = 0; i < 12; ++i)
        grid->addWidget(control(QString::fromUtf8(faces[i]), A2600_TARGET_PORT(port, A2600_ACT_KEY_1 + i), false),
                        i / 3, i % 3, Qt::AlignCenter);
    auto *gridRow = new QHBoxLayout;
    gridRow->addStretch();
    gridRow->addLayout(grid);
    gridRow->addStretch();
    v->addLayout(gridRow);

    auto *fires = new QHBoxLayout;
    fires->setSpacing(6);
    fires->addStretch();
    fires->addWidget(control(QStringLiteral("Fire"), A2600_TARGET_PORT(port, A2600_ACT_JOY_FIRE), true));
    fires->addWidget(control(QStringLiteral("Paddle A"), A2600_TARGET_PORT(port, A2600_ACT_PADDLE_A_FIRE), true));
    fires->addWidget(control(QStringLiteral("Paddle B"), A2600_TARGET_PORT(port, A2600_ACT_PADDLE_B_FIRE), true));
    fires->addStretch();
    v->addLayout(fires);
    return box;
}

void KeypadWindow::onPressed(int target)
{
    if (m_mapState == -1) { setMapState(target); return; }
    if (m_mapState >= 0) return;
    for (PadButton *b : m_controls) if (b->target() == target) b->setHeld(true);
    if (target >= A2600_TARGET_SYSACT(0)) return;   /* fires on release */
    a2600session_press(m_session, target, 1);
}

void KeypadWindow::onReleased(int target)
{
    if (m_mapState != -2) return;
    for (PadButton *b : m_controls) if (b->target() == target) b->setHeld(false);
    if (target >= A2600_TARGET_SYSACT(0)) {
        a2600session_sysaction(m_session, target - A2600_TARGET_SYSACT(0));
        return;
    }
    a2600session_press(m_session, target, 0);
}

void KeypadWindow::setMapState(int state)
{
    m_mapState = state;
    if (state == -2) {
        m_captureTimer.stop();
        a2600session_gamepad_capture_cancel(m_session);
        m_mapButton->setText(QStringLiteral("Map"));
        m_mapButton->setStyleSheet(QString());
        m_hint->setText(QString());
    } else if (state == -1) {
        m_captureTimer.stop();
        a2600session_gamepad_capture_cancel(m_session);
        m_mapButton->setText(QStringLiteral("Cancel"));
        m_mapButton->setStyleSheet(QStringLiteral("background: %1; color: black;").arg(a2600AccentColor().name()));
        m_hint->setText(QStringLiteral("Click a control to remap"));
    } else {
        m_hint->setText(QStringLiteral("Press a key or gamepad button for %1")
                            .arg(QString::fromUtf8(a2600_target_name(state))));
        a2600session_gamepad_capture_begin(m_session);
        m_captureTimer.start(50);
    }
    refreshLabels();
}

void KeypadWindow::pollCapture()
{
    int button;
    if (m_mapState < 0) { m_captureTimer.stop(); return; }
    if (a2600session_gamepad_capture_poll(m_session, &button)) {
        char stolen[128];
        a2600session_binding_set_button(m_session, m_mapState, button, stolen, sizeof stolen);
        const QString msg = stolen[0]
            ? QStringLiteral("Bound %1 (was %2)").arg(QString::fromUtf8(a2600_pad_button_name(button)), QString::fromUtf8(stolen))
            : QString();
        setMapState(-1);
        if (!msg.isEmpty()) m_hint->setText(msg);
    }
}

void KeypadWindow::refreshLabels()
{
    for (PadButton *b : m_controls) {
        if (m_mapState != -2) {
            const a2600_binding bind = a2600session_binding_get(m_session, b->target());
            char key[32];
            a2600session_keysym_name(bind.keysym, key, sizeof key);
            QString text = key[0] ? QString::fromUtf8(key) : QStringLiteral("—");
            if (bind.button != A2600_PAD_BTN_NONE)
                text += QStringLiteral(" / ") + QString::fromUtf8(a2600_pad_button_name(bind.button));
            b->setText(text);
            b->setHeld(b->target() == m_mapState);
        } else {
            b->setText(b->face());
            b->setHeld(false);
        }
    }
}

void KeypadWindow::refreshTypes()
{
    for (int port = 0; port < 2; ++port) {
        const int det = a2600session_detected_port_type(m_session, port);
        m_typeLabel[port]->setText(QStringLiteral("%1 attached").arg(QString::fromUtf8(a2600_ctrl_type_name(det))));
        m_typeLabel[port]->setStyleSheet(det == A2600_CTRL_KEYPAD
            ? QStringLiteral("color: %1; font-weight: bold;").arg(a2600AccentColor().name())
            : QStringLiteral("color: gray;"));
    }
}

void KeypadWindow::showEvent(QShowEvent *e)
{
    refreshTypes();
    m_typeTimer.start(1000);
    QWidget::showEvent(e);
}

void KeypadWindow::hideEvent(QHideEvent *e)
{
    m_typeTimer.stop();
    setMapState(-2);
    QWidget::hideEvent(e);
}

void KeypadWindow::keyPressEvent(QKeyEvent *e)
{
    if (e->isAutoRepeat()) return;
    const uint32_t ks = a2600KeysymFromQt(e);
    if (m_mapState >= 0) {
        if (!ks) return;
        char stolen[128], name[32];
        a2600session_binding_set_key(m_session, m_mapState, ks, stolen, sizeof stolen);
        a2600session_keysym_name(ks, name, sizeof name);
        setMapState(-1);
        if (stolen[0])
            m_hint->setText(QStringLiteral("Bound %1 (was %2)").arg(QString::fromUtf8(name), QString::fromUtf8(stolen)));
        return;
    }
    if (m_mapState == -1) return;
    if (e->key() == Qt::Key_F9) { hide(); return; }
    if (!ks) { QWidget::keyPressEvent(e); return; }
    const int sa = a2600session_key_sysaction(m_session, ks);
    if (sa >= 0) { a2600session_sysaction(m_session, sa); return; }
    if (!a2600session_key(m_session, ks, 1)) QWidget::keyPressEvent(e);
}

void KeypadWindow::keyReleaseEvent(QKeyEvent *e)
{
    if (e->isAutoRepeat() || m_mapState != -2) return;
    const uint32_t ks = a2600KeysymFromQt(e);
    if (!ks || !a2600session_key(m_session, ks, 0)) QWidget::keyReleaseEvent(e);
}
