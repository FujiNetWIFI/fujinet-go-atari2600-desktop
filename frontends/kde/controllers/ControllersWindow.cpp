/*
 * ControllersWindow -- see ControllersWindow.h.
 *
 * What plugs into the Atari 2600's two ports, as a player sees it, drawn from
 * the session's layout (a2600session_controls_layout, in layout units) so every
 * frontend shows the same parts in the same places; the colours are this file's,
 * read off photographs of the real hardware (DESIGN-atari.md).
 *
 *   CX40 joystick   a black case with a ring of ticks, the ribbed boot and an
 *                   octagonal shaft that tilts with the pointer (drag the stick;
 *                   the four directions follow, in eight 45-degree sectors) and
 *                   the one red fire button in its recess.
 *   CX30 paddles    a pair of black wedges, each with a knurled knob you drag
 *                   round (it holds where you leave it), the wheel nudges, a red
 *                   button on the left edge and two turn arrows.
 *   CX20 driving    the same body; the wheel turns endlessly (a drag emits one
 *                   twist per 22.5 degrees, the wheel notches it).
 *   CX50 keypad     twelve round keys under "Keyboard Controller".
 *   Console         the six-switch panel: power, tv type, left and right
 *                   difficulty (latched: drawn where they are) and the momentary
 *                   game select and game reset, over the woodgrain strip. Power
 *                   is a power-cycle (the session's Reboot to CONFIG); STOP on
 *                   the wood stops in the Debugger.
 *
 * What the console sees held -- host keyboard, gamepads and this window -- is lit
 * in the held orange, and the knobs show where the paddles really stand, so the
 * window doubles as an input tester for a new pad.
 *
 * Gestures act on mouse press AND release: a press present only for the instant of
 * a click would fall between the machine's polls. A button is held while the mouse
 * is down on it (dragging it off lets it go); the stick follows the pointer; the
 * knob turns to the point; one contact at a time. Hiding the window and losing the
 * focus let go of everything the mouse holds.
 *
 * FIXED SIZE: the drawing is a fixed number of pixels per layout unit, Map
 * legends are fitted into their parts and gamepad names are elided, so neither
 * Map mode nor a long name resizes the window. A singleton, hidden rather than
 * destroyed, so a half-done remap survives closing it. The timers run only while
 * it is visible.
 *
 * MAP MODE: Map arms a two-step rebind -- click a control (the stick: a side of
 * its ring; a knob or the wheel: its left or right half) or pick one from the
 * list (all 59 are there), then press a host key or a gamepad button. Rebinding
 * steals the key or button from whatever held it (named in the hint). Escape
 * backs out one step. F9, F11, F12 and Ctrl chords are never bound.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ControllersWindow.h"

#include <algorithm>
#include <cmath>

#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPolygonF>
#include <QPushButton>
#include <QRadialGradient>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtMath>

#include <functional>
#include <utility>
#include <vector>

#include "../Accent.h"
#include "../KeyForward.h"
#include "../debugger/DebuggerWindow.h"
#include "../help/HelpWindow.h"

namespace {

using a2600ctl::kMargin;
using a2600ctl::kStageH;
using a2600ctl::kStageW;
using a2600ctl::kUnit;

constexpr int kMaxPadRows = 3;
constexpr qreal kDimAlpha = 0.4;        /* an unpicked part while Map waits for a key */
constexpr int kKnobNudge = 2048;        /* one mouse-wheel notch, of -32767..32767 */

/* ---- colours (photo-read or inferred, see DESIGN-atari.md) ------------------------ */

const QColor kPlastic(0x1C, 0x1C, 0x1C);
const QColor kPlasticHi(0x2A, 0x2A, 0x2A);
const QColor kPlasticDeep(0x10, 0x10, 0x10);
const QColor kFire(0xE2, 0x3B, 0x1F);
const QColor kFireHi(0xF0, 0x5A, 0x38);
const QColor kFireDark(0xB0, 0x2A, 0x14);
const QColor kPaddleBody(0x1F, 0x1F, 0x1F);
const QColor kPaddleHi(0x2E, 0x2E, 0x31);
const QColor kPaddleButton(0xD9, 0x3A, 0x22);
const QColor kPaddleButtonHi(0xE8, 0x49, 0x2C);
const QColor kSilver(0xC9, 0xC9, 0xC9);
const QColor kSilverShade(0x8E, 0x8E, 0x8E);
const QColor kOrange(0xE5, 0x8A, 0x1C);
const QColor kLabelOrange(0xE0, 0x8A, 0x1F);
const QColor kPowerRed(0xC8, 0x34, 0x1E);
const QColor kLegendBlue(0x8C, 0xCF, 0xEA);
const QColor kKeyBlack(0x08, 0x08, 0x08);
const QColor kKeyHi(0x2C, 0x2C, 0x2C);
const QColor kPanel(0x15, 0x15, 0x15);
const QColor kTitle(0xE6, 0xDF, 0xCF);
const QColor kWoodA(0x8A, 0x5A, 0x2B);
const QColor kWoodB(0x6E, 0x45, 0x21);
const QColor kGrain(0x4A, 0x2F, 0x16);
const QColor kMatTop(0x5A, 0x5B, 0x61);
const QColor kMatBottom(0x3E, 0x3F, 0x44);
const QColor kMatEdge(0x1A, 0x1A, 0x1D);
const QColor kMatInk(0xD9, 0xD6, 0xCB);

QString local(const char *text) { return QString::fromUtf8(text ? text : ""); }

/* ---- small drawing helpers ------------------------------------------------------------ */

/* A device's pixel origin and the pixels per layout unit: a part's art is written
 * in the design's own local units and mapped here. */
struct Dev {
    QPointF o;
    double u = kUnit;
    QPointF p(double x, double y) const { return o + QPointF(x * u, y * u); }
    QRectF r(double x, double y, double w, double h) const { return QRectF(p(x, y), QSizeF(w * u, h * u)); }
    double s(double v) const { return v * u; }
};

QFont pixelFont(QFont font, int pixels, bool bold, bool italic = false)
{
    font.setPixelSize(pixels);
    font.setBold(bold);
    font.setItalic(italic);
    return font;
}

/* Lines centred in rect, the font shrunk until they fit. */
void drawFitted(QPainter &pnt, const QRectF &rect, const QString &text, int pixels, bool bold, const QColor &color,
                bool italic = false, int align = Qt::AlignCenter)
{
    const QStringList lines = text.split(QLatin1Char('\n'));
    QFont font = pixelFont(pnt.font(), pixels, bold, italic);
    for (;;) {
        const QFontMetricsF metrics(font);
        qreal wide = 0;
        for (const QString &line : lines) wide = qMax(wide, metrics.horizontalAdvance(line));
        const int size = font.pixelSize() > 0 ? font.pixelSize() : 10;
        if ((wide <= rect.width() + 1 && metrics.lineSpacing() * lines.size() <= rect.height() + 3) || size <= 5)
            break;
        font.setPixelSize(size - 1);
    }
    pnt.setFont(font);
    pnt.setPen(color);
    pnt.drawText(rect, align, text);
}

/* A polygon with its corners rounded by r. */
QPainterPath roundedPolygon(const QVector<QPointF> &pts, double radius)
{
    QPainterPath path;
    const int n = pts.size();
    for (int i = 0; i < n; ++i) {
        const QPointF prev = pts[(i + n - 1) % n], cur = pts[i], next = pts[(i + 1) % n];
        auto toward = [&](const QPointF &from, const QPointF &to) {
            const QPointF delta = to - from;
            const double len = std::hypot(delta.x(), delta.y());
            const double take = std::min(radius, len / 2);
            return from + delta * (take / len);
        };
        const QPointF a = toward(cur, prev), b = toward(cur, next);
        if (i == 0) path.moveTo(a); else path.lineTo(a);
        path.quadTo(cur, b);
    }
    path.closeSubpath();
    return path;
}

/* A binding's key, shortened to fit a small part (tooltips keep the full name). */
QString legendName(QString key)
{
    if (key.startsWith(QLatin1String("Keypad "))) return QStringLiteral("KP ") + key.mid(7);
    if (key.startsWith(QLatin1String("Left "))) return QStringLiteral("L ") + key.mid(5);
    if (key.startsWith(QLatin1String("Right "))) return QStringLiteral("R ") + key.mid(6);
    if (key == QLatin1String("Backspace")) return QStringLiteral("BkSp");
    if (key == QLatin1String("Escape")) return QStringLiteral("Esc");
    if (key == QLatin1String("Return")) return QStringLiteral("Ret");
    if (key == QLatin1String("Space")) return QStringLiteral("Spc");
    if (key == QLatin1String("Up")) return QStringLiteral("↑");
    if (key == QLatin1String("Down")) return QStringLiteral("↓");
    return key;
}

QString keyName(a2600session *sess, int target)
{
    char name[48] = "";
    const a2600_binding bind = a2600session_binding_get(sess, target);
    if (bind.keysym) a2600session_keysym_name(bind.keysym, name, sizeof name);
    return local(name);
}

QString padName(a2600session *sess, int target)
{
    const a2600_binding bind = a2600session_binding_get(sess, target);
    return bind.button == A2600_PAD_BTN_NONE ? QString() : local(a2600_pad_button_name(bind.button));
}

QString orNone(const QString &text) { return text.isEmpty() ? QStringLiteral("none") : text; }

/* The target's number within its port (an a2600_action), or -1 for the console's. */
int actionOf(int target, int port)
{
    if (port < 0 || port > 1) return -1;
    const int act = target - port * A2600_ACT_PER_PORT;
    return act >= 0 && act < A2600_ACT_PER_PORT ? act : -1;
}

/* ---- ElidedLabel: never widens the window --------------------------------------------- */

class ElidedLabel : public QWidget {
public:
    explicit ElidedLabel(bool dim, int lines = 1) : m_dim(dim), m_lines(lines)
    {
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    }
    void setText(const QString &text)
    {
        if (text == m_text) return;
        m_text = text;
        update();
    }
    QString text() const { return m_text; }
    QSize sizeHint() const override { return QSize(0, fontMetrics().lineSpacing() * m_lines); }
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter pnt(this);
        pnt.setPen(m_dim ? QColor(Qt::gray) : palette().color(QPalette::WindowText));
        if (m_lines > 1)
            pnt.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap, m_text);
        else
            pnt.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter,
                         fontMetrics().elidedText(m_text, Qt::ElideRight, width()));
    }

private:
    QString m_text;
    bool m_dim;
    int m_lines;
};

/* ---- StageWidget: both ports and the console ------------------------------------------- */

class StageWidget : public QWidget {
public:
    /* onPick: a control was chosen in Map mode (its target); onHint: a message for the hint line */
    StageWidget(a2600session *session, std::function<void(int)> onPick, std::function<void(const QString &)> onHint)
        : m_session(session), m_onPick(std::move(onPick)), m_onHint(std::move(onHint))
    {
        setFocusPolicy(Qt::NoFocus);
        setMouseTracking(true);
        setFixedSize(kStageW, kStageH);
        syncLayout();
        pollHeld();
    }

