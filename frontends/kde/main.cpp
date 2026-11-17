/*
 * FujiNet Go Atari 2600 -- the KDE (Qt6 Widgets) frontend.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QApplication>
#include <QIcon>

#include "Accent.h"
#include "MainWindow.h"
#include "a2600session.h"

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("fujinet-go-atari2600-kde"));
    app.setApplicationDisplayName(QStringLiteral("FujiNet Go Atari 2600"));
    app.setDesktopFileName(QStringLiteral("online.fujinet.go.atari2600.kde"));
    /* The installed icon is named after the desktop-entry id; running out
     * of the build tree, the in-tree artwork stands in. */
    {
        QIcon icon = QIcon::fromTheme(QStringLiteral("online.fujinet.go.atari2600.kde"));
        if (icon.isNull()) {
            QIcon::setThemeSearchPaths(QIcon::themeSearchPaths()
                                       << QStringLiteral(A2600_SOURCE_ICON_DIR));
            icon = QIcon::fromTheme(QStringLiteral("fujinet-go-atari2600"));
        }
        if (!icon.isNull()) app.setWindowIcon(icon);
    }

    a2600session *session = a2600session_new(nullptr);
    if (!session) {
        qCritical("Could not create the session (unusable config/data dirs?)");
        return 1;
    }

    /* Toggled-on push buttons (Map while armed) wear the accent colour with dark
     * text, light or dark. The rule spells out the whole box: with only a
     * background colour the style still draws its own bevel, which darkens it
     * (Breeze), lightens it (Fusion) or ignores it (Kvantum). */
    const QColor accent = a2600CtlAccent();
    app.setStyleSheet(
        QStringLiteral(
            "QPushButton[accent=\"true\"] { background-color: %1; color: %4;"
            " border: 1px solid %3; border-radius: 4px; padding: 4px 12px; }"
            "QPushButton[accent=\"true\"]:hover { background-color: %2; }")
            .arg(accent.name(), accent.lighter(112).name(), accent.darker(140).name(), a2600CtlAccentInk().name()));

    int rc;
    {
        /* The windows go before the session does: the Controllers window still
         * calls into it as it is hidden. */
        MainWindow win(session);

        a2600session_start_opts opts;
        a2600session_default_opts(session, &opts);
        if (argc > 1) opts.cart_path = argv[1];

        if (a2600session_start(session, &opts) != 0)
            qWarning("%s", a2600session_last_error(session));
        win.show();

        rc = app.exec();
    }
    a2600session_stop(session);
    a2600session_free(session);
    return rc;
}
