/*
 * MainWindow -- see MainWindow.h.
 *
 * Plain Qt6 Widgets, deliberately not KDE Frameworks: it picks up Breeze
 * through the platform theme anyway, and staying framework-free keeps this
 * frontend usable outside a KDE session.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "MainWindow.h"

#include <QApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QStatusBar>
#include <QUrl>

#include "DisplayWidget.h"
#include "FujiNetWindows.h"
#include "KeyForward.h"
#include "SettingsDialog.h"
#include "debugger/DebuggerWindow.h"
#include "keypad/KeypadWindow.h"

static const char *const kCartFilter =
    "Atari 2600 cartridges (*.a26 *.bin *.rom *.fuji);;All files (*)";

MainWindow::MainWindow(a2600session *session, QWidget *parent)
    : QMainWindow(parent), m_session(session)
{
    setWindowTitle(QStringLiteral("FujiNet Go Atari 2600"));
    /* 320x240 at 3x. */
    resize(960, 720 + 60);
    setAcceptDrops(true);

    m_display = new DisplayWidget(session, this);
    m_display->setTvAspect(a2600session_get_int(session, "tv_aspect", 1) != 0);
    m_display->setSmooth(a2600session_get_int(session, "smooth", 0) != 0);
    setCentralWidget(m_display);

    m_dot = new QLabel(QStringLiteral("●"));
    m_status = new QLabel(QStringLiteral("Starting..."));
    statusBar()->addWidget(m_dot);
    statusBar()->addWidget(m_status);

    buildMenus();

    connect(&m_statusTimer, &QTimer::timeout, this, &MainWindow::updateStatus);
    m_statusTimer.start(1000);
    updateStatus();
    /* The gamepad thread cannot call into Qt; it posts system actions and
     * this timer takes them. */
    connect(&m_sysactTimer, &QTimer::timeout, this, &MainWindow::drainSysactions);
    m_sysactTimer.start(100);

    if (qEnvironmentVariableIsSet("A2600_OPEN_KEYPAD"))
        toggleKeypad();
    if (qEnvironmentVariableIsSet("A2600_OPEN_DEBUGGER"))
        DebuggerWindow::showFor(this, session);
}

