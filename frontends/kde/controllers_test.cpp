/*
 * controllers_test -- the KDE Controllers window, offscreen: hidden at start,
 * fixed size (and the same size whatever is plugged in), keys steering the
 * joystick, the mouse holding the fire button and dragging the stick (and letting
 * go when it drags off, and when the window hides), the paddles' knobs turned by
 * a drag and the wheel, a paddle's button and turn arrow, the driving wheel, the
 * keypad's keys, the console's switches (a latching one flips, select follows the
 * press, power and stop post their actions on the release), Map taking a key and
 * a gamepad button (and refusing F9 and Ctrl chords), Clear, Defaults and the
 * target list, F9 hiding it, and a pad's row with its port picker -- on an SDL
 * virtual gamepad, with no machine. Waits poll with a five second ceiling, so a
 * slow runner is merely slow. SKIPs (77) where SDL has no virtual joysticks.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <QWheelEvent>
#include <QWidget>

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "a2600session.h"
#include "controllers/ControllersWindow.h"

/* the session library's pad layer, which a session starts with a machine: here it runs alone */
extern "C" int gamepad_start(a2600session *session);
extern "C" void gamepad_stop(a2600session *session);

namespace {

using a2600ctl::kMargin;
using a2600ctl::kUnit;

int failures = 0;

void check(bool pass, const char *what)
{
    std::printf("%s: %s\n", pass ? "ok" : "FAIL", what);
    if (!pass) failures++;
}

constexpr unsigned bit(int act) { return 1u << act; }

/* Waits until port's held mask has (or has not) every bit of `mask`; the window's own timers and the
 * pad thread both need the event loop. */
void expectHeld(a2600session *sess, int port, unsigned mask, bool want, const char *what)
{
    for (int tries = 0; tries < 500; tries++) {   // 10 s: slow runners
        if (((a2600session_buttons_held(sess, port) & mask) == mask) == want) {
            std::printf("ok: %s\n", what);
            return;
        }
        QTest::qWait(20);
    }
    std::printf("FAIL: %s\n", what);
    failures++;
}

QWidget *findWindow()
{
    for (QWidget *top : QApplication::topLevelWidgets())
        if (top->objectName() == QLatin1String("controllers")) return top;
    return nullptr;
}

QWidget *child(QWidget *win, const char *name)
{
    return win->findChild<QWidget *>(QString::fromLatin1(name), Qt::FindChildrenRecursively);
}

QPushButton *findButton(QWidget *win, const char *text)
{
    for (QPushButton *button : win->findChildren<QPushButton *>())
        if (button->text() == QLatin1String(text)) return button;
    return nullptr;
}

/* the controller types on screen, as the window sees them: the layout follows them */
const a2600_control *layoutOf(a2600session *sess, int *n)
{
    return a2600session_controls_layout(a2600session_view_type(sess, 0), a2600session_view_type(sess, 1), n, nullptr, nullptr);
}

/* the centre, in stage pixels, of the part of this kind and label (port -1: any) */
QPoint centreOf(a2600session *sess, int kind, const char *label, int port, int nth = 0)
{
    int n = 0;
    const a2600_control *lay = layoutOf(sess, &n);
    for (int idx = 0; idx < n; idx++) {
        if (lay[idx].kind != kind) continue;
        if (label && (!lay[idx].label || std::strcmp(lay[idx].label, label) != 0)) continue;
        if (port >= 0 && lay[idx].port != port) continue;
        if (nth-- > 0) continue;
        return QPoint(int(kMargin + (lay[idx].x + lay[idx].w / 2) * kUnit),
                      int(kMargin + (lay[idx].y + lay[idx].h / 2) * kUnit));
    }
    return QPoint(-1, -1);
}

/* the part driving this target (a button, a turn arrow, a switch) */
QPoint centreOfTarget(a2600session *sess, int target)
{
    int n = 0;
    const a2600_control *lay = layoutOf(sess, &n);
    for (int idx = 0; idx < n; idx++)
        if (lay[idx].target == target)
            return QPoint(int(kMargin + (lay[idx].x + lay[idx].w / 2) * kUnit),
                          int(kMargin + (lay[idx].y + lay[idx].h / 2) * kUnit));
    return QPoint(-1, -1);
}

/* a plugged-in change is noticed by the window's own timer */
void plug(a2600session *sess, int port, int type)
{
    a2600session_set_port_type(sess, port, type);
    QTest::qWait(120);
}

QComboBox *waitForPadRow(QWidget *win, const char *padName)
{
    for (int tries = 0; tries < 500; tries++) {   // 10 s: slow runners
        for (QComboBox *box : win->findChildren<QComboBox *>())
            if (box->count() == 3 && box->itemText(0) == QLatin1String("Automatic") &&
                box->itemText(1) == QLatin1String("Left")) {
                (void)padName;
                return box;
            }
        QTest::qWait(20);
    }
    return nullptr;
}

constexpr int P0 = 0, P1 = 1;
constexpr int FIRE0 = A2600_TARGET_PORT(P0, A2600_ACT_JOY_FIRE);

void keyTests(a2600session *sess, QWidget *win)
{
    /* the arrow keys steer the left joystick (the default) */
    QTest::keyPress(win, Qt::Key_Up);
    expectHeld(sess, P0, bit(A2600_ACT_JOY_UP), true, "a typed Up steers the left stick up");
    QTest::keyRelease(win, Qt::Key_Up);
    expectHeld(sess, P0, bit(A2600_ACT_JOY_UP), false, "and its release lets go");
    QTest::keyPress(win, Qt::Key_Space);
    expectHeld(sess, P0, bit(A2600_ACT_JOY_FIRE), true, "Space is the left fire button");
    QTest::keyRelease(win, Qt::Key_Space);
    expectHeld(sess, P0, bit(A2600_ACT_JOY_FIRE), false, "and its release");
}

void joystickTests(a2600session *sess, QWidget *win)
{
    QWidget *stage = child(win, "stage");
    check(stage != nullptr, "the stage widget exists");
    if (!stage) return;
    const QSize fixed = win->size();

    const QPoint fire = centreOf(sess, A2600_CTL_ROUND_BUTTON, "FIRE", P0);
    QTest::mousePress(stage, Qt::LeftButton, Qt::NoModifier, fire);
    expectHeld(sess, P0, bit(A2600_ACT_JOY_FIRE), true, "pressing the fire button holds it");
    QTest::mouseMove(stage, QPoint(2, 2));
    expectHeld(sess, P0, bit(A2600_ACT_JOY_FIRE), false, "dragging off lets it go");
    QTest::mouseMove(stage, fire);
    expectHeld(sess, P0, bit(A2600_ACT_JOY_FIRE), true, "and back on holds it again");
    QTest::mouseRelease(stage, Qt::LeftButton, Qt::NoModifier, fire);
    expectHeld(sess, P0, bit(A2600_ACT_JOY_FIRE), false, "releasing lets go");

    /* the stick follows the pointer: eight sectors of the ring */
    const QPoint stick = centreOf(sess, A2600_CTL_STICK, "STICK", P0);
    const int reach = int(1.6 * kUnit);
    QTest::mousePress(stage, Qt::LeftButton, Qt::NoModifier, stick + QPoint(reach, 0));
    expectHeld(sess, P0, bit(A2600_ACT_JOY_RIGHT), true, "a press right of the shaft tilts the stick right");
    QTest::mouseMove(stage, stick + QPoint(-reach, 0));
    expectHeld(sess, P0, bit(A2600_ACT_JOY_LEFT), true, "dragging to the left tilts it left");
    expectHeld(sess, P0, bit(A2600_ACT_JOY_RIGHT), false, "and lets go of right");
    QTest::mouseMove(stage, stick + QPoint(int(reach * 0.75), -int(reach * 0.75)));
    expectHeld(sess, P0, bit(A2600_ACT_JOY_UP) | bit(A2600_ACT_JOY_RIGHT), true, "the upper right is a diagonal");
    QTest::mouseMove(stage, stick + QPoint(3, 3));
    expectHeld(sess, P0, bit(A2600_ACT_JOY_UP) | bit(A2600_ACT_JOY_RIGHT) | bit(A2600_ACT_JOY_LEFT) |
                             bit(A2600_ACT_JOY_DOWN), false, "a quarter of the reach is dead centre: nothing");
    QTest::mouseMove(stage, stick + QPoint(0, -reach));
    expectHeld(sess, P0, bit(A2600_ACT_JOY_UP), true, "and up is up");
    win->hide();
    expectHeld(sess, P0, bit(A2600_ACT_JOY_UP), false, "hiding the window lets go of what the mouse held");
    QTest::mouseRelease(stage, Qt::LeftButton, Qt::NoModifier, stick);
    win->show();
    check(win->size() == fixed, "the window is the same size after all that");
}

void consoleTests(a2600session *sess, QWidget *win)
{
    QWidget *stage = child(win, "stage");
    if (!stage) return;

    /* a latching switch flips on the press */
    const int before = a2600session_switch_get(sess, A2600_SW_COLOR_BW);
    const QPoint tv = centreOfTarget(sess, A2600_TARGET_SWITCH(A2600_SW_COLOR_BW));
    QTest::mouseClick(stage, Qt::LeftButton, Qt::NoModifier, tv);
    check(a2600session_switch_get(sess, A2600_SW_COLOR_BW) == !before, "clicking TV type flips it");
    QTest::mouseClick(stage, Qt::LeftButton, Qt::NoModifier, tv);
    check(a2600session_switch_get(sess, A2600_SW_COLOR_BW) == before, "and again flips it back");
    const int diff = a2600session_switch_get(sess, A2600_SW_LEFT_DIFF);
    QTest::mouseClick(stage, Qt::LeftButton, Qt::NoModifier, centreOfTarget(sess, A2600_TARGET_SWITCH(A2600_SW_LEFT_DIFF)));
    check(a2600session_switch_get(sess, A2600_SW_LEFT_DIFF) == !diff, "the left difficulty flips");
    QTest::mouseClick(stage, Qt::LeftButton, Qt::NoModifier, centreOfTarget(sess, A2600_TARGET_SWITCH(A2600_SW_LEFT_DIFF)));

    /* select follows the press */
    const QPoint select = centreOfTarget(sess, A2600_TARGET_SWITCH(A2600_SW_SELECT));
    QTest::mousePress(stage, Qt::LeftButton, Qt::NoModifier, select);
    check(a2600session_switch_held(sess, A2600_SW_SELECT) != 0, "game select is held while the mouse is down");
    QTest::mouseRelease(stage, Qt::LeftButton, Qt::NoModifier, select);
    check(a2600session_switch_held(sess, A2600_SW_SELECT) == 0, "and let go on the release");

    /* power and stop post their action on the release, like a real button */
    int sa = -1;
    while (a2600session_sysaction_take(sess, &sa)) {}
    const QPoint power = centreOfTarget(sess, A2600_TARGET_SYSACT(A2600_SYSACT_REBOOT_CONFIG));
    QTest::mousePress(stage, Qt::LeftButton, Qt::NoModifier, power);
    check(!a2600session_sysaction_take(sess, &sa), "power does nothing on the press");
    QTest::mouseRelease(stage, Qt::LeftButton, Qt::NoModifier, power);
    check(a2600session_sysaction_take(sess, &sa) && sa == A2600_SYSACT_REBOOT_CONFIG, "and posts Reboot to CONFIG on the release");
    const QPoint stop = centreOfTarget(sess, A2600_TARGET_SYSACT(A2600_SYSACT_PAUSE));
    QTest::mousePress(stage, Qt::LeftButton, Qt::NoModifier, stop);
    QTest::mouseMove(stage, QPoint(2, 2));
    QTest::mouseRelease(stage, Qt::LeftButton, Qt::NoModifier, QPoint(2, 2));
    check(!a2600session_sysaction_take(sess, &sa), "dragging STOP off cancels it");
    QTest::mouseClick(stage, Qt::LeftButton, Qt::NoModifier, stop);
    check(a2600session_sysaction_take(sess, &sa) && sa == A2600_SYSACT_PAUSE, "a click on STOP posts Pause");
}

void paddleTests(a2600session *sess, QWidget *win)
{
    QWidget *stage = child(win, "stage");
    if (!stage) return;
    const QSize fixed = win->size();
    plug(sess, P0, A2600_CTRL_PADDLES);
    check(a2600session_view_type(sess, P0) == A2600_CTRL_PADDLES, "the left port now shows paddles");
    check(win->size() == fixed && win->minimumSize() == win->maximumSize(), "and the window did not change size");

    const QPoint dialA = centreOf(sess, A2600_CTL_KNOB, "KNOB", P0, 0);
    const int up = int(1.2 * kUnit), diag = int(1.1 * kUnit);
    QTest::mousePress(stage, Qt::LeftButton, Qt::NoModifier, dialA + QPoint(0, -up));
    check(std::abs(a2600session_analog_get(sess, P0, 0)) < 1500, "a press straight up centres the knob");
    QTest::mouseMove(stage, dialA + QPoint(diag, diag));
    check(a2600session_analog_get(sess, P0, 0) > 30000, "dragging to the lower right turns it full clockwise");
    QTest::mouseMove(stage, dialA + QPoint(-diag, diag));
    check(a2600session_analog_get(sess, P0, 0) < -30000, "and to the lower left full counter-clockwise");
    QTest::mouseRelease(stage, Qt::LeftButton, Qt::NoModifier, dialA + QPoint(-diag, diag));
    check(a2600session_analog_get(sess, P0, 0) < -30000, "a knob holds where it was left");

    /* the second paddle: the mouse wheel nudges it */
    const QPoint dialB = centreOf(sess, A2600_CTL_KNOB, "KNOB", P0, 1);
    a2600session_pointer_analog(sess, P0, 1, 0, 1);
    a2600session_pointer_analog(sess, P0, 1, 0, 0);
    QWheelEvent wheel(QPointF(dialB), QPointF(stage->mapToGlobal(dialB)), QPoint(), QPoint(0, 120), Qt::NoButton,
                      Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(stage, &wheel);
    const int nudged = a2600session_analog_get(sess, P0, 1);
    check(nudged > 1500 && nudged < 2600, "a wheel notch nudges paddle B's knob");

    /* paddle A's button and turn arrow */
    const QPoint fireA = centreOf(sess, A2600_CTL_BUTTON, "FIRE", P0, 0);
    QTest::mousePress(stage, Qt::LeftButton, Qt::NoModifier, fireA);
    expectHeld(sess, P0, bit(A2600_ACT_PADDLE_A_FIRE), true, "paddle A's red button is held by the mouse");
    QTest::mouseRelease(stage, Qt::LeftButton, Qt::NoModifier, fireA);
    expectHeld(sess, P0, bit(A2600_ACT_PADDLE_A_FIRE), false, "and let go");
    const QPoint turnRight = centreOf(sess, A2600_CTL_TURN, ">", P0, 0);
    QTest::mousePress(stage, Qt::LeftButton, Qt::NoModifier, turnRight);
    expectHeld(sess, P0, bit(A2600_ACT_PADDLE_A_INC), true, "paddle A's right arrow holds its turn");
    QTest::mouseRelease(stage, Qt::LeftButton, Qt::NoModifier, turnRight);
    expectHeld(sess, P0, bit(A2600_ACT_PADDLE_A_INC), false, "and lets go");
    plug(sess, P0, A2600_CTRL_JOYSTICK);
}

void drivingAndKeypadTests(a2600session *sess, QWidget *win)
{
    QWidget *stage = child(win, "stage");
    if (!stage) return;
    plug(sess, P1, A2600_CTRL_DRIVING);
    check(a2600session_view_type(sess, P1) == A2600_CTRL_DRIVING, "the right port shows the driving controller");

    const QPoint wheelAt = centreOf(sess, A2600_CTL_WHEEL, "WHEEL", P1);
    QWheelEvent notch(QPointF(wheelAt), QPointF(stage->mapToGlobal(wheelAt)), QPoint(), QPoint(0, 120), Qt::NoButton,
                      Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(stage, &notch);
    /* No machine runs here, and the core only pulses a notch into a running one
     * (input_pulse), so what is checked is that the window takes the event. The
     * twist itself is covered by the core's input tests. */
    check(notch.isAccepted(), "a mouse-wheel notch over the driving wheel is taken");

    /* dragging the wheel round twists once per 22.5 degrees (the pulses are inert here too) */
    const int r = int(1.4 * kUnit);
    QTest::mousePress(stage, Qt::LeftButton, Qt::NoModifier, wheelAt + QPoint(0, -r));
    QTest::mouseMove(stage, wheelAt + QPoint(int(r * 0.9), -int(r * 0.4)));
    QTest::mouseMove(stage, wheelAt + QPoint(r, 0));
    QTest::mouseRelease(stage, Qt::LeftButton, Qt::NoModifier, wheelAt + QPoint(r, 0));

    const QPoint fireD = centreOfTarget(sess, A2600_TARGET_PORT(P1, A2600_ACT_DRIVE_FIRE));
    QTest::mousePress(stage, Qt::LeftButton, Qt::NoModifier, fireD);
    expectHeld(sess, P1, bit(A2600_ACT_DRIVE_FIRE), true, "the driving controller's button is held");
    QTest::mouseRelease(stage, Qt::LeftButton, Qt::NoModifier, fireD);
    expectHeld(sess, P1, bit(A2600_ACT_DRIVE_FIRE), false, "and let go");

    plug(sess, P1, A2600_CTRL_KEYPAD);
    check(a2600session_view_type(sess, P1) == A2600_CTRL_KEYPAD, "the right port shows the keypad");
    const QPoint five = centreOf(sess, A2600_CTL_ROUND_BUTTON, "5", P1);
    QTest::mousePress(stage, Qt::LeftButton, Qt::NoModifier, five);
    expectHeld(sess, P1, bit(A2600_ACT_KEY_5), true, "the 5 key is held while the mouse is down");
    QTest::mouseRelease(stage, Qt::LeftButton, Qt::NoModifier, five);
    expectHeld(sess, P1, bit(A2600_ACT_KEY_5), false, "and let go");
    const QPoint pound = centreOf(sess, A2600_CTL_ROUND_BUTTON, "#", P1);
    QTest::mousePress(stage, Qt::LeftButton, Qt::NoModifier, pound);
    expectHeld(sess, P1, bit(A2600_ACT_KEY_POUND), true, "and the # key");
    QTest::mouseRelease(stage, Qt::LeftButton, Qt::NoModifier, pound);
    plug(sess, P1, A2600_CTRL_AUTO);
}

void mapTests(a2600session *sess, QWidget *win, SDL_Joystick *vjoy)
{
    QPushButton *mapButton = findButton(win, "Map");
    QWidget *stage = child(win, "stage");
    check(mapButton && stage, "the Map button exists");
    if (!mapButton || !stage) return;

    mapButton->click();
    check(mapButton->text() == QLatin1String("Done"), "Map arms");
    QTest::mouseClick(stage, Qt::LeftButton, Qt::NoModifier, centreOf(sess, A2600_CTL_ROUND_BUTTON, "FIRE", P0));
    QTest::keyClick(win, Qt::Key_Q);
    check(a2600session_binding_get(sess, FIRE0).keysym == 'q', "the next key is bound to the picked control");
    expectHeld(sess, P0, bit(A2600_ACT_JOY_FIRE), false, "a key taken by Map is not typed into the machine");

    /* a side of the stick's ring */
    const QPoint stick = centreOf(sess, A2600_CTL_STICK, "STICK", P0);
    QTest::mouseClick(stage, Qt::LeftButton, Qt::NoModifier, stick + QPoint(-int(1.7 * kUnit), 0));
    QTest::keyClick(win, Qt::Key_Z);
    check(a2600session_binding_get(sess, A2600_TARGET_PORT(P0, A2600_ACT_JOY_LEFT)).keysym == 'z',
          "clicking the left of the ring picks Left");

    /* F9, F11, F12 and Ctrl chords cannot be bound */
    QTest::mouseClick(stage, Qt::LeftButton, Qt::NoModifier, centreOf(sess, A2600_CTL_ROUND_BUTTON, "FIRE", P0));
    QTest::keyClick(win, Qt::Key_F11);
    check(a2600session_binding_get(sess, FIRE0).keysym == 'q', "F11 is refused");
    QTest::keyClick(win, Qt::Key_B, Qt::ControlModifier);
    check(a2600session_binding_get(sess, FIRE0).keysym == 'q', "a Ctrl chord is refused");
    check(a2600session_key_reserved(A2600_KEYSYM_F9) && a2600session_key_reserved(A2600_KEYSYM_F12), "F9 and F12 are reserved");

    /* Clear takes the key and the pad button away */
    QPushButton *clear = findButton(win, "Clear");
    check(clear && clear->isEnabled(), "Clear is enabled with a control picked");
    if (clear) clear->click();
    const a2600_binding cleared = a2600session_binding_get(sess, FIRE0);
    check(cleared.keysym == 0 && cleared.button == A2600_PAD_BTN_NONE, "Clear unbinds the key and the button");

    /* the list reaches a control the picture is not showing: the right port's Up */
    auto *list = win->findChild<QComboBox *>(QStringLiteral("targetList"));
    check(list && list->isEnabled() && list->count() == A2600_TARGET_COUNT + 1, "the target list holds all 59 controls");
    if (list) {
        const int rightUp = A2600_TARGET_PORT(P1, A2600_ACT_JOY_UP);
        for (int idx = 1; idx < list->count(); idx++)
            if (list->itemData(idx).toInt() == rightUp) {
                emit list->activated(idx);
                break;
            }
        QTest::keyClick(win, Qt::Key_K);
        check(a2600session_binding_get(sess, rightUp).keysym == 'k', "a control picked from the list takes the next key");
    }

    /* a gamepad button */
    QTest::mouseClick(stage, Qt::LeftButton, Qt::NoModifier, centreOf(sess, A2600_CTL_ROUND_BUTTON, "FIRE", P0));
    if (vjoy) {
        SDL_SetJoystickVirtualButton(vjoy, SDL_GAMEPAD_BUTTON_RIGHT_STICK, true);
        for (int tries = 0; tries < 250 && a2600session_binding_get(sess, FIRE0).button != A2600_PAD_BTN_RIGHT_STICK; tries++)
            QTest::qWait(20);
        SDL_SetJoystickVirtualButton(vjoy, SDL_GAMEPAD_BUTTON_RIGHT_STICK, false);
        check(a2600session_binding_get(sess, FIRE0).button == A2600_PAD_BTN_RIGHT_STICK,
              "the next gamepad button is bound to the picked control");
    }
    QTest::keyClick(win, Qt::Key_Escape);
    QTest::keyClick(win, Qt::Key_Escape);
    check(mapButton->text() == QLatin1String("Map"), "Escape twice leaves Map");

    a2600session_bindings_reset(sess);
    check(a2600session_binding_get(sess, FIRE0).keysym == A2600_KEYSYM_SPACE, "Defaults put the key back");

    /* Show bindings: a check box, remembered */
    auto *show = win->findChild<QCheckBox *>(QStringLiteral("showBindings"));
    check(show != nullptr && !show->isChecked(), "Show bindings starts off");
    if (show) {
        show->click();
        check(a2600session_get_int(sess, "controllers_show_bindings", 0) == 1, "and is remembered");
        show->click();
    }
}

void padRowTests(a2600session *sess, QWidget *win, const char *padName)
{
    QComboBox *box = waitForPadRow(win, padName);
    check(box != nullptr, "a pad's row appears with Automatic, Left and Right");
    if (!box) return;
    int slot = -1;
    for (int idx = 0; idx < a2600session_gamepad_count(sess); idx++) {
        char got[64];
        a2600session_gamepad_name(sess, idx, got, sizeof got);
        if (std::strcmp(got, padName) == 0) slot = idx;
    }
    check(slot >= 0, "the pad is in the session");
    if (slot < 0) return;
    check(box->currentIndex() == 0, "it starts on Automatic");
    box->setCurrentIndex(2);
    check(a2600session_gamepad_assignment(sess, slot) == 1, "choosing Right assigns it");
    check(a2600session_gamepad_effective_port(sess, slot) == 1, "and it plays the right port");
    box->setCurrentIndex(0);
    check(a2600session_gamepad_assignment(sess, slot) == -1, "Automatic clears the assignment");
}

} // namespace

int main(int argc, char **argv)
{
    QTemporaryDir config, data;
    if (!config.isValid() || !data.isValid()) return 1;
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        std::printf("SKIP: SDL gamepad subsystem: %s\n", SDL_GetError());
        return 77;
    }
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    static const char padName[] = "a2600test kde pad";
    desc.name = padName;
    const SDL_JoystickID vid = SDL_AttachVirtualJoystick(&desc);
    if (!vid) {
        std::printf("SKIP: no virtual joysticks here: %s\n", SDL_GetError());
        return 77;
    }
    SDL_Joystick *vjoy = SDL_OpenJoystick(vid);

    const QByteArray cfg = config.path().toLocal8Bit(), dat = data.path().toLocal8Bit();
    a2600session_paths paths;
    std::memset(&paths, 0, sizeof paths);
    paths.config_dir = cfg.constData();
    paths.data_dir = dat.constData();
    paths.fujinet_lib = "";      /* never the user's real FujiNet */
    a2600session *sess = a2600session_new(&paths);
    if (sess) a2600session_set_analog(sess, 1, 1, 1);
    /* the pad layer, which the session starts with a machine: here it runs alone */
    if (!sess || gamepad_start(sess) != 0) {
        std::printf("SKIP: the gamepad layer would not start\n");
        return 77;
    }

    QWidget host;
    check(!a2600ControllersVisible(), "hidden at start");
    a2600ControllersToggle(&host, sess);
    check(a2600ControllersVisible(), "F9 / the menu shows it");
    QWidget *win = findWindow();
    check(win != nullptr, "the window exists");
    if (win) {
        check(win->windowTitle() == QLatin1String("Controllers"), "titled Controllers");
        check(win->minimumSize() == win->maximumSize() && win->minimumSize() == win->size(),
              "fixed size: minimum == maximum == size");
        keyTests(sess, win);
        joystickTests(sess, win);
        consoleTests(sess, win);
        paddleTests(sess, win);
        drivingAndKeypadTests(sess, win);
        win->show();
        mapTests(sess, win, vjoy);
        padRowTests(sess, win, padName);
        QTest::keyClick(win, Qt::Key_F9);
        check(!win->isVisible(), "F9 hides the window");
        a2600ControllersToggle(&host, sess);
        check(a2600ControllersVisible(), "and the menu brings it back");
        a2600ControllersToggle(&host, sess);
        check(!a2600ControllersVisible(), "toggling again hides it");
    }

    gamepad_stop(sess);
    if (vjoy) SDL_CloseJoystick(vjoy);
    SDL_DetachVirtualJoystick(vid);
    a2600session_free(sess);
    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