    /* Lets go of everything the mouse holds (also on focus loss and rebuild). */
    void releaseAll()
    {
        if (m_hold == Stick) {
            a2600session_pointer_set(m_session, m_holdPort, 0);
        } else if (m_hold == Knob || m_hold == Wheel) {
            if (m_hold == Knob) a2600session_pointer_analog(m_session, m_holdPort, m_holdPaddle, 0, 0);
        } else if (m_hold == Part && m_partDown) {
            a2600session_press(m_session, m_holdTarget, 0);
        }
        m_hold = None;
        m_holdIdx = -1;
        m_partDown = false;
        m_stickVec = QPointF();
        update();
    }

    /* -2 idle, -1 armed, >= 0 waiting for a key or pad button for that target */
    void setMap(int state)
    {
        m_map = state;
        update();
    }
    void setShowBindings(bool visible)
    {
        m_showBindings = visible;
        update();
    }

    /* The layout for what is plugged in; true when a port's controller changed. */
    bool syncLayout()
    {
        const int left = a2600session_view_type(m_session, 0), right = a2600session_view_type(m_session, 1);
        if (m_lay && left == m_types[0] && right == m_types[1]) return false;
        if (m_hold != None) releaseAll();
        m_types[0] = left;
        m_types[1] = right;
        m_lay = a2600session_controls_layout(left, right, &m_n, nullptr, nullptr);
        update();
        return true;
    }

    /* What the console sees held (keyboard, gamepads, the mouse), where the paddles
     * and the wheel stand and where the latched switches are; repaints only on a
     * change. */
    void pollHeld()
    {
        bool changed = syncLayout();
        for (int port = 0; port < 2; ++port) {
            const unsigned held = a2600session_buttons_held(m_session, port);
            if (held != m_held[port]) {
                m_held[port] = held;
                changed = true;
            }
            for (int pad = -1; pad <= 1; ++pad) {
                const int pos = a2600session_analog_get(m_session, port, pad);
                if (pos != m_pos[port][pad + 1]) {
                    m_pos[port][pad + 1] = pos;
                    changed = true;
                }
            }
        }
        for (int sw = 0; sw < A2600_SW_COUNT; ++sw) {
            const int now = sw == A2600_SW_SELECT || sw == A2600_SW_RESET ? a2600session_switch_held(m_session, sw)
                                                                         : a2600session_switch_get(m_session, sw);
            if (now != m_sw[sw]) {
                m_sw[sw] = now;
                changed = true;
            }
        }
        if (changed) update();
    }

    /* the part with this label (the nth of them), as a pixel rect in the stage */
    QRectF partRect(int idx) const
    {
        const a2600_control &ctl = m_lay[idx];
        return QRectF(origin() + QPointF(ctl.x * kUnit, ctl.y * kUnit), QSizeF(ctl.w * kUnit, ctl.h * kUnit));
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter pnt(this);
        pnt.setRenderHint(QPainter::Antialiasing);
        pnt.setRenderHint(QPainter::TextAntialiasing);
        paintMat(pnt);
        syncLayout();
        for (int idx = 0; idx < m_n; ++idx) paintPart(pnt, idx);
    }

    void mousePressEvent(QMouseEvent *evt) override
    {
        if (evt->button() != Qt::LeftButton) return;
        const QPointF pos = evt->position();
        const int hit = hitAt(pos);
        if (m_map != -2) {
            if (hit >= 0) pick(hit, pos);
            return;
        }
        if (m_hold != None || hit < 0) return;     /* one thing at a time */
        const a2600_control &ctl = m_lay[hit];
        switch (ctl.kind) {
        case A2600_CTL_STICK:
            m_hold = Stick;
            m_holdIdx = hit;
            m_holdPort = ctl.port;
            dragStick(ctl, pos);
            break;
        case A2600_CTL_KNOB:
            m_hold = Knob;
            m_holdIdx = hit;
            m_holdPort = ctl.port;
            m_holdPaddle = ctl.paddle;
            dragKnob(ctl, pos);
            break;
        case A2600_CTL_WHEEL:
            m_hold = Wheel;
            m_holdIdx = hit;
            m_holdPort = ctl.port;
            m_wheelLast = wheelAngle(ctl, pos);
            m_wheelAccum = 0;
            break;
        case A2600_CTL_SWITCH:
            /* a latching switch flips on the press */
            a2600session_press(m_session, ctl.target, 1);
            a2600session_press(m_session, ctl.target, 0);
            pollHeld();
            return;
        case A2600_CTL_BUTTON:
        case A2600_CTL_ROUND_BUTTON:
        case A2600_CTL_TURN:
            m_holdIdx = hit;
            m_holdTarget = ctl.target;
            m_partDown = true;
            if (ctl.target >= A2600_TARGET_SYSACT(0)) {
                m_hold = Strip;                    /* fires on release, like a real button */
            } else {
                m_hold = Part;
                a2600session_press(m_session, ctl.target, 1);
            }
            break;
        default: return;
        }
        pollHeld();
        update();
    }

    /* Dragging off a button lets it go (and back on, holds it again); the stick
     * follows the pointer; the knob turns to it; the wheel turns by the angle. */
    void mouseMoveEvent(QMouseEvent *evt) override
    {
        const QPointF pos = evt->position();
        const int hover = m_hold == None ? hitAt(pos) : -1;
        if (hover != m_hover) {
            m_hover = hover;
            update();
        }
        if (m_hold == Stick) {
            dragStick(m_lay[m_holdIdx], pos);
            update();
        } else if (m_hold == Knob) {
            dragKnob(m_lay[m_holdIdx], pos);
        } else if (m_hold == Wheel) {
            dragWheel(m_lay[m_holdIdx], pos);
        } else if (m_hold == Part || m_hold == Strip) {
            const bool over = hitAt(pos) == m_holdIdx;
            if (over != m_partDown) {
                m_partDown = over;
                if (m_hold == Part) a2600session_press(m_session, m_holdTarget, over ? 1 : 0);
                pollHeld();
                update();
            }
        }
    }

    void mouseReleaseEvent(QMouseEvent *evt) override
    {
        if (evt->button() != Qt::LeftButton) return;
        if (m_hold == Strip && m_partDown && hitAt(evt->position()) == m_holdIdx) {
            /* a console action: the main window's queue runs it, as it does for a key or a pad */
            a2600session_sysaction_post(m_session, m_holdTarget - A2600_TARGET_SYSACT(0));
        }
        if (m_hold != None) releaseAll();
        m_hover = hitAt(evt->position());
        pollHeld();
        update();
    }

    void wheelEvent(QWheelEvent *evt) override
    {
        if (m_map != -2 || m_hold != None) return;
        const int idx = hitAt(evt->position());
        if (idx < 0) return;
        const a2600_control &ctl = m_lay[idx];
        const int steps = evt->angleDelta().y() / 120;
        if (!steps) return;
        if (ctl.kind == A2600_CTL_KNOB) {
            const int value = qBound(-32767, a2600session_analog_get(m_session, ctl.port, ctl.paddle) + steps * kKnobNudge,
                                     32767);
            a2600session_pointer_analog(m_session, ctl.port, ctl.paddle, value, 1);
            a2600session_pointer_analog(m_session, ctl.port, ctl.paddle, value, 0);
        } else if (ctl.kind == A2600_CTL_WHEEL) {
            for (int notch = 0; notch < std::abs(steps); ++notch) {
                a2600session_twist_pulse(m_session, ctl.port, steps > 0 ? 1 : -1);
                m_wheelTurn[ctl.port] += steps > 0 ? A2600_WHEEL_STEP_DEG : -A2600_WHEEL_STEP_DEG;
            }
        } else {
            return;
        }
        pollHeld();
        update();
        evt->accept();
    }

    void leaveEvent(QEvent *evt) override
    {
        if (m_hover != -1) {
            m_hover = -1;
            update();
        }
        QWidget::leaveEvent(evt);
    }

    bool event(QEvent *evt) override
    {
        if (evt->type() == QEvent::ToolTip) {
            auto *help = static_cast<QHelpEvent *>(evt);
            const QString tip = tipAt(help->pos());
            if (tip.isEmpty())
                QToolTip::hideText();
            else
                QToolTip::showText(help->globalPos(), tip, this);
            return true;
        }
        return QWidget::event(evt);
    }

private:
    enum Hold { None, Stick, Knob, Wheel, Part, Strip };

    static QPointF origin() { return a2600ctl::origin(); }

    int hitAt(const QPointF &pos) const
    {
        return a2600session_controls_hit(m_types[0], m_types[1], float((pos.x() - kMargin) / kUnit),
                                         float((pos.y() - kMargin) / kUnit));
    }

    /* a part's centre in layout units */
    static QPointF centreOf(const a2600_control &ctl) { return QPointF(ctl.x + ctl.w / 2, ctl.y + ctl.h / 2); }

    /* the pointer, in layout units from a part's centre */
    QPointF fromCentre(const a2600_control &ctl, const QPointF &pos) const
    {
        const QPointF centre = centreOf(ctl);
        return QPointF((pos.x() - kMargin) / kUnit - centre.x(), (pos.y() - kMargin) / kUnit - centre.y());
    }

    /* The stick follows the pointer wherever it is dragged to: the directions it
     * resolves to are the pointer's hold on the port. */
    void dragStick(const a2600_control &ctl, const QPointF &pos)
    {
        const QPointF d = fromCentre(ctl, pos);
        const float radius = ctl.w / 2;
        a2600session_pointer_set(m_session, ctl.port, a2600session_stick_from_vector(float(d.x()), float(d.y()), radius));
        const double len = std::hypot(d.x(), d.y());
        m_stickVec = len > radius ? QPointF(d.x() / len, d.y() / len) : QPointF(d.x() / radius, d.y() / radius);
        pollHeld();
    }

    /* The knob to the angle of the pointer (nothing in the dead centre). */
    void dragKnob(const a2600_control &ctl, const QPointF &pos)
    {
        const QPointF d = fromCentre(ctl, pos);
        if (std::hypot(d.x(), d.y()) < ctl.w / 2 * 0.2) return;
        a2600session_pointer_analog(m_session, ctl.port, ctl.paddle, a2600session_knob_from_vector(float(d.x()), float(d.y())), 1);
        pollHeld();
    }

    float wheelAngle(const a2600_control &ctl, const QPointF &pos) const
    {
        const QPointF d = fromCentre(ctl, pos);
        return a2600session_vector_angle(float(d.x()), float(d.y()));
    }