void MainWindow::buildMenus()
{
    QMenu *machine = menuBar()->addMenu(QStringLiteral("&Machine"));
    machine->addAction(QStringLiteral("&Open Cartridge..."), QKeySequence(Qt::CTRL | Qt::Key_O), this, [this] {
        const QString f = QFileDialog::getOpenFileName(
            this, QStringLiteral("Open Cartridge"), QString(), QString::fromUtf8(kCartFilter));
        if (!f.isEmpty()) openCart(f);
    });
    machine->addAction(QStringLiteral("&Eject Cartridge"), this, [this] {
        a2600session_eject(m_session);
        statusBar()->showMessage(QStringLiteral("Cartridge ejected — back to CONFIG"), 4000);
    });
    machine->addAction(QStringLiteral("&Import Cartridge to SD..."), this, [this] {
        const QString f = QFileDialog::getOpenFileName(
            this, QStringLiteral("Import Cartridge to SD"), QString(), QString::fromUtf8(kCartFilter));
        if (f.isEmpty()) return;
        char dest[1024];
        if (a2600session_import_cart_to_sd(m_session, f.toLocal8Bit().constData(), dest, sizeof dest) != 0)
            QMessageBox::warning(this, QStringLiteral("Import failed"),
                                 QString::fromUtf8(a2600session_last_error(m_session)));
        else
            statusBar()->showMessage(QStringLiteral("%1 is on the SD host — boot it from the CONFIG client")
                                     .arg(QFileInfo(QString::fromUtf8(dest)).fileName()), 6000);
    });
    machine->addAction(QStringLiteral("Reboot to &CONFIG"), QKeySequence(Qt::CTRL | Qt::Key_R), this,
                       [this] { runSysaction(A2600_SYSACT_REBOOT_CONFIG); });
    machine->addSeparator();
    machine->addAction(QStringLiteral("&Select"), this,
                       [this] { a2600session_switch_pulse(m_session, A2600_SW_SELECT); });
    machine->addAction(QStringLiteral("&Reset"), this,
                       [this] { a2600session_switch_pulse(m_session, A2600_SW_RESET); });
    m_colorAction = machine->addAction(QStringLiteral("Colo&r (B&&W when off)"));
    m_colorAction->setCheckable(true);
    m_colorAction->setChecked(true);
    connect(m_colorAction, &QAction::toggled, this,
            [this](bool on) { a2600session_switch_set(m_session, A2600_SW_COLOR_BW, on); });
    m_leftDiffAction = machine->addAction(QStringLiteral("Left Difficulty &A"), QKeySequence(Qt::ALT | Qt::Key_L));
    m_leftDiffAction->setCheckable(true);
    connect(m_leftDiffAction, &QAction::toggled, this,
            [this](bool on) { a2600session_switch_set(m_session, A2600_SW_LEFT_DIFF, on); });
    m_rightDiffAction = machine->addAction(QStringLiteral("Right Difficulty A"), QKeySequence(Qt::ALT | Qt::Key_R));
    m_rightDiffAction->setCheckable(true);
    connect(m_rightDiffAction, &QAction::toggled, this,
            [this](bool on) { a2600session_switch_set(m_session, A2600_SW_RIGHT_DIFF, on); });
    machine->addSeparator();
    machine->addAction(QStringLiteral("&Preferences..."), QKeySequence(Qt::CTRL | Qt::Key_Comma), this,
                       &MainWindow::showSettings);
    machine->addSeparator();
    machine->addAction(QStringLiteral("&Quit"), QKeySequence::Quit, this, [this] { close(); });

    QMenu *view = menuBar()->addMenu(QStringLiteral("&View"));
    view->addAction(QStringLiteral("&Keypads"), QKeySequence(Qt::Key_F9), this, &MainWindow::toggleKeypad);
    view->addAction(QStringLiteral("&Debugger"), QKeySequence(Qt::Key_F12), this,
                    [this] { DebuggerWindow::showFor(this, m_session); });
    view->addSeparator();
    QAction *tv = view->addAction(QStringLiteral("&TV Aspect (4:3)"));
    tv->setCheckable(true);
    tv->setChecked(a2600session_get_int(m_session, "tv_aspect", 1) != 0);
    connect(tv, &QAction::toggled, this, [this](bool on) {
        m_display->setTvAspect(on);
        a2600session_set_int(m_session, "tv_aspect", on ? 1 : 0);
    });
    QAction *sm = view->addAction(QStringLiteral("&Smooth Scaling"));
    sm->setCheckable(true);
    sm->setChecked(a2600session_get_int(m_session, "smooth", 0) != 0);
    connect(sm, &QAction::toggled, this, [this](bool on) {
        m_display->setSmooth(on);
        a2600session_set_int(m_session, "smooth", on ? 1 : 0);
    });
    view->addAction(QStringLiteral("&Fullscreen"), QKeySequence(Qt::Key_F11), this, [this] {
        if (isFullScreen()) showNormal(); else showFullScreen();
    });

    QMenu *fuji = menuBar()->addMenu(QStringLiteral("&FujiNet"));
    fuji->addAction(QStringLiteral("&Configuration"), this, [this] {
        if (!a2600session_fujinet_running(m_session)) {
            statusBar()->showMessage(QStringLiteral("FujiNet is not running"), 4000);
            return;
        }
        fujinet_config_show(this, m_session);
    });
    fuji->addAction(QStringLiteral("Console &Log"), this, [this] { fujinet_log_show(this, m_session); });

    QMenu *help = menuBar()->addMenu(QStringLiteral("&Help"));
    help->addAction(QStringLiteral("&About FujiNet Go Atari 2600"), this, &MainWindow::showAbout);
}

void MainWindow::showAbout()
{
    QMessageBox::about(this, QStringLiteral("About FujiNet Go Atari 2600"),
        QStringLiteral("<b>FujiNet Go Atari 2600</b> %1<br><br>"
                       "An Atari 2600 with a built-in FujiNet.<br>"
                       "The emulator is Stella (GPL-2.0-or-later) by Bradford W. Mott, "
                       "Stephen Anthony and the Stella Team, with the FujiNet cartridge.<br><br>"
                       "Copyright © 2026 Thomas Cherryhomes — GPL-3.0-or-later<br>"
                       "<a href=\"https://fujinet.online/\">fujinet.online</a>")
            .arg(QStringLiteral(A2600_VERSION_STRING)));
}

void MainWindow::showSettings()
{
    if (SettingsDialog::run(this, m_session))
        restartSession();
}

void MainWindow::restartSession()
{
    a2600session_start_opts o;
    a2600session_settings_flush(m_session);
    a2600session_default_opts(m_session, &o);
    a2600session_stop(m_session);
    if (a2600session_start(m_session, &o) != 0) {
        QMessageBox::warning(this, QStringLiteral("Restart failed"),
                             QString::fromUtf8(a2600session_last_error(m_session)));
        return;
    }
    statusBar()->showMessage(QStringLiteral("Machine options applied (session restarted)"), 5000);
}

void MainWindow::toggleKeypad()
{
    if (!m_keypad) m_keypad = new KeypadWindow(m_session, this);
    if (m_keypad->isVisible()) m_keypad->hide();
    else m_keypad->show();
}

