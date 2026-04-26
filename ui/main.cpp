// ==============================================================================
// ui/main.cpp
// Entry point for the LumenPhotoUI test executable.
// ==============================================================================
#include "MainWindow.h"

#include <QApplication>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);

    // QApplication::setOrganizationName / setApplicationName can be set here
    // once Anthropic-style settings/config persistence is wired in. For now
    // the test UI runs stateless.

    MainWindow window;
    window.show();
    return app.exec();
}