    /* One twist per A2600_WHEEL_STEP_DEG of the shortest turn. */
    void dragWheel(const a2600_control &ctl, const QPointF &pos)
    {
        const QPointF d = fromCentre(ctl, pos);
        if (std::hypot(d.x(), d.y()) < ctl.w / 2 * 0.2) return;
        const float angle = wheelAngle(ctl, pos);
        float delta = angle - m_wheelLast;
        while (delta > 180.f) delta -= 360.f;
        while (delta < -180.f) delta += 360.f;
        m_wheelLast = angle;
        m_wheelAccum += delta;
        while (m_wheelAccum >= A2600_WHEEL_STEP_DEG) {
            a2600session_twist_pulse(m_session, ctl.port, 1);
            m_wheelAccum -= A2600_WHEEL_STEP_DEG;
            m_wheelTurn[ctl.port] += A2600_WHEEL_STEP_DEG;
        }
        while (m_wheelAccum <= -A2600_WHEEL_STEP_DEG) {
            a2600session_twist_pulse(m_session, ctl.port, -1);
            m_wheelAccum += A2600_WHEEL_STEP_DEG;
            m_wheelTurn[ctl.port] -= A2600_WHEEL_STEP_DEG;
        }
        pollHeld();
        update();
    }

    /* Map mode: a click on a control picks its target. The stick has four (the side
     * of the ring that was clicked), a knob and the wheel two (left half, right half). */
    void pick(int idx, const QPointF &pos)
    {
        const a2600_control &ctl = m_lay[idx];
        const QPointF d = fromCentre(ctl, pos);
        if (ctl.kind == A2600_CTL_STICK) {
            if (std::hypot(d.x(), d.y()) < ctl.w / 2 * 0.3) {
                m_onHint(QStringLiteral("Click toward a side of the ring: up, down, left or right."));
                return;
            }
            const int act = std::abs(d.x()) > std::abs(d.y()) ? (d.x() < 0 ? A2600_ACT_JOY_LEFT : A2600_ACT_JOY_RIGHT)
                                                              : (d.y() < 0 ? A2600_ACT_JOY_UP : A2600_ACT_JOY_DOWN);
            m_onPick(A2600_TARGET_PORT(ctl.port, act));
        } else if (ctl.kind == A2600_CTL_KNOB) {
            const int dec = ctl.paddle == 1 ? A2600_ACT_PADDLE_B_DEC : A2600_ACT_PADDLE_A_DEC;
            m_onPick(A2600_TARGET_PORT(ctl.port, d.x() < 0 ? dec : dec + 1));
        } else if (ctl.kind == A2600_CTL_WHEEL) {
            m_onPick(A2600_TARGET_PORT(ctl.port, d.x() < 0 ? A2600_ACT_DRIVE_CCW : A2600_ACT_DRIVE_CW));
        } else if (ctl.target >= 0) {
            m_onPick(ctl.target);
        }
    }

    /* the targets a part drives, for Map's dimming and highlight */
    std::vector<int> targetsOf(const a2600_control &ctl) const
    {
        switch (ctl.kind) {
        case A2600_CTL_STICK:
            return { A2600_TARGET_PORT(ctl.port, A2600_ACT_JOY_UP), A2600_TARGET_PORT(ctl.port, A2600_ACT_JOY_DOWN),
                     A2600_TARGET_PORT(ctl.port, A2600_ACT_JOY_LEFT), A2600_TARGET_PORT(ctl.port, A2600_ACT_JOY_RIGHT) };
        case A2600_CTL_KNOB: {
            const int dec = ctl.paddle == 1 ? A2600_ACT_PADDLE_B_DEC : A2600_ACT_PADDLE_A_DEC;
            return { A2600_TARGET_PORT(ctl.port, dec), A2600_TARGET_PORT(ctl.port, dec + 1) };
        }
        case A2600_CTL_WHEEL:
            return { A2600_TARGET_PORT(ctl.port, A2600_ACT_DRIVE_CCW), A2600_TARGET_PORT(ctl.port, A2600_ACT_DRIVE_CW) };
        default:
            if (ctl.target >= 0) return { ctl.target };
            return {};
        }
    }

    bool mapping() const { return m_map != -2; }
    bool showBind() const { return mapping() || m_showBindings; }

    /* While Map waits for a key, every control but the picked one steps back. */
    bool dimmed(const a2600_control &ctl) const
    {
        if (m_map < 0) return false;
        const std::vector<int> targets = targetsOf(ctl);
        if (targets.empty()) return false;
        for (int target : targets) if (target == m_map) return false;
        return true;
    }
    bool picked(const a2600_control &ctl) const
    {
        if (m_map < 0) return false;
        for (int target : targetsOf(ctl)) if (target == m_map) return true;
        return false;
    }

    /* Is the part lit: held by anyone (keyboard, a pad, this window, a pulse)? */
    bool heldPart(const a2600_control &ctl) const
    {
        if (ctl.target < 0) return false;
        if (ctl.target >= A2600_TARGET_SYSACT(0))
            return m_hold == Strip && m_partDown && m_holdTarget == ctl.target;
        if (ctl.target >= A2600_TARGET_SWITCH(0)) {
            const int sw = ctl.target - A2600_TARGET_SWITCH(0);
            return (sw == A2600_SW_SELECT || sw == A2600_SW_RESET) && m_sw[sw] != 0;
        }
        const int act = actionOf(ctl.target, ctl.port);
        return act >= 0 && (m_held[ctl.port] & (1u << act)) != 0;
    }

    QString tipAt(const QPoint &pos) const
    {
        const int idx = hitAt(pos);
        if (idx < 0) return QString();
        const a2600_control &ctl = m_lay[idx];
        if (ctl.kind == A2600_CTL_STICK) {
            auto key = [&](int act) { return orNone(keyName(m_session, A2600_TARGET_PORT(ctl.port, act))); };
            return QStringLiteral("Drag to move the stick\nUp: %1  Down: %2  Left: %3  Right: %4")
                .arg(key(A2600_ACT_JOY_UP), key(A2600_ACT_JOY_DOWN), key(A2600_ACT_JOY_LEFT), key(A2600_ACT_JOY_RIGHT));
        }
        if (ctl.kind == A2600_CTL_KNOB || ctl.kind == A2600_CTL_WHEEL) {
            const bool wheel = ctl.kind == A2600_CTL_WHEEL;
            const int dec = wheel ? A2600_ACT_DRIVE_CCW : (ctl.paddle == 1 ? A2600_ACT_PADDLE_B_DEC : A2600_ACT_PADDLE_A_DEC);
            return QStringLiteral("%1: drag round to turn it, scroll to nudge it\n%2: %3    %4: %5")
                .arg(wheel ? QStringLiteral("Driving wheel") : QStringLiteral("Paddle %1").arg(ctl.paddle ? 'B' : 'A'),
                     wheel ? QStringLiteral("Counter-clockwise") : QStringLiteral("Left"),
                     orNone(keyName(m_session, A2600_TARGET_PORT(ctl.port, dec))),
                     wheel ? QStringLiteral("Clockwise") : QStringLiteral("Right"),
                     orNone(keyName(m_session, A2600_TARGET_PORT(ctl.port, dec + 1))));
        }
        if (ctl.target >= 0) {
            return QStringLiteral("%1\nKey: %2    Gamepad: %3")
                .arg(local(a2600_target_name(ctl.target)), orNone(keyName(m_session, ctl.target)),
                     orNone(padName(m_session, ctl.target)));
        }
        return QString();
    }

    /* ---- painting ------------------------------------------------------------------ */

    void paintMat(QPainter &pnt)
    {
        const QRectF mat(1, 1, width() - 2, height() - 2);
        QLinearGradient grad(mat.topLeft(), mat.bottomLeft());
        grad.setColorAt(0, kMatTop);
        grad.setColorAt(1, kMatBottom);
        pnt.setPen(QPen(kMatEdge, 2));
        pnt.setBrush(grad);
        pnt.drawRoundedRect(mat, 14, 14);
    }

    QColor heldFill() const { return a2600CtlHeld(); }

    void paintPart(QPainter &pnt, int idx)
    {
        const a2600_control &ctl = m_lay[idx];
        const bool dim = dimmed(ctl);
        const bool lit = heldPart(ctl);
        if (dim && ctl.kind != A2600_CTL_BODY && ctl.kind != A2600_CTL_CAPTION) pnt.setOpacity(kDimAlpha);
        pnt.save();
        switch (ctl.kind) {
        case A2600_CTL_CAPTION: paintCaption(pnt, ctl); break;
        case A2600_CTL_BODY: paintBody(pnt, ctl); break;
        case A2600_CTL_STICK: paintStick(pnt, ctl); break;
        case A2600_CTL_KNOB:
        case A2600_CTL_WHEEL: paintKnob(pnt, ctl); break;
        case A2600_CTL_ROUND_BUTTON: paintRoundButton(pnt, ctl, lit); break;
        case A2600_CTL_BUTTON: paintButton(pnt, ctl, lit); break;
        case A2600_CTL_TURN: paintTurn(pnt, ctl, lit); break;
        case A2600_CTL_SWITCH: paintSwitch(pnt, ctl); break;
        default: break;
        }
        pnt.restore();
        pnt.setOpacity(1);
        if (picked(ctl)) {
            pnt.setPen(QPen(a2600CtlAccent(), 2));
            pnt.setBrush(Qt::NoBrush);
            const QRectF box = partRect(idx).adjusted(-2, -2, 2, 2);
            if (ctl.kind == A2600_CTL_STICK || ctl.kind == A2600_CTL_KNOB || ctl.kind == A2600_CTL_WHEEL ||
                ctl.kind == A2600_CTL_ROUND_BUTTON)
                pnt.drawEllipse(box);
            else
                pnt.drawRoundedRect(box, 4, 4);
        } else if (m_hover == idx && m_hold == None && m_map == -2) {
            pnt.setPen(QPen(QColor(255, 255, 255, 70), 1.5));
            pnt.setBrush(Qt::NoBrush);
            const QRectF box = partRect(idx).adjusted(-1, -1, 1, 1);
            if (ctl.kind == A2600_CTL_STICK || ctl.kind == A2600_CTL_KNOB || ctl.kind == A2600_CTL_WHEEL ||
                ctl.kind == A2600_CTL_ROUND_BUTTON)
                pnt.drawEllipse(box);
            else
                pnt.drawRoundedRect(box, 3, 3);
        }
        if (showBind() && !dim) paintBindingTags(pnt, ctl);
    }

    void paintCaption(QPainter &pnt, const a2600_control &ctl)
    {
        const int type = m_types[ctl.port];
        const QString text = QStringLiteral("%1 — %2").arg(local(ctl.label), local(a2600_ctrl_type_name(type)));
        drawFitted(pnt, QRectF(origin() + QPointF(ctl.x * kUnit, ctl.y * kUnit), QSizeF(ctl.w * kUnit, ctl.h * kUnit)),
                   text, 13, true, kMatInk);
    }

