// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the USBeam contributors
//
// usbeam_app.cpp - the USBeam app for Linux: tray icon + a native Qt window
// (KDE Plasma's toolkit) showing USBeam's screen, served by the USBeam service
// on 127.0.0.1:3242. No browser is opened.
//
//   usbeam-app          open the window (or bring the running one forward)
//   usbeam-app --tray   start hidden in the tray (used at login)

#include <QApplication>
#include <QCloseEvent>
#include <QFile>
#include <QIcon>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMainWindow>
#include <QMenu>
#include <QPalette>
#include <QScreen>
#include <QStyleHints>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QUrl>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineView>
#include <unistd.h>

static const char *TOKEN_FILE = "/run/usbeam/ui-token";
static const int UI_PORT = 3242;

static QString readToken(bool *denied) {
    QFile f(TOKEN_FILE);
    *denied = false;
    if (!f.exists()) return {};
    if (!f.open(QIODevice::ReadOnly)) { *denied = true; return {}; }
    return QString::fromLatin1(f.readAll()).trimmed();
}

static bool darkTheme() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    if (QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark) return true;
    if (QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Light) return false;
#endif
    return QGuiApplication::palette().color(QPalette::Window).lightness() < 128;
}

class Window : public QMainWindow {
public:
    QWebEngineView *view;
    QString token;

    Window() {
        setWindowTitle("USBeam");
        setWindowIcon(QIcon::fromTheme("usbeam"));
        setMinimumSize(560, 420);
        QSize sz(1100, 760);
        if (QScreen *s = QGuiApplication::primaryScreen()) sz = sz.boundedTo(s->availableGeometry().size());
        resize(sz);
        view = new QWebEngineView(this);
        view->setContextMenuPolicy(Qt::NoContextMenu);
        view->page()->setBackgroundColor(darkTheme() ? QColor(0x0f, 0x12, 0x17) : QColor(0xf4, 0xf5, 0xf7));
        view->settings()->setAttribute(QWebEngineSettings::FocusOnNavigationEnabled, true);
        setCentralWidget(view);
        load();
        auto *t = new QTimer(this);              // the service restarted: reload with its new key
        connect(t, &QTimer::timeout, this, [this] {
            bool denied;
            if (isVisible() && readToken(&denied) != token) load();
        });
        t->start(3000);
    }

    void load() {
        bool denied;
        token = readToken(&denied);
        if (token.isEmpty()) {
            QString msg = denied
                ? "Your user account isn't allowed to control USBeam. Only administrators (the <i>wheel</i> group) can."
                : "Waiting for the USBeam service. If this doesn't go away, reinstall USBeam.";
            QString bg = darkTheme() ? "#0f1217" : "#f4f5f7", fg = darkTheme() ? "#e6e9ef" : "#161a21";
            view->setHtml(QString("<html><body style='margin:0;height:100vh;display:flex;flex-direction:column;align-items:center;"
                                  "justify-content:center;gap:8px;font-family:sans-serif;background:%1;color:%2'>"
                                  "<b style='font-size:17px'>USBeam is starting…</b><span style='opacity:.7'>%3</span></body></html>")
                              .arg(bg, fg, msg));
            return;
        }
        view->load(QUrl(QString("http://127.0.0.1:%1/?t=%2&theme=%3").arg(UI_PORT).arg(token, darkTheme() ? "dark" : "light")));
    }

    void showUp() {
        bool denied;
        if (readToken(&denied) != token) load();
        show();
        setWindowState((windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
        raise();
        activateWindow();
    }

protected:
    void closeEvent(QCloseEvent *e) override {   // closing keeps USBeam in the tray
        hide();
        e->ignore();
    }
};

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setApplicationName("USBeam");
    app.setApplicationDisplayName("USBeam");
    app.setDesktopFileName("usbeam");           // matches usbeam.desktop: icon + taskbar grouping
    app.setQuitOnLastWindowClosed(false);
    bool trayOnly = app.arguments().contains("--tray");

    // One copy per user: a second start just brings the running window forward.
    QString sockName = QString("usbeam-app-%1").arg(getuid());
    {
        QLocalSocket probe;
        probe.connectToServer(sockName);
        if (probe.waitForConnected(500)) {
            if (!trayOnly) { probe.write("show"); probe.flush(); probe.waitForBytesWritten(500); }
            return 0;
        }
    }
    QLocalServer::removeServer(sockName);
    QLocalServer server;
    server.listen(sockName);

    Window win;
    QObject::connect(&server, &QLocalServer::newConnection, [&] {
        QLocalSocket *c = server.nextPendingConnection();
        QObject::connect(c, &QLocalSocket::readyRead, [&win, c] { c->readAll(); win.showUp(); c->deleteLater(); });
    });

    QSystemTrayIcon tray(QIcon::fromTheme("usbeam"));
    tray.setToolTip("USBeam");
    QMenu menu;
    menu.addAction("Open USBeam", [&] { win.showUp(); });
    menu.addSeparator();
    menu.addAction("Close this app (sharing keeps running)", [&] { app.quit(); });
    tray.setContextMenu(&menu);
    QObject::connect(&tray, &QSystemTrayIcon::activated, [&](QSystemTrayIcon::ActivationReason r) {
        if (r == QSystemTrayIcon::Trigger || r == QSystemTrayIcon::DoubleClick) {
            if (win.isVisible() && win.isActiveWindow()) win.hide(); else win.showUp();
        }
    });
    tray.show();

    if (!trayOnly) win.showUp();
    return app.exec();
}