void MainWindow::runSysaction(int sa)
{
    switch (sa) {
    case A2600_SYSACT_REBOOT_CONFIG:
        a2600session_sysaction(m_session, sa);
        statusBar()->showMessage(QStringLiteral("Back to the FujiNet CONFIG client"), 4000);
        break;
    case A2600_SYSACT_PAUSE:
        DebuggerWindow::showFor(this, m_session);
        a2600session_sysaction(m_session, sa);
        break;
    default: break;
    }
}

void MainWindow::drainSysactions()
{
    int sa;
    while (a2600session_sysaction_take(m_session, &sa)) runSysaction(sa);
}

void MainWindow::updateStatus()
{
    QString text;
    bool on = false;
    if (!a2600session_is_running(m_session)) {
        text = QStringLiteral("Stopped");
    } else if (a2600session_cart_link_up(m_session) < 0) {
        text = QStringLiteral("Local cartridge: %1")
                   .arg(QFileInfo(QString::fromUtf8(a2600session_cart_path(m_session))).fileName());
    } else if (a2600session_cart_booted_game(m_session)) {
        text = QStringLiteral("FujiNet: booted a game"); on = true;
    } else if (a2600session_cart_link_up(m_session) == 1) {
        text = QStringLiteral("FujiNet connected"); on = true;
    } else {
        char st[128];
        a2600session_cart_status(m_session, st, sizeof st);
        text = QStringLiteral("FujiNet: %1").arg(QString::fromUtf8(st));
    }
    m_status->setText(text);
    m_dot->setStyleSheet(on ? QStringLiteral("color: %1;").arg(a2600AccentColor().name())
                            : QStringLiteral("color: gray;"));
}

void MainWindow::openCart(const QString &path)
{
    if (a2600session_load_cart(m_session, path.toLocal8Bit().constData()) != 0) {
        QMessageBox::warning(this, QStringLiteral("Could not open"),
                             QString::fromUtf8(a2600session_last_error(m_session)));
        return;
    }
    statusBar()->showMessage(QStringLiteral("Running %1").arg(QFileInfo(path).fileName()), 4000);
}

void MainWindow::loadMedia(const QString &path)
{
    if (a2600session_media_is_cartridge(path.toLocal8Bit().constData())) {
        openCart(path);
        return;
    }
    char dest[1024];
    if (a2600session_import_media(m_session, path.toLocal8Bit().constData(), dest, sizeof dest) != 0) {
        QMessageBox::warning(this, QStringLiteral("Import failed"),
                             QString::fromUtf8(a2600session_last_error(m_session)));
        return;
    }
    statusBar()->showMessage(QStringLiteral("Copied to FujiNet's SD folder — mount it from the CONFIG client"), 6000);
}

void MainWindow::keyPressEvent(QKeyEvent *e)
{
    if (e->isAutoRepeat()) return;
    if (e->key() == Qt::Key_F9) { toggleKeypad(); return; }
    if (e->key() == Qt::Key_F12) { DebuggerWindow::showFor(this, m_session); return; }
    if (e->modifiers() & (Qt::ControlModifier | Qt::AltModifier)) { QMainWindow::keyPressEvent(e); return; }

    const uint32_t ks = a2600KeysymFromQt(e);
    if (!ks) { QMainWindow::keyPressEvent(e); return; }

    const int sa = a2600session_key_sysaction(m_session, ks);
    if (sa >= 0) {
        if (!m_sysactDown[sa]) { m_sysactDown[sa] = true; runSysaction(sa); }
        return;
    }
    if (!a2600session_key(m_session, ks, 1)) QMainWindow::keyPressEvent(e);
}

void MainWindow::keyReleaseEvent(QKeyEvent *e)
{
    if (e->isAutoRepeat()) return;
    const uint32_t ks = a2600KeysymFromQt(e);
    if (!ks) { QMainWindow::keyReleaseEvent(e); return; }
    const int sa = a2600session_key_sysaction(m_session, ks);
    if (sa >= 0) { m_sysactDown[sa] = false; return; }
    if (!a2600session_key(m_session, ks, 0)) QMainWindow::keyReleaseEvent(e);
}

bool MainWindow::event(QEvent *e)
{
    if (e->type() == QEvent::WindowDeactivate) {
        a2600session_release_all(m_session);
        for (bool &d : m_sysactDown) d = false;
    }
    return QMainWindow::event(e);
}

void MainWindow::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasUrls()) e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *e)
{
    const QList<QUrl> urls = e->mimeData()->urls();
    if (urls.isEmpty()) return;
    const QString path = urls.first().toLocalFile();
    if (!path.isEmpty()) loadMedia(path);
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    m_statusTimer.stop();
    m_sysactTimer.stop();
    QMainWindow::closeEvent(e);
}