    /* The device a body belongs to: its top-left in pixels. */
    Dev devOf(const a2600_control &body) const
    {
        Dev dev;
        dev.o = origin() + QPointF(body.x * kUnit, body.y * kUnit);
        return dev;
    }

    void paintBody(QPainter &pnt, const a2600_control &body)
    {
        const QString label = local(body.label);
        const Dev dev = devOf(body);
        /* the slot and the wood are drawn from their own boxes: only a device's body
         * is the origin its parts' art is written in */
        if (label != QLatin1String("Cartridge slot") && label != QLatin1String("Wood")) {
            m_dev = dev;
            m_devBody = QPointF(body.x, body.y);
        }
        if (label == QLatin1String("Joystick")) paintJoystickBody(pnt, dev, body.port);
        else if (label == QLatin1String("Paddle A") || label == QLatin1String("Paddle B") ||
                 label == QLatin1String("Driving"))
            paintPaddleBody(pnt, dev, body);
        else if (label == QLatin1String("Keypad")) paintKeypadBody(pnt, dev);
        else if (label == QLatin1String("Console")) paintConsoleBody(pnt, dev);
        else if (label == QLatin1String("Cartridge slot")) paintSlot(pnt, body);
        else if (label == QLatin1String("Wood")) paintWood(pnt, body);
    }

    /* ---- the CX40 joystick (DESIGN-atari.md section 2) ---- */

    /* the stick's tilt: -1..1 each way, from the drag or from what is held */
    QPointF tiltOf(int port) const
    {
        if (m_hold == Stick && m_holdPort == port) return m_stickVec;
        const unsigned held = m_held[port];
        double dx = 0, dy = 0;
        if (held & (1u << A2600_ACT_JOY_LEFT)) dx -= 1;
        if (held & (1u << A2600_ACT_JOY_RIGHT)) dx += 1;
        if (held & (1u << A2600_ACT_JOY_UP)) dy -= 1;
        if (held & (1u << A2600_ACT_JOY_DOWN)) dy += 1;
        if (dx && dy) { dx *= 0.7071; dy *= 0.7071; }
        return QPointF(dx, dy);
    }

    void paintJoystickBody(QPainter &pnt, const Dev &dev, int port)
    {
        /* the case: textured black, a bevel round its top face */
        const QRectF box = dev.r(0, 0, 8, 8);
        QLinearGradient face(box.topLeft(), box.bottomRight());
        face.setColorAt(0, QColor(0x30, 0x30, 0x30));
        face.setColorAt(1, QColor(0x17, 0x17, 0x17));
        pnt.setPen(QPen(kPlasticDeep, dev.s(0.14)));
        pnt.setBrush(face);
        pnt.drawRoundedRect(box, dev.s(0.9), dev.s(0.9));
        pnt.setPen(QPen(kPlasticHi, dev.s(0.1)));
        pnt.setBrush(Qt::NoBrush);
        pnt.drawRoundedRect(dev.r(0.25, 0.25, 7.5, 7.5), dev.s(0.7), dev.s(0.7));

        /* the fire button's recess: a teardrop, a round pit joined to the ring */
        const QPointF fireC(1.7, 1.7), ringC(4.5, 4.5);
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(kPlasticDeep);
        pnt.drawEllipse(dev.p(fireC.x(), fireC.y()), dev.s(1.1), dev.s(1.1));
        {
            const QPointF dir(0.7071, 0.7071), perp(-0.7071, 0.7071);
            const double half = 0.45, reach = 2.2;
            QPolygonF neck;
            neck << dev.p(fireC.x() + perp.x() * half, fireC.y() + perp.y() * half)
                 << dev.p(fireC.x() + dir.x() * reach + perp.x() * half, fireC.y() + dir.y() * reach + perp.y() * half)
                 << dev.p(fireC.x() + dir.x() * reach - perp.x() * half, fireC.y() + dir.y() * reach - perp.y() * half)
                 << dev.p(fireC.x() - perp.x() * half, fireC.y() - perp.y() * half);
            pnt.drawPolygon(neck);
        }

        /* the ring and its ticks */
        const QPointF centre = dev.p(ringC.x(), ringC.y());
        pnt.setBrush(kPlasticDeep);
        pnt.drawEllipse(centre, dev.s(3.0), dev.s(3.0));

        /* a held direction lights its sector of the ring (two held: the diagonal's) */
        const QPointF tilt = tiltOf(port);
        const unsigned held = m_held[port];
        const bool heldDir = held & ((1u << A2600_ACT_JOY_UP) | (1u << A2600_ACT_JOY_DOWN) | (1u << A2600_ACT_JOY_LEFT) |
                                     (1u << A2600_ACT_JOY_RIGHT));
        if (heldDir || (m_hold == Stick && m_holdPort == port && std::hypot(tilt.x(), tilt.y()) > 0.3)) {
            const double qtAngle = qRadiansToDegrees(std::atan2(-tilt.y(), tilt.x()));
            QPainterPath wedge;
            wedge.moveTo(centre);
            wedge.arcTo(QRectF(centre.x() - dev.s(3.0), centre.y() - dev.s(3.0), dev.s(6.0), dev.s(6.0)), qtAngle - 22.5, 45);
            wedge.closeSubpath();
            QColor tint = heldFill();
            tint.setAlpha(110);
            pnt.setBrush(tint);
            pnt.drawPath(wedge);
        }
        QColor tick(0x3A, 0x3A, 0x3A);
        pnt.setPen(QPen(tick, dev.s(0.1), Qt::SolidLine, Qt::RoundCap));
        for (int i = 0; i < 36; ++i) {
            const double a = qDegreesToRadians(i * 10.0);
            const double r0 = 2.55, r1 = 2.85;
            pnt.drawLine(dev.p(ringC.x() + std::cos(a) * r0, ringC.y() + std::sin(a) * r0),
                         dev.p(ringC.x() + std::cos(a) * r1, ringC.y() + std::sin(a) * r1));
        }
        /* "TOP", embossed near the top of the ring */
        drawFitted(pnt, dev.r(3.7, 1.52, 1.6, 0.5), QStringLiteral("TOP"), 9, true, kPlasticHi);
    }

    void paintStick(QPainter &pnt, const a2600_control &ctl)
    {
        const Dev dev = m_dev;
        const double cx = ctl.x - m_devBody.x() + ctl.w / 2, cy = ctl.y - m_devBody.y() + ctl.h / 2;
        const QPointF tilt = tiltOf(ctl.port);

        /* the boot: concentric ribbed rings, squashed a little toward the tilt */
        static const struct { double r; QColor col; } rings[] = {
            { 2.1, QColor(0x15, 0x15, 0x15) }, { 1.7, QColor(0x26, 0x26, 0x26) }, { 1.3, QColor(0x15, 0x15, 0x15) },
        };
        pnt.setPen(Qt::NoPen);
        for (const auto &ring : rings) {
            pnt.setBrush(ring.col);
            pnt.drawEllipse(dev.p(cx + tilt.x() * 0.3 * (ring.r / 2.1), cy + tilt.y() * 0.3 * (ring.r / 2.1)),
                            dev.s(ring.r), dev.s(ring.r * (1 - 0.08 * std::hypot(tilt.x(), tilt.y()))));
        }
        /* the shaft cap: an octagon that slides toward where it is pushed, up to 1.0 u */
        const QPointF cap = dev.p(cx + tilt.x() * 1.0, cy + tilt.y() * 1.0);
        QPolygonF octagon;
        for (int i = 0; i < 8; ++i) {
            const double a = qDegreesToRadians(22.5 + i * 45.0);
            octagon << cap + QPointF(std::cos(a) * dev.s(0.85), std::sin(a) * dev.s(0.85));
        }
        pnt.setPen(QPen(kPlasticHi, dev.s(0.1)));
        pnt.setBrush(QColor(0x0C, 0x0C, 0x0C));
        pnt.drawPolygon(octagon);
        /* a highlight on the cap's upper-left, so it reads as a top */
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(QColor(255, 255, 255, 24));
        pnt.drawEllipse(cap + QPointF(-dev.s(0.25), -dev.s(0.28)), dev.s(0.3), dev.s(0.22));
    }

    void paintRoundButton(QPainter &pnt, const a2600_control &ctl, bool lit)
    {
        const QRectF box = partRect(partIndex(ctl));
        const QPointF centre = box.center();
        const bool fire = local(ctl.label) == QLatin1String("FIRE");
        if (fire) {
            /* the joystick's fire button: red, glossy, in its recess */
            const double r = kUnit * 0.8;
            pnt.setPen(QPen(lit ? heldFill() : kFireDark, lit ? kUnit * 0.15 : kUnit * 0.06));
            pnt.setBrush(lit ? kFireDark : kFire);
            pnt.drawEllipse(centre, r, r);
            pnt.setPen(Qt::NoPen);
            pnt.setBrush(lit ? QColor(255, 255, 255, 30) : kFireHi);
            pnt.drawEllipse(centre + QPointF(-r * 0.25, -r * 0.3), r * 0.45, r * 0.3);
            return;
        }
        /* a keypad key: glossy black, a ring when held, its legend printed above-left */
        const double r = kUnit * 0.5;
        pnt.setPen(QPen(lit ? heldFill() : QColor(0x00, 0x00, 0x00), lit ? kUnit * 0.1 : kUnit * 0.04));
        pnt.setBrush(lit ? QColor(0x3A, 0x2A, 0x14) : kKeyBlack);
        pnt.drawEllipse(centre, r, r);
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(kKeyHi);
        pnt.drawEllipse(centre + QPointF(-r * 0.2, -r * 0.35), r * 0.5, r * 0.28);
        drawFitted(pnt, QRectF(centre + QPointF(-kUnit * 0.95, -kUnit * 0.95), QSizeF(kUnit * 0.7, kUnit * 0.5)),
                   local(ctl.label), 12, true, lit ? Qt::white : kLegendBlue);
    }

    /* ---- the CX30 paddle and the CX20 driving controller (sections 3 and 4) ---- */

