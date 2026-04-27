// ==============================================================================
// ui/main.cpp
// Entry point for the LumenPhotoUI test executable.
// ==============================================================================
#include "MainWindow.h"

#include <QApplication>
#include <QCoreApplication>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);

    QCoreApplication::setOrganizationName(QStringLiteral("Lumen"));
    QCoreApplication::setApplicationName(QStringLiteral("Lumen Photo Studio"));

    MainWindow window;
    window.show();
    return app.exec();
}
