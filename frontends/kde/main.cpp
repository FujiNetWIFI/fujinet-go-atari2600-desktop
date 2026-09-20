/*
 * FujiNet Go Atari 2600 -- the KDE (Qt6 Widgets) frontend.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QApplication>
#include <QIcon>

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

    MainWindow win(session);

    a2600session_start_opts opts;
    a2600session_default_opts(session, &opts);
    if (argc > 1) opts.cart_path = argv[1];

    if (a2600session_start(session, &opts) != 0)
        qWarning("%s", a2600session_last_error(session));
    win.show();

    const int rc = app.exec();
    a2600session_stop(session);
    a2600session_free(session);
    return rc;
}
