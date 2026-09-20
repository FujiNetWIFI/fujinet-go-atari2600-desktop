/*
 * The main window.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QLabel>
#include <QMainWindow>
#include <QTimer>

#include "a2600session.h"

class DisplayWidget;
class KeypadWindow;
class QAction;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(a2600session *session, QWidget *parent = nullptr);

protected:
    void keyPressEvent(QKeyEvent *e) override;
    void keyReleaseEvent(QKeyEvent *e) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dropEvent(QDropEvent *e) override;
    void closeEvent(QCloseEvent *e) override;
    bool event(QEvent *e) override;

private:
    void buildMenus();
    void updateStatus();
    void drainSysactions();
    void runSysaction(int sa);
    void openCart(const QString &path);
    void loadMedia(const QString &path);
    void toggleKeypad();
    void showSettings();
    void restartSession();
    void showAbout();

    a2600session *m_session;
    DisplayWidget *m_display = nullptr;
    KeypadWindow *m_keypad = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_dot = nullptr;
    QTimer m_statusTimer;
    QTimer m_sysactTimer;
    QAction *m_colorAction = nullptr;
    QAction *m_leftDiffAction = nullptr;
    QAction *m_rightDiffAction = nullptr;
    bool m_sysactDown[A2600_SYSACT_COUNT] = {};
};
