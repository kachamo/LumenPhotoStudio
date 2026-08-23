// ==============================================================================
// catalog/CatalogImporter.h
// Background folder scan and import.
//
// Importing 40,000 files must never block the UI, must be cancellable, and must
// report progress. The importer owns its own worker thread and its own
// CatalogDatabase connection (see the threading note in CatalogDatabase.h).
//
// Re-importing a folder is cheap and non-destructive: files already present
// with an unchanged size+mtime are skipped, and user data (rating, flag, label,
// keywords, edits) is always preserved.
// ==============================================================================
#pragma once

#include "catalog/CatalogTypes.h"

#include <QObject>
#include <QString>
#include <QStringList>

namespace lps {

class CatalogImporter : public QObject
{
    Q_OBJECT

public:
    explicit CatalogImporter(QObject* parent = nullptr);
    ~CatalogImporter() override;

    // Extensions the importer will pick up (lower-case, no dot).
    static QStringList supportedExtensions();
    static bool        isSupportedFile(const QString& path);

    bool isRunning() const;

    // Path of the catalog the worker will write to. Must be set before start();
    // defaults to CatalogDatabase::defaultCatalogPath().
    void setCatalogPath(const QString& path);

public slots:
    // Scans `folderPath` (recursively if asked) and imports on a worker thread.
    // Emits progress/finished. Calling while running is ignored.
    void startImport(const QString& folderPath, bool recursive = true);
    void cancel();

signals:
    void started(const QString& folderPath);
    // `current` counts files processed; `total` is the scan result (0 while scanning).
    void progress(int current, int total, const QString& currentFile);
    void imageImported(qint64 imageId, const QString& absolutePath);
    // Emitted once at the end. `cancelled` distinguishes a user stop from completion.
    void finished(int imported, int skipped, int failed, bool cancelled);
    void failed(const QString& errorMessage);

private:
    struct Impl;
    Impl* d;
};

} // namespace lps