    void paintPaddleBody(QPainter &pnt, const Dev &dev, const a2600_control &body)
    {
        const bool driving = local(body.label) == QLatin1String("Driving");
        const QVector<QPointF> pts = { dev.p(1.0, 0.3), dev.p(5.6, 0.3), dev.p(7.0, 5.0), dev.p(6.2, 5.9), dev.p(0.8, 5.9),
                                       dev.p(0.0, 4.6) };
        const QPainterPath shape = roundedPolygon(pts, dev.s(0.7));
        QLinearGradient grad(dev.p(0, 0), dev.p(7, 6));
        grad.setColorAt(0, kPaddleHi);
        grad.setColorAt(1, QColor(0x16, 0x16, 0x16));
        pnt.setPen(QPen(QColor(0x0A, 0x0A, 0x0A), dev.s(0.1)));
        pnt.setBrush(grad);
        pnt.drawPath(shape);
        pnt.setPen(QPen(kPaddleHi, dev.s(0.08)));
        pnt.setBrush(Qt::NoBrush);
        pnt.drawPath(roundedPolygon({ dev.p(1.15, 0.5), dev.p(5.45, 0.5), dev.p(6.75, 4.95), dev.p(6.05, 5.7),
                                      dev.p(0.95, 5.7), dev.p(0.25, 4.55) }, dev.s(0.55)));

        /* the label plate, bottom right */
        const QRectF plate = dev.r(3.85, 4.7, 2.75, 1.05);
        pnt.setPen(QPen(QColor(0x8A, 0x8A, 0x8A), dev.s(0.05)));
        pnt.setBrush(kPlasticDeep);
        pnt.drawRoundedRect(plate, dev.s(0.22), dev.s(0.22));
        drawFitted(pnt, dev.r(3.95, 4.78, 1.55, 0.9), driving ? QStringLiteral("driving") : QStringLiteral("paddle"), 12,
                   true, QColor(0xE6, 0xE6, 0xE6), true);
        if (driving) {
            /* the orange racing car */
            pnt.setPen(Qt::NoPen);
            pnt.setBrush(QColor(0xE8, 0x50, 0x1A));
            pnt.drawRoundedRect(dev.r(5.55, 5.12, 0.9, 0.32), dev.s(0.1), dev.s(0.1));
            pnt.drawRoundedRect(dev.r(5.85, 4.92, 0.4, 0.26), dev.s(0.09), dev.s(0.09));
            pnt.setBrush(QColor(0x10, 0x10, 0x10));
            pnt.drawEllipse(dev.p(5.8, 5.45), dev.s(0.11), dev.s(0.11));
            pnt.drawEllipse(dev.p(6.2, 5.45), dev.s(0.11), dev.s(0.11));
        } else {
            /* two crossed rackets and a ball, in the plate's gold */
            const QColor gold(0xDC, 0xCB, 0x78);
            pnt.setPen(QPen(gold, dev.s(0.06)));
            pnt.setBrush(Qt::NoBrush);
            pnt.drawEllipse(dev.p(5.7, 5.02), dev.s(0.2), dev.s(0.24));
            pnt.drawEllipse(dev.p(6.08, 4.95), dev.s(0.2), dev.s(0.24));
            pnt.setPen(QPen(gold, dev.s(0.08), Qt::SolidLine, Qt::RoundCap));
            pnt.drawLine(dev.p(5.75, 5.26), dev.p(6.0, 5.55));
            pnt.drawLine(dev.p(6.1, 5.2), dev.p(5.85, 5.55));
            pnt.setPen(Qt::NoPen);
            pnt.setBrush(gold);
            pnt.drawEllipse(dev.p(5.5, 5.5), dev.s(0.07), dev.s(0.07));
        }
    }

    /* the knob (paddle) or the wheel (driving controller): ribbed, turning */
    void paintKnob(QPainter &pnt, const a2600_control &ctl)
    {
        const Dev dev = m_dev;
        const bool wheel = ctl.kind == A2600_CTL_WHEEL;
        const double cx = ctl.x - m_devBody.x() + ctl.w / 2, cy = ctl.y - m_devBody.y() + ctl.h / 2;
        const QPointF centre = dev.p(cx, cy);
        const int pos = m_pos[ctl.port][ctl.paddle + 1];
        /* a paddle turns +-135 degrees across its travel; the wheel turns endlessly (what
         * this window and the pads have given it) */
        const double angle = wheel ? m_wheelTurn[ctl.port] + pos / 32767.0 * A2600_KNOB_TRAVEL_DEG
                                   : pos / 32767.0 * A2600_KNOB_TRAVEL_DEG;

        pnt.setPen(QPen(QColor(0x05, 0x05, 0x05), dev.s(0.08)));
        pnt.setBrush(QColor(0x0E, 0x0E, 0x0E));
        pnt.drawEllipse(centre, dev.s(2.05), dev.s(2.05));
        /* the knurled rim: sixty ribs that turn with the knob */
        pnt.setPen(QPen(QColor(0x2A, 0x2A, 0x2A), dev.s(0.09), Qt::SolidLine, Qt::RoundCap));
        for (int i = 0; i < 60; ++i) {
            const double a = qDegreesToRadians(angle + i * 6.0);
            pnt.drawLine(centre + QPointF(std::sin(a) * dev.s(1.88), -std::cos(a) * dev.s(1.88)),
                         centre + QPointF(std::sin(a) * dev.s(2.02), -std::cos(a) * dev.s(2.02)));
        }
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(QColor(0x1A, 0x1A, 0x1A));
        pnt.drawEllipse(centre, dev.s(1.65), dev.s(1.65));
        QRadialGradient cap(centre - QPointF(dev.s(0.4), dev.s(0.5)), dev.s(1.8));
        cap.setColorAt(0, QColor(0x2E, 0x2E, 0x2E));
        cap.setColorAt(1, QColor(0x1E, 0x1E, 0x1E));
        pnt.setBrush(cap);
        pnt.drawEllipse(centre, dev.s(1.55), dev.s(1.55));
        /* the position mark (added: the real knob has none, but a turn must be visible) */
        const double rad = qDegreesToRadians(angle);
        const bool active = (m_hold == Knob || m_hold == Wheel) && m_holdPort == ctl.port;
        pnt.setPen(QPen(active ? heldFill() : QColor(0xD8, 0xD8, 0xD8), dev.s(0.13), Qt::SolidLine, Qt::RoundCap));
        pnt.drawLine(centre + QPointF(std::sin(rad) * dev.s(0.85), -std::cos(rad) * dev.s(0.85)),
                     centre + QPointF(std::sin(rad) * dev.s(1.45), -std::cos(rad) * dev.s(1.45)));
        if (wheel) {
            /* gray-code ticks, twelve of them, turning with it */
            pnt.setPen(Qt::NoPen);
            pnt.setBrush(QColor(0x5A, 0x5A, 0x5A));
            for (int i = 0; i < 12; ++i) {
                const double a = qDegreesToRadians(angle + i * 30.0);
                pnt.drawEllipse(centre + QPointF(std::sin(a) * dev.s(1.3), -std::cos(a) * dev.s(1.3)), dev.s(0.05), dev.s(0.05));
            }
        }
    }

    void paintButton(QPainter &pnt, const a2600_control &ctl, bool lit)
    {
        const QString label = local(ctl.label);
        const QRectF box = partRect(partIndex(ctl));
        if (label == QLatin1String("FIRE")) {
            /* a paddle's red button, on the left edge */
            QLinearGradient grad(box.topLeft(), box.bottomLeft());
            grad.setColorAt(0, lit ? kFireDark : kPaddleButtonHi);
            grad.setColorAt(1, lit ? kFireDark.darker(120) : kPaddleButton);
            pnt.setPen(QPen(lit ? heldFill() : QColor(0x7A, 0x1A, 0x0C), lit ? kUnit * 0.1 : kUnit * 0.05));
            pnt.setBrush(grad);
            pnt.drawRoundedRect(box, kUnit * 0.22, kUnit * 0.22);
            return;
        }
        if (label == QLatin1String("STOP")) {
            /* on the wood strip: the session's stop-in-debugger */
            pnt.setPen(QPen(lit ? heldFill() : kSilverShade, kUnit * (lit ? 0.1 : 0.05)));
            pnt.setBrush(lit ? QColor(0x5A, 0x3A, 0x10) : QColor(0x22, 0x16, 0x0C));
            pnt.drawRoundedRect(box, kUnit * 0.25, kUnit * 0.25);
            drawFitted(pnt, box.adjusted(2, 1, -2, -1), QStringLiteral("STOP"), 11, true, QColor(0xE6, 0xE6, 0xE6));
            return;
        }
        /* POWER, SELECT and RESET are slide switches on the panel */
        paintConsoleSwitch(pnt, ctl, lit);
    }

    void paintTurn(QPainter &pnt, const a2600_control &ctl, bool lit)
    {
        const QRectF box = partRect(partIndex(ctl));
        const bool left = local(ctl.label) == QLatin1String("<") || local(ctl.label) == QLatin1String("CCW");
        QPolygonF tri;
        const QRectF in = box.adjusted(box.width() * 0.12, box.height() * 0.12, -box.width() * 0.12, -box.height() * 0.12);
        if (left)
            tri << QPointF(in.left(), in.center().y()) << QPointF(in.right(), in.top()) << QPointF(in.right(), in.bottom());
        else
            tri << QPointF(in.right(), in.center().y()) << QPointF(in.left(), in.top()) << QPointF(in.left(), in.bottom());
        pnt.setPen(QPen(lit ? heldFill() : QColor(0x05, 0x05, 0x05), kUnit * 0.05));
        pnt.setBrush(lit ? heldFill() : QColor(0x6E, 0x6E, 0x72));
        pnt.drawPolygon(tri);
    }

    /* ---- the CX50 keypad controller (section 5) ---- */

    void paintKeypadBody(QPainter &pnt, const Dev &dev)
    {
        pnt.setPen(QPen(QColor(0x08, 0x08, 0x08), dev.s(0.1)));
        pnt.setBrush(QColor(0x14, 0x14, 0x14));
        pnt.drawRoundedRect(dev.r(0, 0, 5.6, 8.0), dev.s(0.6), dev.s(0.6));
        pnt.setPen(QPen(QColor(0xD7, 0xD7, 0xD7), dev.s(0.07)));
        pnt.setBrush(QColor(0x1A, 0x1A, 0x1A));
        pnt.drawRoundedRect(dev.r(0.45, 0.45, 4.7, 7.1), dev.s(0.35), dev.s(0.35));
        drawFitted(pnt, dev.r(0.7, 0.95, 4.2, 1.2), QStringLiteral("Keyboard\nController"), 16, true, kLegendBlue);
    }

    /* ---- the console (section 6) ---- */

    void paintConsoleBody(QPainter &pnt, const Dev &dev)
    {
        pnt.setPen(QPen(QColor(0x05, 0x05, 0x05), dev.s(0.1)));
        pnt.setBrush(kPanel);
        pnt.drawRoundedRect(dev.r(0, 0, 24.0, 5.2), dev.s(0.4), dev.s(0.4));
        /* the orange pin line, inset */
        pnt.setPen(QPen(kOrange, dev.s(0.06)));
        pnt.setBrush(Qt::NoBrush);
        pnt.drawRoundedRect(dev.r(0.25, 0.25, 23.5, 4.7), dev.s(0.3), dev.s(0.3));
        /* the title, with its trademark */
        drawFitted(pnt, dev.r(7.9, 0.5, 7.4, 0.9), QStringLiteral("video computer system"), 15, true, kTitle, true);
        drawFitted(pnt, dev.r(15.35, 0.4, 0.7, 0.4), QStringLiteral("TM"), 7, false, kTitle);
    }

