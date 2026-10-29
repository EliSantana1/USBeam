// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the USBeam contributors
//
// usbeam_app.cpp - the USBeam app for Linux: tray icon + a native Qt window showing USBeam's
// screen, served by the USBeam service on 127.0.0.1:3242. No browser is opened.
//
//   usbeam-app            open the window (or bring the running one forward)
//   usbeam-app --tray     start hidden in the tray (used at sign-in)
//   usbeam-app --install  (AppImage) install or update without asking first (used by updates)
//
// When started from an AppImage that isn't installed yet, it offers to install USBeam for all
// users (through the normal password prompt), using setup.sh from inside the AppImage.

#include <QApplication>
#include <QCloseEvent>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPalette>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QStandardPaths>
#include <QStyleHints>
#include <QSystemTrayIcon>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QWebEngineDownloadRequest>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineView>
#include <unistd.h>
#include <limits.h>
#include <cstring>
#include <functional>
#include "../../common/version.h"

static const char *TOKEN_FILE = "/run/usbeam/ui-token";
static const int UI_PORT = 3242;
static const char *PREFIX = "/opt/usbeam";
static const char *INSTALLED_IMAGE = "/opt/usbeam/USBeam.AppImage";
static const char *PROJECT = "https://github.com/EliSantana1/USBeam";
static const char *DOWNLOADS = "https://github.com/EliSantana1/USBeam/releases/download/";
static bool g_quitting;

static QString readToken(bool *denied) {
    QFile f(TOKEN_FILE);
    *denied = false;
    if (!f.exists()) return {};
    if (!f.open(QIODevice::ReadOnly)) { *denied = true; return {}; }
    return QString::fromLatin1(f.readAll()).trimmed();
}

// Start the USBeam service if it isn't running (after Quit, Restart or a crash). USBeam's
// polkit rule lets administrators start and stop this one service without a password.
static void ensureService() {
    QProcess::startDetached("systemctl", { "start", "--no-ask-password", "usbeam.service" });
}

static bool darkTheme() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    if (QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark) return true;
    if (QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Light) return false;
#endif
    return QGuiApplication::palette().color(QPalette::Window).lightness() < 128;
}

static QIcon appIcon() {
    QIcon i = QIcon::fromTheme("usbeam");
    if (!i.isNull()) return i;
    // Inside the AppImage (before it's installed): the icon that comes with it.
    QString p = QCoreApplication::applicationDirPath() + "/../share/icons/hicolor/scalable/apps/usbeam.svg";
    return QFile::exists(p) ? QIcon(p) : QIcon();
}

// How USBeam is installed on this computer.
static QString installKind() {
    if (QFile::exists(QString(PREFIX) + "/installed")) return "appimage";
    if (QFile::exists("/usr/lib/systemd/system/usbeam.service")) return "package";
    return "none";
}

static bool newerVersion(const QString &a, const QString &b) {      // is a newer than b?
    const QStringList pa = a.split('.'), pb = b.split('.');
    for (int i = 0; i < qMax(pa.size(), pb.size()); i++) {
        int x = i < pa.size() ? pa[i].toInt() : 0, y = i < pb.size() ? pb[i].toInt() : 0;
        if (x != y) return x > y;
    }
    return false;
}

