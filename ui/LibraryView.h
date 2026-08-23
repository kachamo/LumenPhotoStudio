// ==============================================================================
// ui/LibraryView.h
// The Library workspace: folder tree, thumbnail grid, filter bar, import.
//
// This is the half of the application Lumen was missing. It owns the catalog
// connection and hands a path to MainWindow when the user opens an image.
// ==============================================================================
#pragma once

#include "catalog/CatalogTypes.h"

#include <QWidget>

class LibraryView : public QWidget
{
    Q_OBJECT

public:
    explicit LibraryView(QWidget* parent = nullptr);
    ~LibraryView() override;

    // Opens the catalog database. Returns false if it could not be opened.
    bool openCatalog(const QString& catalogPath = QString());
    void refresh();

public slots:
    void importFolder();                            // prompts for a folder
    void importFolderPath(const QString& path);     // no prompt

signals:
    // The user opened an image; MainWindow should load it in the editor.
    void imageActivated(const QString& absolutePath);
    void statusMessage(const QString& message);

private:
    struct Impl;
    Impl* d;
};