    void paintSlot(QPainter &pnt, const a2600_control &body)
    {
        const QRectF box = QRectF(origin() + QPointF(body.x * kUnit, body.y * kUnit), QSizeF(body.w * kUnit, body.h * kUnit));
        pnt.setPen(QPen(kPlastic, kUnit * 0.12));
        pnt.setBrush(QColor(0x0A, 0x0A, 0x0A));
        pnt.drawRoundedRect(box, kUnit * 0.2, kUnit * 0.2);
        pnt.setPen(QPen(QColor(0x16, 0x16, 0x16), kUnit * 0.06));
        pnt.drawLine(box.left() + kUnit * 0.5, box.bottom() - kUnit * 0.45, box.right() - kUnit * 0.5, box.bottom() - kUnit * 0.45);
    }

    void paintWood(QPainter &pnt, const a2600_control &body)
    {
        const QRectF box = QRectF(origin() + QPointF(body.x * kUnit, body.y * kUnit), QSizeF(body.w * kUnit, body.h * kUnit));
        QLinearGradient grad(box.topLeft(), box.bottomLeft());
        grad.setColorAt(0, kWoodA);
        grad.setColorAt(1, kWoodB);
        pnt.setPen(QPen(QColor(0x3A, 0x24, 0x10), 1));
        pnt.setBrush(grad);
        pnt.drawRoundedRect(box, kUnit * 0.15, kUnit * 0.15);
        /* the grain */
        QColor grain = kGrain;
        grain.setAlpha(46);
        pnt.setPen(QPen(grain, 1));
        for (int i = 1; i < 9; ++i) {
            const double y = box.top() + box.height() * i / 9.0;
            QPainterPath line;
            line.moveTo(box.left() + 4, y);
            for (int x = 0; x < 12; ++x) {
                const double sx = box.left() + 4 + (box.width() - 8) * (x + 1) / 12.0;
                line.lineTo(sx, y + std::sin(i * 1.7 + x * 0.9) * box.height() * 0.03);
            }
            pnt.drawPath(line);
        }
        /* the Atari fuji, silver, at the right end */
        const double fx = box.right() - kUnit * 3.3, fy = box.center().y();
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(QColor(0xC7, 0xC7, 0xC7));
        const double h = kUnit * 0.95, w = kUnit * 0.16;
        for (int bar = -1; bar <= 1; ++bar) {
            QPolygonF poly;
            const double lean = bar * kUnit * 0.1;
            const double top = bar == 0 ? fy - h / 2 : fy - h / 2 + kUnit * 0.1;
            poly << QPointF(fx + bar * kUnit * 0.38 - w / 2 + lean, top) << QPointF(fx + bar * kUnit * 0.38 + w / 2 + lean, top)
                 << QPointF(fx + bar * kUnit * 0.38 + w / 2 + bar * kUnit * 0.18, fy + h / 2)
                 << QPointF(fx + bar * kUnit * 0.38 - w / 2 + bar * kUnit * 0.18, fy + h / 2);
            pnt.drawPolygon(poly);
        }
        drawFitted(pnt, QRectF(fx + kUnit * 0.75, fy - kUnit * 0.3, kUnit * 1.9, kUnit * 0.6), QStringLiteral("ATARI"), 11,
                   true, QColor(0xC7, 0xC7, 0xC7));
    }

    void paintSwitch(QPainter &pnt, const a2600_control &ctl) { paintConsoleSwitch(pnt, ctl, false); }

    /* A slide switch: a black slot with a silver toggle at the top or bottom, its
     * legend above and its position labels beside. COLOR/B&W and the difficulties
     * are drawn where they ARE (they latch); power is fixed on; select and reset
     * tilt down while held (and their sawtooth arrow lights). */
    void paintConsoleSwitch(QPainter &pnt, const a2600_control &ctl, bool lit)
    {
        const Dev dev = m_dev;                    /* the console's */
        const double cx = ctl.x - m_devBody.x() + ctl.w / 2;
        const double top = ctl.y - m_devBody.y();
        const int sw = ctl.target >= A2600_TARGET_SWITCH(0) && ctl.target < A2600_TARGET_SYSACT(0)
                           ? ctl.target - A2600_TARGET_SWITCH(0) : -1;
        const bool power = ctl.target >= A2600_TARGET_SYSACT(0);
        const bool momentary = sw == A2600_SW_SELECT || sw == A2600_SW_RESET;
        bool up = true;
        if (sw == A2600_SW_COLOR_BW || sw == A2600_SW_LEFT_DIFF || sw == A2600_SW_RIGHT_DIFF) up = m_sw[sw] != 0;
        else if (momentary) up = !lit;
        const QString legend = local(ctl.legend);

        /* the legend above, in two lines when it has two words */
        QString text = legend;
        text.replace(QLatin1Char(' '), QLatin1Char('\n'));
        drawFitted(pnt, dev.r(cx - 1.25, top - 1.2, 2.5, 1.1), text, 11, power, power ? kPowerRed : kLabelOrange, false,
                   Qt::AlignHCenter | Qt::AlignBottom);

        /* the slot and its toggle */
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(QColor(0x0A, 0x0A, 0x0A));
        pnt.drawRoundedRect(dev.r(cx - 0.25, top + 0.35, 0.5, 1.5), dev.s(0.22), dev.s(0.22));
        const QRectF toggle = dev.r(cx - 0.275, up ? top + 0.15 : top + 1.1, 0.55, 0.95);
        QLinearGradient grad(toggle.topLeft(), toggle.topRight());
        grad.setColorAt(0, kSilver);
        grad.setColorAt(0.55, lit ? heldFill() : kSilver);
        grad.setColorAt(1, kSilverShade);
        pnt.setPen(QPen(lit ? heldFill() : QColor(0x50, 0x50, 0x50), dev.s(lit ? 0.1 : 0.04)));
        pnt.setBrush(grad);
        pnt.drawRoundedRect(toggle, dev.s(0.25), dev.s(0.25));

        /* the position labels, to the right of the slot */
        const QColor label = kLabelOrange;
        auto labels = [&](const char *upText, const char *downText) {
            drawFitted(pnt, dev.r(cx + 0.4, top + 0.1, 1.1, 0.8), local(upText), 11, false, label, false,
                       Qt::AlignLeft | Qt::AlignVCenter);
            drawFitted(pnt, dev.r(cx + 0.4, top + 1.1, 1.1, 0.8), local(downText), 11, false, label, false,
                       Qt::AlignLeft | Qt::AlignVCenter);
        };
        if (power) labels("on", "off");
        else if (sw == A2600_SW_COLOR_BW) labels("color", "b·w");
        else if (sw == A2600_SW_LEFT_DIFF || sw == A2600_SW_RIGHT_DIFF) labels("a", "b");
        else if (momentary) {
            /* the sawtooth arrow: bars growing toward the top */
            pnt.setPen(QPen(lit ? heldFill() : kLabelOrange, dev.s(0.09), Qt::SolidLine, Qt::RoundCap));
            for (int bar = 0; bar < 5; ++bar) {
                const double y = top + 1.75 - bar * 0.34;
                pnt.drawLine(dev.p(cx + 0.55, y), dev.p(cx + 0.65 + bar * 0.12, y));
            }
            QPolygonF tip;
            tip << dev.p(cx + 0.75, top + 0.05) << dev.p(cx + 0.55, top + 0.4) << dev.p(cx + 1.0, top + 0.4);
            pnt.setPen(Qt::NoPen);
            pnt.setBrush(lit ? heldFill() : kLabelOrange);
            pnt.drawPolygon(tip);
        }
    }

    /* ---- Show bindings / Map: the key each control has ---- */

    void paintTag(QPainter &pnt, const QRectF &box, const QString &text, bool pill, const QColor &ink = Qt::white)
    {
        if (text.isEmpty()) return;
        if (pill) {
            pnt.setPen(Qt::NoPen);
            pnt.setBrush(QColor(0, 0, 0, 170));
            pnt.drawRoundedRect(box, 3, 3);
        }
        drawFitted(pnt, box.adjusted(1, 0, -1, 0), text, 9, true, ink);
    }

    QString bindText(int target) const
    {
        const QString key = keyName(m_session, target);
        return key.isEmpty() ? QStringLiteral("—") : legendName(key);
    }

    void paintBindingTags(QPainter &pnt, const a2600_control &ctl)
    {
        const QRectF box = partRect(partIndexOf(ctl));
        switch (ctl.kind) {
        case A2600_CTL_STICK: {
            /* the four directions, round the ring */
            const double r = ctl.w / 2 * kUnit;
            const QPointF c = box.center();
            struct Side { int act; double dx, dy; };
            const Side sides[4] = { { A2600_ACT_JOY_UP, 0, -1 }, { A2600_ACT_JOY_DOWN, 0, 1 },
                                    { A2600_ACT_JOY_LEFT, -1, 0 }, { A2600_ACT_JOY_RIGHT, 1, 0 } };
            for (const Side &side : sides) {
                const QPointF at = c + QPointF(side.dx * (r + kUnit * 0.98), side.dy * (r + kUnit * 0.95));
                paintTag(pnt, QRectF(at.x() - kUnit * 0.7, at.y() - kUnit * 0.25, kUnit * 1.4, kUnit * 0.5),
                         bindText(A2600_TARGET_PORT(ctl.port, side.act)), true);
            }
            break;
        }
        case A2600_CTL_KNOB:
        case A2600_CTL_WHEEL:
            break;                              /* the turn arrows carry their keys */
        case A2600_CTL_ROUND_BUTTON:
            if (ctl.target >= 0) {
                const bool fire = local(ctl.label) == QLatin1String("FIRE");
                paintTag(pnt, box.adjusted(fire ? -4 : 1, fire ? 8 : 6, fire ? 4 : -1, fire ? -8 : -6), bindText(ctl.target), false);
            }
            break;
        case A2600_CTL_BUTTON:
            if (ctl.target >= 0 && local(ctl.label) == QLatin1String("FIRE"))
                paintTag(pnt, box.adjusted(-2, 4, 2, -4), bindText(ctl.target), false);
            else if (ctl.target >= 0 && local(ctl.label) == QLatin1String("STOP"))
                paintTag(pnt, box.translated(kUnit * 3.4, 0), bindText(ctl.target), true);
            else if (ctl.target >= 0)
                paintTag(pnt, QRectF(box.center().x() - kUnit * 0.7, box.bottom() + 2, kUnit * 1.4, kUnit * 0.5),
                         bindText(ctl.target), true);
            break;
        case A2600_CTL_SWITCH:
            if (ctl.target >= 0)
                paintTag(pnt, QRectF(box.center().x() - kUnit * 0.7, box.bottom() + 2, kUnit * 1.4, kUnit * 0.5),
                         bindText(ctl.target), true);
            break;
        case A2600_CTL_TURN:
            if (ctl.target >= 0)
                paintTag(pnt, QRectF(box.center().x() - kUnit * 0.4, box.bottom() + 2, kUnit * 0.8, kUnit * 0.5),
                         bindText(ctl.target), true);
            break;
        default: break;
        }
    }

