// ==============================================================================
// catalog/CatalogDatabase.h
// SQLite-backed catalog store.
//
// One database file holds every folder, image, rating, keyword and collection.
// It lives in QStandardPaths::AppDataLocation by default so it survives
// reinstalls and is not tied to any one image folder.
//
// Threading: a QSqlDatabase connection belongs to the thread that opened it.
// This class is NOT thread-safe; each thread that needs the catalog must own
// its own CatalogDatabase instance (open() derives a per-thread connection
// name). CatalogImporter relies on this.
// ==============================================================================
#pragma once

#include "catalog/CatalogTypes.h"

#include <QString>
#include <QVector>

namespace lps {

class CatalogDatabase
{
public:
    // Current schema version. Bump when changing the schema and add a step to
    // migrate(); open() refuses a database newer than this.
    static constexpr int kSchemaVersion = 1;

    CatalogDatabase();
    ~CatalogDatabase();

    CatalogDatabase(const CatalogDatabase&)            = delete;
    CatalogDatabase& operator=(const CatalogDatabase&) = delete;

    // Default on-disk location: <AppDataLocation>/catalog/lumen-catalog.db
    static QString defaultCatalogPath();

    // Opens (creating and migrating if needed). Returns false and sets
    // lastError() on failure. Safe to call once per instance.
    bool open(const QString& databasePath = QString());
    void close();
    bool isOpen() const;
    QString databasePath() const;
    QString lastError() const;

    // ---- folders ------------------------------------------------------------
    // Adds a folder root, or returns the existing id if already present.
    qint64 addFolder(const QString& absolutePath);
    bool   removeFolder(qint64 folderId);          // cascades to its images
    QVector<CatalogFolder> folders(bool withCounts = true) const;

    // ---- images -------------------------------------------------------------
    // Inserts or updates by (folderId, relativePath). Returns the image id.
    // Preserves user data (rating/flag/label/keywords/look) on re-import.
    qint64 upsertImage(const CatalogImage& image);
    bool   removeImage(qint64 imageId);

    CatalogImage image(qint64 imageId) const;
    CatalogImage imageByPath(const QString& absolutePath) const;
    bool         hasImage(qint64 folderId, const QString& relativePath) const;

    // The main grid query.
    QVector<CatalogImage> query(const CatalogFilter& filter,
                                int limit  = -1,
                                int offset = 0) const;
    int queryCount(const CatalogFilter& filter) const;

    // ---- user data (cheap targeted updates, not a full upsert) --------------
    bool setRating(qint64 imageId, int rating);            // clamped 0..5
    bool setFlag(qint64 imageId, ImageFlag flag);
    bool setColorLabel(qint64 imageId, ColorLabel label);
    bool setLookJson(qint64 imageId, const QString& lookJson);

    // ---- keywords -----------------------------------------------------------
    bool        addKeyword(qint64 imageId, const QString& keyword);
    bool        removeKeyword(qint64 imageId, const QString& keyword);
    QStringList keywordsFor(qint64 imageId) const;
    QStringList allKeywords() const;

    // ---- collections --------------------------------------------------------
    qint64 createCollection(const QString& name);
    bool   deleteCollection(qint64 collectionId);
    bool   addToCollection(qint64 collectionId, qint64 imageId);
    bool   removeFromCollection(qint64 collectionId, qint64 imageId);

    // ---- misc ---------------------------------------------------------------
    CatalogStats stats() const;
    bool transaction();
    bool commit();
    bool rollback();

private:
    struct Impl;
    Impl* d;
};

} // namespace lps
