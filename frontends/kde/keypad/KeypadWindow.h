/*
 * The Keypads window: both keyboard controllers side by side, the console
 * switches, and Map mode for rebinding any control to a key or a gamepad
 * button.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QWidget>
#include <vector>

#include "a2600session.h"

/* A button that reports press and release, not "clicked": a keypad key is
 * HELD, since the machine samples it once per frame. */
class PadButton : public QPushButton {
    Q_OBJECT
public:
    PadButton(const QString &face, int target, QWidget *parent = nullptr);
    int target() const { return m_target; }
    QString face() const { return m_face; }
    void setHeld(bool held);
signals:
    void pressedTarget(int target);
    void releasedTarget(int target);
protected:
    void mousePressEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *e) override;
private:
    int m_target;
    QString m_face;
    bool m_down = false;
};

class KeypadWindow : public QWidget {
    Q_OBJECT
public:
    explicit KeypadWindow(a2600session *session, QWidget *parent = nullptr);

protected:
    void keyPressEvent(QKeyEvent *e) override;
    void keyReleaseEvent(QKeyEvent *e) override;
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;

private:
    QWidget *buildController(int port);
    PadButton *control(const QString &face, int target, bool wide);
    void onPressed(int target);
    void onReleased(int target);
    void setMapState(int state);
    void refreshLabels();
    void pollCapture();
    void refreshTypes();

    a2600session *m_session;
    std::vector<PadButton *> m_controls;
    QPushButton *m_mapButton = nullptr;
    QLabel *m_hint = nullptr;
    QLabel *m_typeLabel[2] = { nullptr, nullptr };
    QTimer m_captureTimer;
    QTimer m_typeTimer;
    /* -2 idle, -1 armed and waiting for a target, >=0 waiting for a key or
     * gamepad button. */
    int m_mapState = -2;
};