    /* a part's index in the layout (the layout array is contiguous) */
    int partIndexOf(const a2600_control &ctl) const { return int(&ctl - m_lay); }
    int partIndex(const a2600_control &ctl) const { return partIndexOf(ctl); }

    a2600session *m_session;
    std::function<void(int)> m_onPick;
    std::function<void(const QString &)> m_onHint;

    const a2600_control *m_lay = nullptr;
    int m_n = 0;
    int m_types[2] = { -1, -1 };

    unsigned m_held[2] = { 0, 0 };
    int m_pos[2][3] = {};                 /* analog_get: [port][paddle + 1] */
    int m_sw[A2600_SW_COUNT] = {};
    float m_wheelTurn[2] = { 0, 0 };      /* the driving wheel, in degrees, as this window turned it */

    Hold m_hold = None;
    int m_holdIdx = -1;
    int m_holdPort = 0;
    int m_holdPaddle = 0;
    int m_holdTarget = -1;
    bool m_partDown = false;
    QPointF m_stickVec;                   /* the drag, -1..1 each way (drawing only) */
    float m_wheelLast = 0;
    float m_wheelAccum = 0;
    int m_hover = -1;

    int m_map = -2;
    bool m_showBindings = false;

    Dev m_dev;                            /* the device being painted (set by its body) */
    QPointF m_devBody;
};

/* ---- ControllersWindow --------------------------------------------------------------- */

class ControllersWindow : public QWidget {
public:
    ControllersWindow(a2600session *session, QWidget *parent);
    void present();
    void refresh();

protected:
    void keyPressEvent(QKeyEvent *evt) override;
    void keyReleaseEvent(QKeyEvent *evt) override;
    void showEvent(QShowEvent *evt) override;
    void hideEvent(QHideEvent *evt) override;
    void changeEvent(QEvent *evt) override;
    /* Tab is a key of the machine's, not focus traversal. */
    bool focusNextPrevChild(bool) override { return false; }

private:
    void letGo();
    void setMapState(int state);
    void bound(int target, const QString &what, const QString &stolen);
    void pollCapture();
    void pollSlow();
    void syncPlugs();
    void rebuildPads();
    void refreshPadPorts();

    a2600session *m_session;
    StageWidget *m_stage = nullptr;
    QComboBox *m_plug[2] = { nullptr, nullptr };
    QLabel *m_plugNote[2] = { nullptr, nullptr };
    QWidget *m_padArea = nullptr;
    QVBoxLayout *m_padBox = nullptr;
    std::vector<QWidget *> m_padRows;
    std::vector<QLabel *> m_padEff;       /* each pad's "plays the left port" */
    unsigned m_padGen = ~0u;
    QPushButton *m_mapButton = nullptr;
    QPushButton *m_clearButton = nullptr;
    QComboBox *m_targetList = nullptr;
    ElidedLabel *m_hint = nullptr;
    QTimer m_captureTimer;
    QTimer m_heldTimer;
    QTimer m_slowTimer;
    uint32_t m_swallow = 0;               /* the key Map took: its repeats and release too */
    /* -2 idle, -1 armed and waiting for a target, >= 0 waiting for a key or
     * gamepad button for that target. */
    int m_mapState = -2;
};

ControllersWindow::ControllersWindow(a2600session *session, QWidget *parent)
    /* A dialog-type window, so a tiling compositor floats it: it is a panel of
     * fixed-size parts and nothing here wants more room. */
    : QWidget(parent, Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint | Qt::CustomizeWindowHint),
      m_session(session)
{
    setObjectName(QStringLiteral("controllers"));
    setWindowTitle(QStringLiteral("Controllers"));
    setProperty("a2600HelpContext", QStringLiteral("controllers"));
    setFocusPolicy(Qt::StrongFocus);

    auto *root = new QVBoxLayout(this);
    /* Sized to its parts and kept there: a resizable panel gets tiled and its
     * parts stretched. */
    root->setSizeConstraint(QLayout::SetFixedSize);

    m_stage = new StageWidget(
        session,
        [this](int target) { setMapState(target); },
        [this](const QString &text) { m_hint->setText(text); });
    m_stage->setObjectName(QStringLiteral("stage"));
    root->addWidget(m_stage, 0, Qt::AlignHCenter);

    /* what is plugged into each port */
    auto *plug = new QHBoxLayout;
    for (int port = 0; port < 2; ++port) {
        plug->addWidget(new QLabel(port ? QStringLiteral("Right port:") : QStringLiteral("Left port:")));
        m_plug[port] = new QComboBox;
        m_plug[port]->setObjectName(QStringLiteral("plug%1").arg(port));
        m_plug[port]->setFocusPolicy(Qt::NoFocus);
        for (int i = 0; a2600_ctrl_type_name(i); ++i) m_plug[port]->addItem(local(a2600_ctrl_type_name(i)));
        m_plug[port]->setToolTip(QStringLiteral("What is plugged into this port: Auto follows what each cartridge "
                                                "asks for (applied at once, even to a booted game)"));
        connect(m_plug[port], &QComboBox::activated, this, [this, port](int idx) {
            a2600session_set_port_type(m_session, port, idx);
            letGo();
            syncPlugs();
            rebuildPads();
        });
        m_plugNote[port] = new QLabel;
        m_plugNote[port]->setStyleSheet(QStringLiteral("color: gray;"));
        m_plugNote[port]->setFixedWidth(150);
        plug->addWidget(m_plug[port]);
        plug->addWidget(m_plugNote[port]);
        if (!port) plug->addSpacing(18);
    }
    plug->addStretch(1);
    root->addLayout(plug);

    /* Gamepads: one row per pad, rebuilt as they come and go, in room for a
     * fixed number so the window does not change size when one is plugged in */
    auto *pads = new QGroupBox(QStringLiteral("Gamepads"));
    auto *padsBox = new QVBoxLayout(pads);
    m_padArea = new QWidget;
    m_padBox = new QVBoxLayout(m_padArea);
    m_padBox->setContentsMargins(0, 0, 0, 0);
    m_padBox->setSpacing(2);
    const int rowHeight = QComboBox().sizeHint().height() + 2;
    m_padArea->setFixedHeight(rowHeight * kMaxPadRows + m_padBox->spacing() * (kMaxPadRows - 1));
    m_padBox->setAlignment(Qt::AlignTop);
    padsBox->addWidget(m_padArea);
    root->addWidget(pads);

    auto *mapRow = new QHBoxLayout;
    m_mapButton = new QPushButton(QStringLiteral("Map"));
    m_mapButton->setFocusPolicy(Qt::NoFocus);
    m_mapButton->setFixedWidth(qMax(m_mapButton->sizeHint().width(),
                                    m_mapButton->fontMetrics().horizontalAdvance(QStringLiteral("Done")) + 32));
    m_mapButton->setToolTip(QStringLiteral("Pick a control, then press a key or a gamepad button to give it"));
    connect(m_mapButton, &QPushButton::clicked, this, [this] { setMapState(m_mapState == -2 ? -1 : -2); });
    m_clearButton = new QPushButton(QStringLiteral("Clear"));
    m_clearButton->setFocusPolicy(Qt::NoFocus);
    m_clearButton->setToolTip(QStringLiteral("Take the picked control's key and gamepad button away"));
    connect(m_clearButton, &QPushButton::clicked, this, [this] {
        if (m_mapState < 0) return;
        a2600session_binding_clear(m_session, m_mapState);
        m_hint->setText(QStringLiteral("Cleared. Press a key or a gamepad button to bind it again"));
        m_stage->update();
    });
    auto *defaults = new QPushButton(QStringLiteral("Defaults"));
    defaults->setFocusPolicy(Qt::NoFocus);
    defaults->setToolTip(QStringLiteral("Put every key and gamepad button back to its default"));
    connect(defaults, &QPushButton::clicked, this, [this] {
        a2600session_bindings_reset(m_session);
        setMapState(m_mapState == -2 ? -2 : -1);
        m_hint->setText(QStringLiteral("Every control is back to its default."));
    });
    /* all 59 controls, for the ones the picture is not showing (the other port's
     * paddles, the console actions) */
    m_targetList = new QComboBox;
    m_targetList->setObjectName(QStringLiteral("targetList"));
    m_targetList->setFocusPolicy(Qt::NoFocus);
    m_targetList->setFixedWidth(230);
    m_targetList->setMaxVisibleItems(24);
    m_targetList->addItem(QStringLiteral("Pick a control…"), -1);
    for (int target = 0; target < A2600_TARGET_COUNT; ++target)
        m_targetList->addItem(local(a2600_target_name(target)), target);
    m_targetList->setToolTip(QStringLiteral("In Map mode: choose any control, including ones not on the picture"));
    connect(m_targetList, &QComboBox::activated, this, [this](int idx) {
        const int target = m_targetList->itemData(idx).toInt();
        if (target >= 0 && m_mapState != -2) setMapState(target);
        m_targetList->setCurrentIndex(0);
    });
    mapRow->addWidget(m_mapButton);
    mapRow->addWidget(m_clearButton);
    mapRow->addWidget(defaults);
    mapRow->addWidget(m_targetList);
    mapRow->addStretch(1);
    mapRow->addWidget(a2600HelpButton(QStringLiteral("controllers")));
    root->addLayout(mapRow);

    auto *optRow = new QHBoxLayout;
    auto *bindings = new QCheckBox(QStringLiteral("Show bindings"));
    bindings->setObjectName(QStringLiteral("showBindings"));
    bindings->setFocusPolicy(Qt::NoFocus);
    bindings->setToolTip(QStringLiteral("Show the key each control has (hover for its gamepad button)"));
    bindings->setChecked(a2600session_get_int(session, "controllers_show_bindings", 0) > 0);
    connect(bindings, &QCheckBox::toggled, this, [this](bool checked) {
        a2600session_set_int(m_session, "controllers_show_bindings", checked ? 1 : 0);
        m_stage->setShowBindings(checked);
    });
    m_stage->setShowBindings(bindings->isChecked());
    optRow->addWidget(bindings);
    optRow->addStretch(1);
    root->addLayout(optRow);

    m_hint = new ElidedLabel(true, 2);
    root->addWidget(m_hint);

    connect(&m_captureTimer, &QTimer::timeout, this, [this] { pollCapture(); });
    connect(&m_heldTimer, &QTimer::timeout, this, [this] { m_stage->pollHeld(); });
    connect(&m_slowTimer, &QTimer::timeout, this, [this] { pollSlow(); });
    setMapState(-2);
    syncPlugs();
    letGo();
    m_stage->pollHeld();
}