// ---------------------------------------------------------------- setup (AppImage)
// Root can't read inside an AppImage that a user has open, so the files setup.sh needs are
// copied to a temporary folder first. Returns "" on success, or what went wrong.
static QString runSetup(const QString &image) {
    const QString appdir = qEnvironmentVariable("APPDIR");
    QTemporaryDir tmp;
    if (appdir.isEmpty() || !tmp.isValid()) return "USBeam's files could not be prepared.";
    const char *files[] = { "usbeam", "setup.sh", "usbeam.service", "usbeam.rules", "usbeam-modules.conf",
                            "usbeam.desktop", "usbeam-autostart.desktop", "usbeam.svg" };
    for (const char *f : files) {
        QString from = appdir + "/usr/lib/usbeam/" + f, to = tmp.path() + "/" + f;
        if (!QFile::copy(from, to)) return QString("Missing file inside the AppImage: %1").arg(f);
        QFile::setPermissions(to, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::ReadGroup | QFile::ExeGroup | QFile::ReadOther | QFile::ExeOther);
    }
    QFile v(tmp.path() + "/version");
    if (v.open(QIODevice::WriteOnly)) { v.write(USBEAM_VERSION "\n"); v.close(); }
    QFile::setPermissions(tmp.path(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::ReadGroup | QFile::ExeGroup | QFile::ReadOther | QFile::ExeOther);

    QProcess p;
    // Through /bin/sh: some systems don't allow running programs from the temporary folder.
    p.start("pkexec", { "/bin/sh", tmp.path() + "/setup.sh", "--install", tmp.path(), image });
    if (!p.waitForStarted(10000)) return "The password prompt (pkexec) could not be started.";
    p.waitForFinished(300000);
    if (p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0) return "";
    switch (p.exitCode()) {
    case 126: case 127: return "Installation was cancelled.";
    case 3: return "USBeam is already installed as a package on this computer. Remove that package first.";
    case 4: return "This Linux uses no systemd, which USBeam needs for its background service.";
    default: return QString("Installation failed (%1). %2").arg(p.exitCode()).arg(QString::fromLocal8Bit(p.readAllStandardError()).trimmed());
    }
}

// Started from an AppImage file: install it, update the installed copy, or switch to it.
// Returns -1 to carry on starting the app here, otherwise the exit code.
static int appImageStart(bool forceInstall, const QStringList &args) {
    const QString image = qEnvironmentVariable("APPIMAGE");
    if (image.isEmpty() || QFileInfo(image).canonicalFilePath() == INSTALLED_IMAGE) return -1;
    const QString kind = installKind();
    if (kind == "package") {
        QMessageBox::warning(nullptr, "USBeam", "USBeam is already installed as a package on this computer.\n\n"
                                                "Open USBeam from your application menu, or remove the package before using this AppImage.");
        return 1;
    }
    QString installed;
    QFile f(QString(PREFIX) + "/installed");
    if (f.open(QIODevice::ReadOnly)) installed = QString::fromLatin1(f.readAll()).trimmed();
    const bool update = kind == "appimage";
    if (update && !forceInstall && !newerVersion(USBEAM_VERSION, installed)) {
        QProcess::startDetached(INSTALLED_IMAGE, args);              // already installed: use that copy
        return 0;
    }
    if (!forceInstall) {
        QMessageBox box(QMessageBox::Question, "USBeam",
            update ? QString("Update USBeam from %1 to %2?").arg(installed, USBEAM_VERSION) : QString("Install USBeam %1?").arg(USBEAM_VERSION),
            QMessageBox::NoButton);
        box.setInformativeText(update
            ? "You'll be asked for your password."
            : "USBeam will be installed for everyone on this computer, in /opt/usbeam (this file moves there). "
              "It starts with the computer, shows its icon in the tray, and adds itself to your application menu.\n\n"
              "You'll be asked for your password.");
        QPushButton *ok = box.addButton(update ? "Update" : "Install", QMessageBox::AcceptRole);
        box.addButton(QMessageBox::Cancel);
        box.setWindowIcon(appIcon());
        box.exec();
        if (box.clickedButton() != ok) return 0;
    }
    QString err = runSetup(image);
    if (!err.isEmpty()) { QMessageBox::warning(nullptr, "USBeam", err); return 1; }
    QProcess::startDetached(INSTALLED_IMAGE, {});
    return 0;
}

// ---------------------------------------------------------------- the window
class Page : public QWebEnginePage {
public:
    std::function<void(const QUrl &)> onAppLink;
    using QWebEnginePage::QWebEnginePage;
protected:
    // Links "usbeam-app:..." are requests from the page to this app (open a link, update, uninstall).
    bool acceptNavigationRequest(const QUrl &url, NavigationType type, bool isMainFrame) override {
        if (url.scheme() == "usbeam-app") { if (onAppLink) onAppLink(url); return false; }
        return QWebEnginePage::acceptNavigationRequest(url, type, isMainFrame);
    }
};

class Window : public QMainWindow {
public:
    QWebEngineView *view;
    Page *page;
    QString token;

    Window() {
        setWindowTitle("USBeam");
        setWindowIcon(appIcon());
        setMinimumSize(560, 420);
        QSize sz(1100, 760);
        if (QScreen *s = QGuiApplication::primaryScreen()) sz = sz.boundedTo(s->availableGeometry().size());
        resize(sz);
        view = new QWebEngineView(this);
        page = new Page(view);
        view->setPage(page);
        view->setContextMenuPolicy(Qt::NoContextMenu);
        page->setBackgroundColor(darkTheme() ? QColor(0x0f, 0x12, 0x17) : QColor(0xf4, 0xf5, 0xf7));
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
                ? "Your user account isn't allowed to control USBeam. Only administrators can."
                : "Waiting for the USBeam service. If this doesn't go away, reinstall USBeam.";
            QString bg = darkTheme() ? "#0f1217" : "#f4f5f7", fg = darkTheme() ? "#e6e9ef" : "#161a21";
            view->setHtml(QString("<html><body style='margin:0;height:100vh;display:flex;flex-direction:column;align-items:center;"
                                  "justify-content:center;gap:8px;font-family:sans-serif;background:%1;color:%2'>"
                                  "<b style='font-size:17px'>USBeam is starting…</b><span style='opacity:.7'>%3</span></body></html>")
                              .arg(bg, fg, msg));
            return;
        }
        const QString display = QGuiApplication::platformName().startsWith("wayland") ? "wayland" : QGuiApplication::platformName() == "xcb" ? "x11" : "";
        view->load(QUrl(QString("http://127.0.0.1:%1/?t=%2&theme=%3&display=%4").arg(UI_PORT).arg(token, darkTheme() ? "dark" : "light", display)));
    }

    void showUp() {
        bool denied;
        if (readToken(&denied) != token) load();
        show();
        setWindowState((windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
        raise();
        activateWindow();
    }

    // Tell the page how an app task is going (shown in the gear menu).
    void report(const QString &text) {
        QString js = QString("window.appEvent && window.appEvent({text: %1})").arg(jsString(text));
        page->runJavaScript(js);
    }
    static QString jsString(const QString &s) {
        QString o = "\"";
        for (QChar c : s) { if (c == '"' || c == '\\') o += '\\'; if (c == '\n') { o += "\\n"; continue; } o += c; }
        return o + "\"";
    }

protected:
    void closeEvent(QCloseEvent *e) override {   // closing keeps USBeam in the tray
        hide();
        e->ignore();
    }
};

// Remove what this user's copy of the app keeps (web cache), after uninstalling.
static void removeUserData() {
    for (auto loc : { QStandardPaths::AppDataLocation, QStandardPaths::CacheLocation, QStandardPaths::AppLocalDataLocation }) {
        QString d = QStandardPaths::writableLocation(loc);
        if (!d.isEmpty() && d.contains("USBeam", Qt::CaseInsensitive)) QDir(d).removeRecursively();
    }
}

int main(int argc, char **argv) {
    // Inside the AppImage, Chromium's own sandbox (used by the window's web view) often can't start
    // (it needs features some distributions restrict). The window only shows USBeam's own local page.
    if (!qEnvironmentVariableIsEmpty("APPIMAGE")) {
        if (qEnvironmentVariableIsEmpty("QTWEBENGINE_DISABLE_SANDBOX")) qputenv("QTWEBENGINE_DISABLE_SANDBOX", "1");
        // Draw the window without the graphics card: Qt inside the AppImage can't always use the
        // computer's own graphics drivers, and USBeam's simple page doesn't need them.
        if (qEnvironmentVariableIsEmpty("QT_QUICK_BACKEND")) qputenv("QT_QUICK_BACKEND", "software");
        // AppImageLauncher (installed on some systems): don't offer to "integrate" USBeam,
        // it installs itself. Also applies to the copies of USBeam this one starts.
        qputenv("APPIMAGELAUNCHER_DISABLE", "1");
    }
    // Wayland first; X11 only on desktops without Wayland.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM") &&
        (!qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY") || qEnvironmentVariable("XDG_SESSION_TYPE") == "wayland"))
        qputenv("QT_QPA_PLATFORM", "wayland;xcb");
    QApplication app(argc, argv);
    app.setApplicationName("USBeam");
    app.setApplicationDisplayName("USBeam");
    app.setDesktopFileName("usbeam");           // matches usbeam.desktop: icon + taskbar grouping
    app.setQuitOnLastWindowClosed(false);
    const bool trayOnly = app.arguments().contains("--tray");
    const bool forceInstall = app.arguments().contains("--install");
    QStringList fwd; if (trayOnly) fwd << "--tray";

    int r = appImageStart(forceInstall, fwd);
    if (r >= 0) return r;

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

    ensureService();
    Window win;
    QObject::connect(&server, &QLocalServer::newConnection, [&] {
        QLocalSocket *c = server.nextPendingConnection();
        QObject::connect(c, &QLocalSocket::readyRead, [&win, c] { c->readAll(); win.showUp(); c->deleteLater(); });
    });

    QNetworkAccessManager net;
    auto postAction = [&](const QByteArray &json, std::function<void()> done) {
        bool denied;
        QString tok = readToken(&denied);
        if (tok.isEmpty()) { done(); return; }
        QNetworkRequest req(QUrl(QString("http://127.0.0.1:%1/api/action").arg(UI_PORT)));
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        req.setRawHeader("X-USBeam-Token", tok.toLatin1());
        QNetworkReply *rep = net.post(req, json);
        QObject::connect(rep, &QNetworkReply::finished, &app, [rep, done] { rep->deleteLater(); done(); });
    };

    QSystemTrayIcon tray(appIcon());
    tray.setToolTip("USBeam");
    QMenu menu;
    menu.addAction("Open USBeam", [&] { win.showUp(); });
    menu.addSeparator();
    menu.addAction("Quit USBeam", [&] {
        // Quit = stop everything: the service ends all connections, returns shared devices
        // to this PC and releases devices used from other PCs. Then close.
        g_quitting = true;
        win.hide();
        postAction("{\"op\":\"quit\"}", [&app] { app.quit(); });
        QTimer::singleShot(3000, &app, [&app] { app.quit(); });
    });
    tray.setContextMenu(&menu);
    QObject::connect(&tray, &QSystemTrayIcon::activated, [&](QSystemTrayIcon::ActivationReason why) {
        if (why == QSystemTrayIcon::Trigger || why == QSystemTrayIcon::DoubleClick) {
            if (win.isVisible() && win.isActiveWindow()) win.hide(); else win.showUp();
        }
    });

    // At sign-in: the tray icon only if "Show USBeam in the tray when I sign in" is on.
    if (trayOnly) {
        auto *tries = new int(0);
        auto *check = new QTimer(&app);
        QObject::connect(check, &QTimer::timeout, &app, [&, tries, check] {
            bool denied;
            QString tok = readToken(&denied);
            if (tok.isEmpty() && ++*tries < 15) return;          // the service may still be starting
            check->stop();
            if (tok.isEmpty()) { tray.show(); return; }
            QNetworkRequest req(QUrl(QString("http://127.0.0.1:%1/api/state").arg(UI_PORT)));
            req.setRawHeader("X-USBeam-Token", tok.toLatin1());
            QNetworkReply *rep = net.get(req);
            QObject::connect(rep, &QNetworkReply::finished, &app, [&, rep] {
                QByteArray body = rep->readAll();
                rep->deleteLater();
                if (body.contains("\"tray\":false}")) app.quit(); else tray.show();
            });
        });
        check->start(2000);
    } else tray.show();

    // Keep the service running (it stops for Quit, and briefly for Restart).
    auto *svc = new QTimer(&app);
    auto *missing = new int(0);
    QObject::connect(svc, &QTimer::timeout, &app, [missing] {
        if (g_quitting) return;
        if (QFile::exists(TOKEN_FILE)) { *missing = 0; return; }
        if (++*missing >= 2) { *missing = 0; ensureService(); }
    });
    svc->start(5000);

    // "Save diagnostics": files the page creates go to the Downloads folder.
    QObject::connect(win.view->page()->profile(), &QWebEngineProfile::downloadRequested, &app, [&](QWebEngineDownloadRequest *d) {
        QString dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
        if (dir.isEmpty()) dir = QDir::homePath();
        d->setDownloadDirectory(dir);
        d->accept();
    });

    // Requests from the page.
    win.page->onAppLink = [&](const QUrl &url) {
        const QString what = url.path();
        const QUrlQuery q(url);
        if (what == "open") {
            QUrl target(q.queryItemValue("url", QUrl::FullyDecoded));
            if (target.toString().startsWith(PROJECT)) QDesktopServices::openUrl(target);
            return;
        }
        if (what == "uninstall") {
            const QString kind = installKind();
            win.report("Uninstalling…");
            g_quitting = true;                         // don't start the service again while it's being removed
            auto *p = new QProcess(&app);
            QObject::connect(p, &QProcess::finished, &app, [&, p](int code, QProcess::ExitStatus st) {
                p->deleteLater();
                if (st == QProcess::NormalExit && code == 0) {
                    g_quitting = true;
                    removeUserData();
                    tray.hide();
                    QMessageBox::information(nullptr, "USBeam", "USBeam has been uninstalled.");
                    app.quit();
                } else {
                    g_quitting = false;
                    win.report(code == 126 || code == 127 ? "Uninstall was cancelled." : QString("Uninstall failed (%1).").arg(code));
                }
            });
            if (kind == "appimage") p->start("pkexec", { "/bin/sh", QString(PREFIX) + "/setup.sh", "--uninstall" });
            else if (kind == "package" && QFile::exists("/usr/bin/pacman")) p->start("pkexec", { "/usr/bin/pacman", "-Rns", "--noconfirm", "usbeam" });
            else { g_quitting = false; win.report("Remove USBeam with your package manager."); }
            return;
        }
        if (what == "update") {
            const QString src = q.queryItemValue("url", QUrl::FullyDecoded), sha = q.queryItemValue("sha256").toLower();
            const QString ver = q.queryItemValue("version");
            if (installKind() != "appimage" || !src.startsWith(DOWNLOADS) || src.contains("..") || !src.endsWith(".AppImage") ||
                ver.isEmpty() || ver.contains(QRegularExpression("[^0-9.]"))) { win.report("This update can't be installed from here."); return; }
            QNetworkRequest req{QUrl(src)};
            req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
            QNetworkReply *rep = net.get(req);
            QObject::connect(rep, &QNetworkReply::downloadProgress, &win, [&win, ver](qint64 got, qint64 total) {
                if (total > 0) win.report(QString("Downloading USBeam %1… %2%").arg(ver).arg(got * 100 / total));
            });
            QObject::connect(rep, &QNetworkReply::finished, &app, [&, rep, sha, ver] {
                rep->deleteLater();
                if (rep->error() != QNetworkReply::NoError) { win.report("The download failed. Is this PC online?"); return; }
                QByteArray data = rep->readAll();
                if (!sha.isEmpty() && QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex() != sha.toLatin1()) {
                    win.report("The download was damaged (its fingerprint doesn't match). Nothing was changed.");
                    return;
                }
                // Saved in this user's cache folder (the temporary folder may not allow running programs).
                const QString cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
                QDir().mkpath(cache);
                QString path = cache + QString("/USBeam-%1.AppImage").arg(ver);
                QFile f(path);
                if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size()) { win.report("The update could not be saved."); return; }
                f.close();
                f.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::ReadGroup | QFile::ExeGroup | QFile::ReadOther | QFile::ExeOther);
                win.report("Installing… you'll be asked for your password.");
                if (QProcess::startDetached(path, { "--install" })) { g_quitting = true; tray.hide(); app.quit(); }
                else win.report("The update could not be started.");
            });
            return;
        }
    };

    // After a package update replaces this program, restart into the new version (same state:
    // window open or just the tray icon).
    auto *upd = new QTimer(&app);
    QObject::connect(upd, &QTimer::timeout, &app, [&] {
        char exe[PATH_MAX];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        if (n <= 0) return;
        exe[n] = 0;
        const char *tag = " (deleted)";
        size_t len = strlen(exe), tl = strlen(tag);
        if (len <= tl || strcmp(exe + len - tl, tag) != 0) return;
        exe[len - tl] = 0;
        static int seen = 0;
        if (access(exe, X_OK) != 0 || ++seen < 2) return;   // give the update time to finish
        upd->stop();
        QStringList args;
        if (!win.isVisible()) args << "--tray";
        server.close();                                // let the new copy take over
        QProcess::startDetached(QString::fromLocal8Bit(exe), args);
        app.quit();
    });
    upd->start(5000);

    if (!trayOnly) win.showUp();
    return app.exec();
}
