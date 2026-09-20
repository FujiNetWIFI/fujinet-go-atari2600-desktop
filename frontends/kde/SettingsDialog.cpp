/*
 * SettingsDialog -- see SettingsDialog.h.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "SettingsDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>
#include <vector>

namespace {

QCheckBox *check(const char *text, const char *tip, a2600session *s, const char *key, int def)
{
    auto *b = new QCheckBox(QString::fromUtf8(text));
    b->setToolTip(QString::fromUtf8(tip));
    b->setChecked(a2600session_get_int(s, key, def) != 0);
    return b;
}

bool commit(a2600session *s, const char *key, int def, int value)
{
    if (a2600session_get_int(s, key, def) == value) return false;
    a2600session_set_int(s, key, value);
    return true;
}

QString portSubtitle(a2600session *s, int port)
{
    if (a2600session_port_type(s, port) == A2600_CTRL_AUTO)
        return QStringLiteral("Auto: Stella attached %1")
            .arg(QString::fromUtf8(a2600_ctrl_type_name(a2600session_detected_port_type(s, port))));
    return QStringLiteral("Forced for every cartridge");
}

} // namespace

bool SettingsDialog::run(QWidget *parent, a2600session *session)
{
    QDialog dlg(parent);
    dlg.setWindowTitle(QStringLiteral("Preferences"));
    dlg.setMinimumWidth(520);
    auto *outer = new QVBoxLayout(&dlg);

    /* Machine (restart) */
    auto *machine = new QGroupBox(QStringLiteral("Machine (applied by restarting the session)"));
    auto *mform = new QFormLayout(machine);
    auto *tv = new QComboBox;
    for (int i = 0; a2600_tv_format_name(i); ++i) tv->addItem(QString::fromUtf8(a2600_tv_format_name(i)));
    tv->setCurrentIndex(a2600session_get_int(session, "tv_format", A2600_TV_AUTO));
    tv->setToolTip(QStringLiteral("Auto lets each cartridge decide (frame layout detection)"));
    mform->addRow(QStringLiteral("TV format"), tv);
    outer->addWidget(machine);

    /* Controllers (live) */
    auto *ports = new QGroupBox(QStringLiteral("Controllers (applied immediately, even to a game booted over FujiNet)"));
    auto *pform = new QFormLayout(ports);
    QComboBox *portBox[2];
    QLabel *portNote[2];
    for (int port = 0; port < 2; ++port) {
        portBox[port] = new QComboBox;
        for (int i = 0; a2600_ctrl_type_name(i); ++i)
            portBox[port]->addItem(QString::fromUtf8(a2600_ctrl_type_name(i)));
        portBox[port]->setCurrentIndex(a2600session_port_type(session, port));
        portNote[port] = new QLabel(portSubtitle(session, port));
        portNote[port]->setStyleSheet(QStringLiteral("color: gray;"));
        auto *row = new QVBoxLayout;
        row->addWidget(portBox[port]);
        row->addWidget(portNote[port]);
        pform->addRow(port ? QStringLiteral("Right port") : QStringLiteral("Left port"), row);
        QObject::connect(portBox[port], &QComboBox::currentIndexChanged, &dlg, [=](int idx) {
            a2600session_set_port_type(session, port, idx);
            portNote[port]->setText(portSubtitle(session, port));
        });
    }
    outer->addWidget(ports);

    auto *analog = new QGroupBox(QStringLiteral("Analog sticks drive"));
    auto *aform = new QVBoxLayout(analog);
    QCheckBox *aj = check("Joystick", "Left stick as the four directions", session, "analog_joystick", 1);
    QCheckBox *ap = check("Paddles", "Left stick X is paddle A, right stick X is paddle B", session, "analog_paddle", 1);
    QCheckBox *ad = check("Driving controller", "Left stick X as the wheel", session, "analog_driving", 1);
    auto applyAnalog = [=] {
        a2600session_set_analog(session, aj->isChecked(), ap->isChecked(), ad->isChecked());
    };
    QObject::connect(aj, &QCheckBox::toggled, &dlg, applyAnalog);
    QObject::connect(ap, &QCheckBox::toggled, &dlg, applyAnalog);
    QObject::connect(ad, &QCheckBox::toggled, &dlg, applyAnalog);
    aform->addWidget(aj); aform->addWidget(ap); aform->addWidget(ad);
    outer->addWidget(analog);

    /* Gamepads: one row per pad, refreshed as they come and go */
    auto *pads = new QGroupBox(QStringLiteral("Gamepads (assigned to ports in connection order unless chosen here)"));
    auto *padForm = new QFormLayout(pads);
    std::vector<QWidget *> padRows;
    unsigned seenGen = a2600session_gamepad_generation(session) + 1;
    auto rebuildPads = [&, padForm, session]() mutable {
        for (QWidget *w : padRows) { padForm->removeRow(w); }
        padRows.clear();
        const int n = a2600session_gamepad_count(session);
        if (n == 0) {
            auto *l = new QLabel(QStringLiteral("No gamepads connected — plug one in, it is picked up as it appears"));
            padForm->addRow(l);
            padRows.push_back(l);
            return;
        }
        for (int i = 0; i < n && i < 8; ++i) {
            char name[128];
            a2600session_gamepad_name(session, i, name, sizeof name);
            auto *box = new QComboBox;
            box->addItems({ QStringLiteral("Automatic"), QStringLiteral("Left port"), QStringLiteral("Right port") });
            box->setCurrentIndex(a2600session_gamepad_assignment(session, i) + 1);
            const int eff = a2600session_gamepad_effective_port(session, i);
            box->setToolTip(QStringLiteral("Driving the %1 port").arg(eff == 0 ? "left" : eff == 1 ? "right" : "no"));
            QObject::connect(box, &QComboBox::currentIndexChanged, box, [=](int idx) {
                a2600session_gamepad_assign(session, i, idx - 1);
            });
            padForm->addRow(QString::fromUtf8(name), box);
            padRows.push_back(box);
        }
    };
    auto *padTimer = new QTimer(&dlg);
    QObject::connect(padTimer, &QTimer::timeout, &dlg, [&, session]() mutable {
        const unsigned gen = a2600session_gamepad_generation(session);
        if (gen != seenGen) { seenGen = gen; rebuildPads(); }
        for (int port = 0; port < 2; ++port) portNote[port]->setText(portSubtitle(session, port));
    });
    padTimer->start(1000);
    seenGen = a2600session_gamepad_generation(session);
    rebuildPads();
    outer->addWidget(pads);

    /* Audio (live) */
    auto *audio = new QGroupBox(QStringLiteral("Audio"));
    auto *audioForm = new QFormLayout(audio);
    auto *volume = new QSlider(Qt::Horizontal);
    volume->setRange(0, 100);
    volume->setValue(a2600session_get_int(session, "volume", 100));
    QObject::connect(volume, &QSlider::valueChanged, &dlg, [=](int v) { a2600session_set_volume(session, v); });
    audioForm->addRow(QStringLiteral("Volume"), volume);
    outer->addWidget(audio);

    /* Host (restart) */
    auto *host = new QGroupBox(QStringLiteral("Host (applied by restarting the session)"));
    auto *hform = new QVBoxLayout(host);
    QCheckBox *fuji = check("Enable FujiNet", "Run the in-process FujiNet the cartridge dials into. Off means no network and a link-down CONFIG client.", session, "enable_fujinet", 1);
    QCheckBox *snd = check("Audio", "Open the system audio device", session, "enable_audio", 1);
    QCheckBox *gp = check("Gamepads", "Poll USB/Bluetooth gamepads", session, "enable_gamepad", 1);
    hform->addWidget(fuji); hform->addWidget(snd); hform->addWidget(gp);
    outer->addWidget(host);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    outer->addWidget(buttons);

    if (dlg.exec() != QDialog::Accepted)
        return false;

    bool changed = false;
    changed |= commit(session, "tv_format", A2600_TV_AUTO, tv->currentIndex());
    changed |= commit(session, "enable_fujinet", 1, fuji->isChecked() ? 1 : 0);
    changed |= commit(session, "enable_audio", 1, snd->isChecked() ? 1 : 0);
    changed |= commit(session, "enable_gamepad", 1, gp->isChecked() ? 1 : 0);
    return changed;
}