/* Lets go of what the mouse holds in this window. */
void ControllersWindow::letGo()
{
    m_stage->releaseAll();
}

/* The port boxes follow the session (a type changed in Preferences, or a cartridge's). */
void ControllersWindow::syncPlugs()
{
    for (int port = 0; port < 2; ++port) {
        const int type = a2600session_port_type(m_session, port);
        if (m_plug[port]->currentIndex() != type) m_plug[port]->setCurrentIndex(type);
        const QString note = type == A2600_CTRL_AUTO
            ? QStringLiteral("detected: %1").arg(local(a2600_ctrl_type_name(a2600session_detected_port_type(m_session, port))))
            : QStringLiteral("forced");
        if (m_plugNote[port]->text() != note) m_plugNote[port]->setText(note);
    }
}

/* ---- Map mode -------------------------------------------------------------------------- */

void ControllersWindow::setMapState(int state)
{
    m_mapState = state;
    a2600CtlSetAccent(m_mapButton, state != -2);
    m_clearButton->setEnabled(state >= 0);
    m_targetList->setEnabled(state != -2);
    if (state == -2) {
        m_captureTimer.stop();
        a2600session_gamepad_capture_cancel(m_session);
        m_mapButton->setText(QStringLiteral("Map"));
        m_hint->setText(QStringLiteral("Press a control with the mouse to hold it. Drag the stick, turn a knob or the "
                                       "wheel, flip a switch."));
    } else if (state == -1) {
        m_captureTimer.stop();
        a2600session_gamepad_capture_cancel(m_session);
        m_mapButton->setText(QStringLiteral("Done"));
        m_hint->setText(QStringLiteral("Click a control to remap it (the stick: a side of its ring; a knob or the "
                                       "wheel: its left or right half), or pick one from the list. Escape leaves Map."));
    } else {
        letGo();
        m_hint->setText(QStringLiteral("Press a key or a gamepad button for %1 (Escape cancels).")
                            .arg(local(a2600_target_name(state))));
        a2600session_gamepad_capture_begin(m_session);
        m_captureTimer.start(50);
    }
    m_stage->setMap(m_mapState);
    m_stage->pollHeld();
}

/* A binding was made: stay armed, since remapping several in a row is normal. */
void ControllersWindow::bound(int target, const QString &what, const QString &stolen)
{
    setMapState(-1);
    m_hint->setText(stolen.isEmpty() ? QStringLiteral("%1 is now %2").arg(local(a2600_target_name(target)), what)
                                     : QStringLiteral("%1 is now %2 (taken from %3)")
                                           .arg(local(a2600_target_name(target)), what, stolen));
}

void ControllersWindow::pollCapture()
{
    int button;
    if (m_mapState < 0) { m_captureTimer.stop(); return; }
    if (!a2600session_gamepad_capture_poll(m_session, &button)) return;
    char stolen[128] = "";
    const int target = m_mapState;
    a2600session_binding_set_button(m_session, target, button, stolen, sizeof stolen);
    bound(target, local(a2600_pad_button_name(button)), local(stolen));
}

/* ---- polling ---------------------------------------------------------------------------- */

/* Hot-plugged pads, auto-assignment changes, a port type changed in Preferences. */
void ControllersWindow::pollSlow()
{
    syncPlugs();
    const unsigned gen = a2600session_gamepad_generation(m_session);
    if (gen != m_padGen) {
        m_padGen = gen;
        rebuildPads();
    } else {
        refreshPadPorts();
    }
}

/* ---- gamepads --------------------------------------------------------------------------- */

namespace {
QString portName(int port)
{
    return port < 0 ? QStringLiteral("plays nothing")
                    : port == 0 ? QStringLiteral("plays the left port") : QStringLiteral("plays the right port");
}
} // namespace

void ControllersWindow::refreshPadPorts()
{
    for (int idx = 0; idx < (int)m_padEff.size(); ++idx) {
        const QString text = portName(a2600session_gamepad_effective_port(m_session, idx));
        if (m_padEff[(size_t)idx]->text() != text) m_padEff[(size_t)idx]->setText(text);
    }
}

/* One row per pad: its name, the port it plays and the one it is set to. */
void ControllersWindow::rebuildPads()
{
    for (QWidget *row : m_padRows) delete row;
    m_padRows.clear();
    m_padEff.clear();
    const int count = a2600session_gamepad_count(m_session);
    if (count == 0) {
        auto *none = new QLabel(QStringLiteral("No gamepads connected - plug one in"));
        none->setStyleSheet(QStringLiteral("color: gray;"));
        m_padBox->addWidget(none);
        m_padRows.push_back(none);
        return;
    }
    for (int idx = 0; idx < count && idx < kMaxPadRows; ++idx) {
        char name[128];
        a2600session_gamepad_name(m_session, idx, name, sizeof name);
        auto *row = new QWidget;
        auto *line = new QHBoxLayout(row);
        line->setContentsMargins(0, 0, 0, 0);
        auto *label = new ElidedLabel(false);
        label->setText(local(name));
        label->setToolTip(local(name));
        auto *eff = new QLabel;
        eff->setStyleSheet(QStringLiteral("color: gray;"));
        /* -1 = automatic, 0 = the left port, 1 = the right */
        auto *box = new QComboBox;
        box->setFocusPolicy(Qt::NoFocus);
        box->addItem(QStringLiteral("Automatic"));
        box->addItem(QStringLiteral("Left"));
        box->addItem(QStringLiteral("Right"));
        const int assign = a2600session_gamepad_assignment(m_session, idx);
        box->setCurrentIndex(assign < 0 || assign > 1 ? 0 : assign + 1);
        connect(box, &QComboBox::currentIndexChanged, this, [this, idx](int sel) {
            a2600session_gamepad_assign(m_session, idx, sel <= 0 ? -1 : sel - 1);
            refreshPadPorts();
        });
        line->addWidget(label, 1);
        line->addWidget(eff);
        line->addWidget(box);
        m_padBox->addWidget(row);
        m_padRows.push_back(row);
        m_padEff.push_back(eff);
    }
    if (count > kMaxPadRows) {
        /* the rest are in Preferences ▸ Input */
        m_padEff.back()->setToolTip(QStringLiteral("%1 more gamepads are listed in Preferences").arg(count - kMaxPadRows));
    }
    refreshPadPorts();
}

/* ---- the window ------------------------------------------------------------------------- */

void ControllersWindow::present()
{
    m_padGen = a2600session_gamepad_generation(m_session);
    syncPlugs();
    rebuildPads();
    show();
    raise();
}

void ControllersWindow::refresh()
{
    syncPlugs();
    if (isVisible()) {
        letGo();
        m_stage->pollHeld();
    }
}

void ControllersWindow::showEvent(QShowEvent *evt)
{
    m_stage->pollHeld();
    m_heldTimer.start(30);
    m_slowTimer.start(250);
    QWidget::showEvent(evt);
}

void ControllersWindow::hideEvent(QHideEvent *evt)
{
    m_heldTimer.stop();
    m_slowTimer.stop();
    setMapState(-2);
    letGo();
    QWidget::hideEvent(evt);
}

/* What this window holds, and the keyboard's holds too: the main window releases
 * its own keys when it loses activation, not when this one gains it. */
void ControllersWindow::changeEvent(QEvent *evt)
{
    if (evt->type() == QEvent::ActivationChange && !isActiveWindow()) {
        letGo();
        a2600session_release_all(m_session);
        m_swallow = 0;
    }
    QWidget::changeEvent(evt);
}

void ControllersWindow::keyPressEvent(QKeyEvent *evt)
{
    if (evt->key() == Qt::Key_F9) {
        if (!evt->isAutoRepeat()) hide();
        return;
    }
    if (evt->key() == Qt::Key_F12) {
        if (!evt->isAutoRepeat()) DebuggerWindow::toggleFor(parentWidget(), m_session);
        return;
    }
    if (evt->key() == Qt::Key_F11) return;          /* full screen belongs to the main window */
    const uint32_t keysym = a2600KeysymFromQt(evt);
    if (m_swallow && keysym == m_swallow) return;

    if (m_mapState != -2) {
        if (evt->isAutoRepeat()) return;
        /* Esc backs out one step: a pending target, then Map itself */
        if (evt->key() == Qt::Key_Escape) {
            m_swallow = keysym;
            setMapState(m_mapState >= 0 ? -1 : -2);
            return;
        }
        if (m_mapState < 0 || !keysym) return;      /* armed, or nothing to bind: nothing is typed */
        if (evt->modifiers() & Qt::ControlModifier) {
            m_hint->setText(QStringLiteral("Ctrl chords belong to the app: they cannot be mapped."));
            return;
        }
        if (a2600session_key_reserved(keysym)) {
            m_hint->setText(QStringLiteral("F9, F11 and F12 belong to the app: they cannot be mapped."));
            return;
        }
        char name[48] = "", stolen[128] = "";
        const int target = m_mapState;
        a2600session_binding_set_key(m_session, target, keysym, stolen, sizeof stolen);
        if (a2600session_binding_get(m_session, target).keysym != keysym) {
            m_hint->setText(QStringLiteral("That key cannot be mapped."));
            return;
        }
        a2600session_keysym_name(keysym, name, sizeof name);
        m_swallow = keysym;
        bound(target, local(name), local(stolen));
        return;
    }
    /* Otherwise what the main window does with it: the session's bindings first (a
     * console action goes to the main window's queue, as a gamepad's does). */
    if (!a2600ForwardKey(m_session, evt, true)) QWidget::keyPressEvent(evt);
}

/* A release always goes to the session, in Map mode too: it only lets go of
 * what a press asserted, so a key held while Map was armed cannot stick. */
void ControllersWindow::keyReleaseEvent(QKeyEvent *evt)
{
    if (evt->isAutoRepeat()) return;
    const uint32_t keysym = a2600KeysymFromQt(evt);
    if (m_swallow && keysym == m_swallow) {
        m_swallow = 0;
        return;
    }
    if (!a2600ForwardKey(m_session, evt, false)) QWidget::keyReleaseEvent(evt);
}

QPointer<ControllersWindow> g_window;

} // namespace

void a2600ControllersToggle(QWidget *parent, a2600session *session)
{
    if (!g_window) g_window = new ControllersWindow(session, parent);
    if (g_window->isVisible()) g_window->hide();
    else g_window->present();
}

void a2600ControllersShow(QWidget *parent, a2600session *session)
{
    if (!g_window) g_window = new ControllersWindow(session, parent);
    if (!g_window->isVisible()) g_window->present();
    else g_window->raise();
}

bool a2600ControllersVisible()
{
    return g_window && g_window->isVisible();
}

bool a2600ControllersActive()
{
    return g_window && g_window->isActiveWindow();
}

void a2600ControllersRefresh()
{
    if (g_window) g_window->refresh();
}
