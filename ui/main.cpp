// ==============================================================================
// ui/main.cpp
// Entry point for the LumenPhotoStudio executable.
//
// Responsibilities:
//   1. Establish the application identity (name, version, organization,
//      desktop-file id, window icon) before anything reads QSettings.
//   2. Support a headless --smoke-test mode used by CI.
//
// Every identity string comes from lps_build_info.h, which CMake generates
// from packaging/cmake/lps_build_info.h.in. Nothing here is hardcoded, so the
// reported version can never drift from project(VERSION) in CMakeLists.txt.
// ==============================================================================
#include "MainWindow.h"

#include "lps_build_info.h"

#include <QApplication>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QIcon>
#include <QStringList>
#include <QTimer>
#include <QtGlobal>

#include <exception>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);

    // ---- Application identity --------------------------------------------
    // Must precede MainWindow construction: SettingsManager and
    // AutosaveManager resolve their QStandardPaths locations from these.
    QCoreApplication::setOrganizationName(QStringLiteral(LPS_COMPANY_NAME_STR));
    QCoreApplication::setOrganizationDomain(QStringLiteral(LPS_ORG_DOMAIN_STR));
    QCoreApplication::setApplicationName(QStringLiteral(LPS_APP_NAME_STR));
    QCoreApplication::setApplicationVersion(QStringLiteral(LPS_APP_VERSION_STR));

    // Lets Wayland compositors and the XDG desktop match the running window to
    // packaging/linux/studio.lumen.photostudio.desktop, which is what supplies
    // the taskbar icon. Harmless on Windows and macOS.
    QGuiApplication::setDesktopFileName(QStringLiteral(LPS_DESKTOP_ID_STR));

    // Compiled into the binary via resources/resources.qrc
    // (prefix "/icons", alias "lumen_logo_512.png").
    QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/lumen_logo_512.png")));

    // ---- CI smoke test ----------------------------------------------------
    // `LumenPhotoStudio --smoke-test` constructs the main window, pumps the
    // event loop once and exits 0 without needing a display. CI runs it under
    // QT_QPA_PLATFORM=offscreen.
    const bool smokeTest =
        QCoreApplication::arguments().contains(QStringLiteral("--smoke-test"));

    try {
        // Armed before MainWindow is constructed so this zero-timer is first
        // in the event loop's queue. MainWindow's constructor registers its
        // own zero-timer for the autosave-recovery prompt; arming ours first
        // guarantees we quit before that modal dialog could hang a headless
        // run.
        //
        // QCoreApplication::exit() is used deliberately rather than quit():
        // since Qt 6.5 quit() first tries to close every top-level window,
        // and MainWindow::closeEvent() is allowed to veto that.
        if (smokeTest) {
            QTimer::singleShot(0, qApp, []() {
                QCoreApplication::exit(0);
            });
        }

        MainWindow window;
        window.show();
        return app.exec();
    } catch (const std::exception& ex) {
        qCritical("Fatal: unhandled exception during startup: %s", ex.what());
        return 1;
    } catch (...) {
        qCritical("Fatal: unhandled non-standard exception during startup");
        return 1;
    }
}
