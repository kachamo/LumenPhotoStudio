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
#include <QDir>
#include <QEventLoop>
#include <QPixmap>
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
    const QStringList args = QCoreApplication::arguments();
    const bool smokeTest = args.contains(QStringLiteral("--smoke-test"));

    // ---- Screenshot mode ---------------------------------------------------
    // `LumenPhotoStudio --screenshot <dir>` renders each workspace to PNG and
    // exits. Needed because this is a GUI application whose README has to show
    // what it looks like, and because grabbing widgets is the only way to
    // inspect the interface without a human at the screen. Works under
    // QT_QPA_PLATFORM=offscreen, so it can run unattended.
    QString shotDir;
    const int shotArg = args.indexOf(QStringLiteral("--screenshot"));
    if (shotArg >= 0 && shotArg + 1 < args.size())
        shotDir = args.at(shotArg + 1);

    // Optional: `--open <image>` loads a photo first, so the editor screenshot
    // shows the application doing its job rather than an empty canvas.
    QString shotImage;
    const int openArg = args.indexOf(QStringLiteral("--open"));
    if (openArg >= 0 && openArg + 1 < args.size())
        shotImage = args.at(openArg + 1);

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

        // Exercise the Library workspace in smoke mode. Constructing
        // MainWindow alone never opens the catalog (it is opened lazily on
        // first use), so without this CI would not compile-and-run a single
        // line of the SQLite path, the thumbnail cache, or the grid model.
        if (smokeTest)
            window.showLibraryWorkspace();

        if (!shotDir.isEmpty()) {
            QDir().mkpath(shotDir);
            window.resize(1600, 1000);

            const auto shoot = [&](const QString& name) {
                // Let layout and any queued paints settle before grabbing.
                for (int i = 0; i < 8; ++i)
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 40);
                const QPixmap pm = window.grab();
                const QString out = QDir(shotDir).filePath(name + QStringLiteral(".png"));
                qInfo("screenshot %s -> %s", qPrintable(name),
                      pm.save(out) ? "ok" : "FAILED");
            };

            shoot(QStringLiteral("01-welcome"));
            window.showLibraryWorkspace();
            shoot(QStringLiteral("02-library"));

            if (!shotImage.isEmpty() && !window.loadImageFromPath(shotImage))
                qWarning("could not open %s", qPrintable(shotImage));
            window.showEditorWorkspace();
            shoot(QStringLiteral("03-editor"));

            return 0;
        }

        return app.exec();
    } catch (const std::exception& ex) {
        qCritical("Fatal: unhandled exception during startup: %s", ex.what());
        return 1;
    } catch (...) {
        qCritical("Fatal: unhandled non-standard exception during startup");
        return 1;
    }
}
