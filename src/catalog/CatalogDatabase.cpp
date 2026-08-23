// ==============================================================================
// catalog/CatalogDatabase.cpp
// SQLite implementation of the catalog store.
//
// Notes that are not obvious from the header:
//
//   * Connections are per-instance. A QSqlDatabase connection may only be used
//     from the thread that opened it, so open() derives a unique connection
//     name from the creating thread's id plus a process-wide counter, and
//     close() removes it again. CatalogImporter therefore owns a second
//     CatalogDatabase on its worker thread and the two never share a handle.
//
//   * WAL is not an optimisation here, it is a requirement. The importer writes
//     from a worker thread while the UI reads; under the default rollback
//     journal a writer locks out every reader for the length of its
//     transaction, which is exactly the stall the Library view must not have.
//
//   * upsertImage() never touches the user-data columns on the update path.
//     Re-importing a folder is routine, and it must never cost the user a
//     rating, a flag, a colour label, a keyword or an edit.
//
//   * Every user-supplied value is bound, never concatenated. searchText comes
//     straight from a text box, so its LIKE metacharacters are escaped too.
//
//   * Timestamps are INTEGER epoch seconds; 0 means "unknown" and round-trips
//     as an invalid QDateTime.
//
// Error strings are developer-facing (this is an engine class, not UI), so they
// are deliberately specific and untranslated.
// ==============================================================================
#include "catalog/CatalogDatabase.h"

#include <QAtomicInteger>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QStringList>
#include <QThread>
#include <QVariant>
#include <QVariantList>
#include <QtGlobal>

#include <algorithm>
#include <memory>

namespace lps {
namespace {

// ==============================================================================
// Small helpers
// ==============================================================================

// Path comparison follows the platform. Windows filesystems are
// case-insensitive, so a recent-files entry spelled "c:/photos/a.jpg" must
// still resolve against a folder root stored as "C:/Photos".
#ifdef Q_OS_WIN
constexpr Qt::CaseSensitivity kPathCase = Qt::CaseInsensitive;
#else
constexpr Qt::CaseSensitivity kPathCase = Qt::CaseSensitive;
#endif

// 0 means "unknown". The test below is `!= 0` rather than `> 0` on purpose:
// negative epochs are legitimate in a photo catalog (a scanned print from 1965
// carries a capture time decades before the epoch).
qint64 toEpoch(const QDateTime& dt)
{
    return dt.isValid() ? dt.toSecsSinceEpoch() : 0;
}

QDateTime fromEpoch(qint64 seconds)
{
    return seconds != 0 ? QDateTime::fromSecsSinceEpoch(seconds) : QDateTime();
}

// Folder roots are normalised on the way in so "C:\Photos\" and "C:/Photos"
// are one row rather than two. Case is left alone: it is significant on Linux
// and on case-sensitive macOS volumes.
QString normalizeFolderPath(const QString& path)
{
    if (path.isEmpty())
        return QString();

    // Clean lexically FIRST, then canonicalise.
    //
    // canonicalPath() returns empty if any component does not exist, so a path
    // like "<root>/nested/.." with no "nested" directory silently fell through
    // to the un-canonicalised branch. On macOS that is not cosmetic: the temp
    // and home trees live under /var, which is a symlink to /private/var, so
    // "<root>" canonicalised to /private/var/... while "<root>/nested/.."
    // cleaned to /var/... . Two different strings for one folder, and therefore
    // two rows in `folders` for the same directory. Linux and Windows have no
    // equivalent symlink, so the bug was invisible there.
    //
    // Resolving ".." lexically before touching the filesystem is not identical
    // to resolving it after (it differs if a component is a symlink), but it is
    // consistent, and canonicalPath() below still resolves symlinks in whatever
    // survives — which is what actually matters for de-duplicating roots.
    const QDir dir(QDir::cleanPath(QDir::fromNativeSeparators(path)));
    const QString canonical = dir.canonicalPath();
    QString result = canonical.isEmpty() ? QDir::cleanPath(dir.absolutePath()) : canonical;

    // Drop a trailing separator, but keep it on a bare root ("/" or "C:/").
    while (result.length() > 1 && result.endsWith(QLatin1Char('/'))
           && !result.endsWith(QLatin1String(":/"))) {
        result.chop(1);
    }
    return result;
}

QString normalizeFilePath(const QString& path)
{
    if (path.isEmpty())
        return QString();

    const QFileInfo info(QDir::fromNativeSeparators(path));
    const QString canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? QDir::cleanPath(info.absoluteFilePath()) : canonical;
}

// Relative paths are stored with '/' separators and no leading "./" so that
// hasImage(), upsertImage() and the UNIQUE(folder_id, relative_path) key all
// agree regardless of which separator the caller happened to use.
QString normalizeRelativePath(const QString& path)
{
    if (path.isEmpty())
        return QString();

    QString result = QDir::cleanPath(QDir::fromNativeSeparators(path));
    while (result.startsWith(QLatin1String("./")))
        result.remove(0, 2);
    while (result.startsWith(QLatin1Char('/')))
        result.remove(0, 1);
    return result;
}

// A default-constructed QString is *null*, and QSqlQuery binds null as SQL
// NULL, which the NOT NULL text columns reject. Callers legitimately leave EXIF
// fields unset (a scanner has no lens model), so "absent" and "empty" must both
// store as ''. QStringLiteral("") is empty but not null, which is the point.
QString textOrEmpty(const QString& text)
{
    return text.isNull() ? QStringLiteral("") : text;
}

// Escapes the LIKE metacharacters so that searching for "50%" does not match
// every row. Paired with ESCAPE '\' in the SQL.
QString likePattern(const QString& text)
{
    QString escaped;
    escaped.reserve(text.size() + 8);
    for (const QChar c : text) {
        if (c == QLatin1Char('\\') || c == QLatin1Char('%') || c == QLatin1Char('_'))
            escaped.append(QLatin1Char('\\'));
        escaped.append(c);
    }
    return QStringLiteral("%") + escaped + QStringLiteral("%");
}

// Column order of the standard image SELECT. Named so the row reader cannot
// drift out of sync with the column list.
enum ImageColumn {
    ColId = 0,
    ColFolderId,
    ColFolderPath,
    ColFileName,
    ColRelativePath,
    ColFileSize,
    ColFileMtime,
    ColWidth,
    ColHeight,
    ColIsRaw,
    ColCaptureTime,
    ColCameraModel,
    ColLensModel,
    ColIso,
    ColAperture,
    ColShutterSpeed,
    ColFocalLength,
    ColRating,
    ColFlag,
    ColColorLabel,
    ColLookJson,
    ColImportedAt
};

QString imageColumnList()
{
    return QStringLiteral(
        "i.id, i.folder_id, f.path, i.filename, i.relative_path, i.file_size, "
        "i.file_mtime, i.width, i.height, i.is_raw, i.capture_time, i.camera_model, "
        "i.lens_model, i.iso, i.aperture, i.shutter_speed, i.focal_length, i.rating, "
        "i.flag, i.color_label, i.look_json, i.imported_at");
}

// LEFT JOIN rather than INNER JOIN for two reasons: it cannot silently drop
// rows (so queryCount() can never disagree with query().size()), and SQLite is
// not permitted to reorder the tables of a LEFT JOIN, which pins `images` as
// the outer loop so the ORDER BY index actually gets used.
QString imageFromClause()
{
    return QStringLiteral(" FROM images i LEFT JOIN folders f ON f.id = i.folder_id");
}

CatalogImage readImageRow(const QSqlQuery& q)
{
    CatalogImage img;
    img.id           = q.value(ColId).toLongLong();
    img.folderId     = q.value(ColFolderId).toLongLong();
    img.fileName     = q.value(ColFileName).toString();
    img.relativePath = q.value(ColRelativePath).toString();

    const QString folderPath = q.value(ColFolderPath).toString();
    img.absolutePath = folderPath.isEmpty()
        ? img.relativePath
        : folderPath + QLatin1Char('/') + img.relativePath;

    img.fileSize     = q.value(ColFileSize).toLongLong();
    img.fileModified = fromEpoch(q.value(ColFileMtime).toLongLong());
    img.width        = q.value(ColWidth).toInt();
    img.height       = q.value(ColHeight).toInt();
    img.isRaw        = q.value(ColIsRaw).toInt() != 0;

    img.captureTime  = fromEpoch(q.value(ColCaptureTime).toLongLong());
    img.cameraModel  = q.value(ColCameraModel).toString();
    img.lensModel    = q.value(ColLensModel).toString();
    img.iso          = q.value(ColIso).toString();
    img.aperture     = q.value(ColAperture).toString();
    img.shutterSpeed = q.value(ColShutterSpeed).toString();
    img.focalLength  = q.value(ColFocalLength).toString();

    // Clamped on read as well as on write: the catalog file is user-writable
    // and a hand-mangled value must not produce a nonsense enumerator.
    img.rating     = std::clamp(q.value(ColRating).toInt(), 0, 5);
    img.flag       = static_cast<ImageFlag>(std::clamp(q.value(ColFlag).toInt(), -1, 1));
    img.colorLabel = static_cast<ColorLabel>(std::clamp(q.value(ColColorLabel).toInt(), 0, 5));

    img.lookJson   = q.value(ColLookJson).toString();
    img.importedAt = fromEpoch(q.value(ColImportedAt).toLongLong());
    return img;
}

// SortKey -> ORDER BY expression. Not user input, so composing it into the SQL
// is safe; everything that *is* user input is bound.
QString sortExpression(SortKey key)
{
    switch (key) {
    case SortKey::CaptureTime: return QStringLiteral("i.capture_time");
    // The COLLATE has to match idx_images_filename exactly, or the index is
    // unusable for the sort.
    case SortKey::FileName:    return QStringLiteral("i.filename COLLATE NOCASE");
    case SortKey::Rating:      return QStringLiteral("i.rating");
    case SortKey::ImportedAt:  return QStringLiteral("i.imported_at");
    case SortKey::FileSize:    return QStringLiteral("i.file_size");
    }
    return QStringLiteral("i.capture_time");   // defensive: a cast-in bogus value
}

// A WHERE clause plus its bindings, built once and used by both query() and
// queryCount() so the two can never drift apart.
struct Predicate
{
    QString      sql;     // empty when the filter matches everything
    QVariantList binds;   // positional, in order of appearance
};

Predicate buildPredicate(const CatalogFilter& filter)
{
    Predicate   p;
    QStringList clauses;

    if (filter.folderId >= 0) {
        clauses << QStringLiteral("i.folder_id = ?");
        p.binds << filter.folderId;
    }
    if (filter.minRating > 0) {
        clauses << QStringLiteral("i.rating >= ?");
        p.binds << filter.minRating;
    }
    if (filter.pickedOnly) {
        // A picked image is never rejected, so hideRejected adds nothing here.
        clauses << QStringLiteral("i.flag = ?");
        p.binds << static_cast<int>(ImageFlag::Picked);
    } else if (filter.hideRejected) {
        clauses << QStringLiteral("i.flag <> ?");
        p.binds << static_cast<int>(ImageFlag::Rejected);
    }
    if (filter.colorLabel != ColorLabel::None) {
        clauses << QStringLiteral("i.color_label = ?");
        p.binds << static_cast<int>(filter.colorLabel);
    }

    const QString search = filter.searchText.trimmed();
    if (!search.isEmpty()) {
        // Four separate positional placeholders rather than one repeated named
        // one: Qt rewrites named placeholders to positional for SQLite, and a
        // name that appears several times is a classic source of surprises.
        // SQLite's LIKE is already case-insensitive for ASCII.
        clauses << QStringLiteral(
            "(i.filename LIKE ? ESCAPE '\\'"
            " OR i.camera_model LIKE ? ESCAPE '\\'"
            " OR i.lens_model LIKE ? ESCAPE '\\'"
            " OR EXISTS (SELECT 1 FROM image_keywords ik"
            "            JOIN keywords k ON k.id = ik.keyword_id"
            "            WHERE ik.image_id = i.id AND k.name LIKE ? ESCAPE '\\'))");
        const QString pattern = likePattern(search);
        p.binds << pattern << pattern << pattern << pattern;
    }

    // Keywords are AND-ed: one EXISTS per keyword. keywords.name is declared
    // COLLATE NOCASE, so "=" here is already case-insensitive.
    for (const QString& keyword : filter.keywords) {
        const QString trimmed = keyword.trimmed();
        if (trimmed.isEmpty())
            continue;
        clauses << QStringLiteral(
            "EXISTS (SELECT 1 FROM image_keywords ik"
            "        JOIN keywords k ON k.id = ik.keyword_id"
            "        WHERE ik.image_id = i.id AND k.name = ?)");
        p.binds << trimmed;
    }

    if (!clauses.isEmpty())
        p.sql = QStringLiteral(" WHERE ") + clauses.join(QStringLiteral(" AND "));
    return p;
}

void bindAll(QSqlQuery& q, const QVariantList& binds)
{
    for (const QVariant& v : binds)
        q.addBindValue(v);
}

// ==============================================================================
// Schema
//
// Every index below earns its place; an unindexed sort over 40,000 rows is a
// visible stall in the grid, and every extra index is a cost on the import
// path, so the set is deliberately minimal.
// ==============================================================================
QStringList schemaStatements()
{
    return QStringList{
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS schema_info ("
            "  key   TEXT PRIMARY KEY,"
            "  value TEXT NOT NULL)"),

        // AUTOINCREMENT everywhere a row id is handed out to the UI: without it
        // SQLite reuses the rowid of a deleted row, and a stale selection or a
        // cached thumbnail id would silently point at a different photo.
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS folders ("
            "  id       INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  path     TEXT    NOT NULL UNIQUE,"
            "  added_at INTEGER NOT NULL DEFAULT 0)"),

        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS images ("
            "  id            INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  folder_id     INTEGER NOT NULL REFERENCES folders(id) ON DELETE CASCADE,"
            "  filename      TEXT    NOT NULL,"
            "  relative_path TEXT    NOT NULL,"
            "  file_size     INTEGER NOT NULL DEFAULT 0,"
            "  file_mtime    INTEGER NOT NULL DEFAULT 0,"
            "  width         INTEGER NOT NULL DEFAULT 0,"
            "  height        INTEGER NOT NULL DEFAULT 0,"
            "  is_raw        INTEGER NOT NULL DEFAULT 0,"
            "  capture_time  INTEGER NOT NULL DEFAULT 0,"
            "  camera_model  TEXT    NOT NULL DEFAULT '',"
            "  lens_model    TEXT    NOT NULL DEFAULT '',"
            "  iso           TEXT    NOT NULL DEFAULT '',"
            "  aperture      TEXT    NOT NULL DEFAULT '',"
            "  shutter_speed TEXT    NOT NULL DEFAULT '',"
            "  focal_length  TEXT    NOT NULL DEFAULT '',"
            "  rating        INTEGER NOT NULL DEFAULT 0 CHECK (rating      BETWEEN  0 AND 5),"
            "  flag          INTEGER NOT NULL DEFAULT 0 CHECK (flag        BETWEEN -1 AND 1),"
            "  color_label   INTEGER NOT NULL DEFAULT 0 CHECK (color_label BETWEEN  0 AND 5),"
            "  look_json     TEXT    NOT NULL DEFAULT '',"
            "  imported_at   INTEGER NOT NULL DEFAULT 0,"
            // The upsert conflict target. Its implicit index is also what
            // hasImage() and imageByPath() ride on, and it is the leading-column
            // index for folder_id, so folders(id) ON DELETE CASCADE is a lookup
            // rather than a table scan.
            "  UNIQUE (folder_id, relative_path))"),

        // Keyword names are matched case-insensitively throughout, so the
        // uniqueness must be too: "Portrait" and "portrait" are one keyword.
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS keywords ("
            "  id   INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  name TEXT NOT NULL COLLATE NOCASE UNIQUE)"),

        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS image_keywords ("
            "  image_id   INTEGER NOT NULL REFERENCES images(id)   ON DELETE CASCADE,"
            "  keyword_id INTEGER NOT NULL REFERENCES keywords(id) ON DELETE CASCADE,"
            "  PRIMARY KEY (image_id, keyword_id))"),

        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS collections ("
            "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  name       TEXT NOT NULL COLLATE NOCASE UNIQUE,"
            "  created_at INTEGER NOT NULL DEFAULT 0)"),

        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS collection_images ("
            "  collection_id INTEGER NOT NULL REFERENCES collections(id) ON DELETE CASCADE,"
            "  image_id      INTEGER NOT NULL REFERENCES images(id)      ON DELETE CASCADE,"
            "  PRIMARY KEY (collection_id, image_id))"),

        // ---- indexes ---------------------------------------------------------
        // The single most important one: the Library's default view is a folder
        // in capture-time order. This covers the WHERE and the ORDER BY at once,
        // so selecting a folder is an index range scan of one screenful instead
        // of a full scan plus a temp-B-tree sort of every row in it.
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_images_folder_capture"
                       "  ON images (folder_id, capture_time)"),

        // The all-folders default view. SQLite walks this index backwards for
        // "newest first" and stops at LIMIT, touching a page of rows rather than
        // sorting 40,000. (The rowid is implicitly the last index column, which
        // is what lets the "…, i.id" tiebreak stay inside the index.)
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_images_capture_time"
                       "  ON images (capture_time)"),

        // SortKey::FileName. NOCASE so "IMG_10.jpg" sorts next to "img_9.jpg",
        // and so the ORDER BY collation matches the index collation.
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_images_filename"
                       "  ON images (filename COLLATE NOCASE)"),

        // SortKey::Rating, and the `rating >= n` filter, which is the second
        // most used control in the filter bar after the folder tree.
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_images_rating"
                       "  ON images (rating)"),

        // SortKey::ImportedAt — "what did I just add?" is a common view.
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_images_imported_at"
                       "  ON images (imported_at)"),

        // SortKey::FileSize — rarer, but it is in the enum, and an unindexed
        // sort key is exactly the stall this table is trying to avoid.
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_images_file_size"
                       "  ON images (file_size)"),

        // pickedOnly (flag = 1) is selective, and "show me my picks, newest
        // first" is a whole Lightroom workflow. capture_time is the second
        // column so the filter and the default sort resolve in one index scan;
        // with a bare (flag) index SQLite finds the rows but then sorts them in
        // a temp B-tree, which measured 13x slower here.
        // hideRejected (flag <> -1) matches nearly everything and the planner
        // correctly ignores this index for it; that is the right call.
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_images_flag_capture"
                       "  ON images (flag, capture_time)"),

        // colorLabel = n, same shape and the same reason. Full rather than
        // partial (`WHERE color_label <> 0`) because the label is a bound
        // parameter, and SQLite can only use a partial index when the query's
        // own WHERE clause proves the index predicate at prepare time, which a
        // placeholder never does.
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_images_color_capture"
                       "  ON images (color_label, capture_time)"),

        // The PRIMARY KEY (image_id, keyword_id) already serves image -> keywords.
        // This is the reverse direction, which is what the keyword filter and
        // the keyword arm of the text search need.
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_image_keywords_keyword"
                       "  ON image_keywords (keyword_id)"),

        // Same reasoning, plus it turns the ON DELETE CASCADE from images into
        // a lookup rather than a scan of every collection membership.
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_collection_images_image"
                       "  ON collection_images (image_id)")

        // Deliberately NOT indexed: camera_model and lens_model. The only query
        // against them is the text search, which is LIKE '%needle%'; a B-tree
        // cannot serve a leading wildcard, so an index there would cost import
        // time and buy nothing. If substring search over metadata ever becomes
        // slow enough to matter, the answer is an FTS5 table, not a B-tree.
    };
}

// Runs a list of statements, stopping at the first failure.
bool execAll(QSqlDatabase& db, const QStringList& statements, QString* errorOut)
{
    for (const QString& sql : statements) {
        QSqlQuery q(db);
        if (!q.exec(sql)) {
            if (errorOut) {
                *errorOut = QStringLiteral("statement failed [%1]: %2")
                                .arg(sql.left(72), q.lastError().text().trimmed());
            }
            return false;
        }
    }
    return true;
}

bool writeSchemaVersion(QSqlDatabase& db, int version, QString* errorOut)
{
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT INTO schema_info (key, value) VALUES ('version', :version) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value"));
    q.bindValue(QStringLiteral(":version"), QString::number(version));
    if (!q.exec()) {
        if (errorOut)
            *errorOut = QStringLiteral("cannot write schema version: ")
                        + q.lastError().text().trimmed();
        return false;
    }
    return true;
}

// Applies schema steps in order. v1 is the initial schema, so there is nothing
// to do yet; the seam exists so a future v2 lands as one more `if` here plus a
// bumped kSchemaVersion, never as an ad-hoc fixup at a call site.
bool migrate(QSqlDatabase& db, int fromVersion, QString* errorOut)
{
    int version = fromVersion;

    // if (version < 2) {
    //     if (!execAll(db, migrationToV2(), errorOut)) return false;
    //     version = 2;
    // }

    if (version != CatalogDatabase::kSchemaVersion) {
        if (errorOut) {
            *errorOut = QStringLiteral("no migration path from catalog schema version %1 to %2")
                            .arg(fromVersion)
                            .arg(CatalogDatabase::kSchemaVersion);
        }
        return false;
    }
    return writeSchemaVersion(db, version, errorOut);
}

} // namespace

// ==============================================================================
// Impl
// ==============================================================================
struct CatalogDatabase::Impl
{
    QString path;
    QString connectionName;
    QString lastError;
    bool    open = false;

    // The importer calls hasImage() and upsertImage() once per file. Re-parsing
    // and re-planning a 20-column upsert 40,000 times is measurable, so the two
    // hot statements are prepared once and rebound per row.
    //
    // They hold a reference to the driver, so they MUST be destroyed before
    // QSqlDatabase::removeDatabase() or Qt logs "connection is still in use".
    std::unique_ptr<QSqlQuery> upsertStmt;
    std::unique_ptr<QSqlQuery> selectIdStmt;
    std::unique_ptr<QSqlQuery> hasImageStmt;

    QSqlDatabase handle() const
    {
        // `false` = do not try to (re)open; a closed handle must surface as an
        // error, not as a silent reconnect on the wrong thread.
        return QSqlDatabase::database(connectionName, false);
    }

    bool fail(const QString& message)
    {
        lastError = message;
        return false;
    }

    bool fail(const QString& context, const QSqlError& error)
    {
        lastError = context + QStringLiteral(": ") + error.text().trimmed();
        return false;
    }

    bool fail(const QString& context, const QSqlQuery& query)
    {
        return fail(context, query.lastError());
    }

    bool requireOpen(const QString& context)
    {
        if (open)
            return true;
        lastError = context + QStringLiteral(": catalog is not open");
        return false;
    }

    void releaseStatements()
    {
        upsertStmt.reset();
        selectIdStmt.reset();
        hasImageStmt.reset();
    }

    // Fills in the keywords of an already-fetched result set with one batched
    // query per chunk, rather than the N+1 that would make a 40k grid crawl.
    void attachKeywords(QVector<CatalogImage>& images);
};

void CatalogDatabase::Impl::attachKeywords(QVector<CatalogImage>& images)
{
    if (images.isEmpty())
        return;

    QHash<qint64, qsizetype> indexById;
    indexById.reserve(images.size());
    for (qsizetype i = 0; i < images.size(); ++i)
        indexById.insert(images[i].id, i);

    QSqlDatabase db = handle();

    // Chunked to stay well under SQLITE_MAX_VARIABLE_NUMBER on every build.
    constexpr qsizetype kChunk = 500;
    for (qsizetype start = 0; start < images.size(); start += kChunk) {
        const qsizetype count = std::min(kChunk, images.size() - start);

        QString sql = QStringLiteral(
            "SELECT ik.image_id, k.name FROM image_keywords ik "
            "JOIN keywords k ON k.id = ik.keyword_id WHERE ik.image_id IN (");
        for (qsizetype i = 0; i < count; ++i)
            sql += (i == 0 ? QStringLiteral("?") : QStringLiteral(",?"));
        sql += QStringLiteral(") ORDER BY k.name COLLATE NOCASE");

        QSqlQuery q(db);
        q.setForwardOnly(true);
        if (!q.prepare(sql)) {
            fail(QStringLiteral("attachKeywords"), q);
            return;
        }
        for (qsizetype i = 0; i < count; ++i)
            q.addBindValue(images[start + i].id);
        if (!q.exec()) {
            fail(QStringLiteral("attachKeywords"), q);
            return;
        }
        while (q.next()) {
            const auto it = indexById.constFind(q.value(0).toLongLong());
            if (it != indexById.constEnd())
                images[*it].keywords.append(q.value(1).toString());
        }
    }
}

// ==============================================================================
// Construction / lifetime
// ==============================================================================
CatalogDatabase::CatalogDatabase() : d(new Impl) {}

CatalogDatabase::~CatalogDatabase()
{
    close();
    delete d;
}

QString CatalogDatabase::defaultCatalogPath()
{
    const QString base =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(base).filePath(QStringLiteral("catalog/lumen-catalog.db"));
}

// ==============================================================================
// open / close
// ==============================================================================
bool CatalogDatabase::open(const QString& databasePath)
{
    if (d->open) {
        return d->fail(QStringLiteral("open: catalog is already open at ") + d->path);
    }

    const QString resolved =
        databasePath.isEmpty()
            ? defaultCatalogPath()
            : QDir::cleanPath(QDir::fromNativeSeparators(databasePath));
    if (resolved.isEmpty())
        return d->fail(QStringLiteral("open: no catalog path was resolved"));

    const QString dirPath = QFileInfo(resolved).absolutePath();
    if (!dirPath.isEmpty() && !QDir().mkpath(dirPath)) {
        return d->fail(QStringLiteral("open: cannot create catalog directory ") + dirPath);
    }

    if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE"))) {
        return d->fail(QStringLiteral(
            "open: the Qt SQLite driver (QSQLITE) is not available; check that the "
            "sqldrivers plugin is deployed next to the executable"));
    }

    // A QSqlDatabase connection belongs to the thread that opened it, and two
    // connections may not share a name. The counter guarantees uniqueness even
    // for two instances on the same thread; the thread id is there so the name
    // is meaningful in a Qt warning message.
    static QAtomicInteger<quint64> s_serial{0};
    const QString connection =
        QStringLiteral("lps_catalog_%1_%2")
            .arg(QString::number(reinterpret_cast<quintptr>(QThread::currentThreadId()), 16))
            .arg(s_serial.fetchAndAddOrdered(1));

    QString failure;
    {
        // Scoped so that the QSqlDatabase copy is destroyed before any
        // removeDatabase() below; Qt warns loudly otherwise.
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        if (!db.isValid()) {
            failure = QStringLiteral("open: could not create a QSQLITE connection");
        } else {
            db.setDatabaseName(resolved);
            if (!db.open()) {
                failure = QStringLiteral("open: cannot open %1: %2")
                              .arg(resolved, db.lastError().text().trimmed());
            } else {
                // ---- pragmas ------------------------------------------------
                // Order matters: PRAGMA foreign_keys is a no-op inside a
                // transaction, so it has to run before any schema work.
                QSqlQuery pragma(db);

                if (!pragma.exec(QStringLiteral("PRAGMA foreign_keys = ON"))) {
                    failure = QStringLiteral("open: cannot enable foreign keys: ")
                              + pragma.lastError().text().trimmed();
                } else if (!pragma.exec(QStringLiteral("PRAGMA foreign_keys"))
                           || !pragma.next() || pragma.value(0).toInt() != 1) {
                    // Without cascades, removeFolder() would orphan its images.
                    failure = QStringLiteral(
                        "open: SQLite refused to enable foreign key enforcement");
                }

                if (failure.isEmpty()) {
                    // WAL lets the importer write while the UI reads. If the
                    // catalog sits on a filesystem that cannot do WAL (some
                    // network shares), degrade rather than refuse to open.
                    if (pragma.exec(QStringLiteral("PRAGMA journal_mode = WAL"))
                        && pragma.next()) {
                        const QString mode = pragma.value(0).toString();
                        if (mode.compare(QStringLiteral("wal"), Qt::CaseInsensitive) != 0) {
                            qWarning("CatalogDatabase: journal_mode is '%s', not WAL; "
                                     "background import will block the UI's reads",
                                     qPrintable(mode));
                        }
                    }
                    // NORMAL is the documented companion to WAL: durable across
                    // a process crash, and only at risk from a power cut, which
                    // for a re-buildable catalog is the right trade for the
                    // order-of-magnitude it buys on bulk import.
                    pragma.exec(QStringLiteral("PRAGMA synchronous = NORMAL"));
                    // Even under WAL there is one writer at a time. Wait for the
                    // importer's transaction instead of failing with SQLITE_BUSY.
                    pragma.exec(QStringLiteral("PRAGMA busy_timeout = 5000"));
                }

                // ---- schema -------------------------------------------------
                if (failure.isEmpty()) {
                    QSqlQuery probe(db);
                    if (!probe.exec(QStringLiteral(
                            "SELECT COUNT(*) FROM sqlite_master "
                            "WHERE type = 'table' AND name = 'schema_info'"))
                        || !probe.next()) {
                        failure = QStringLiteral("open: %1 is not a readable SQLite catalog: %2")
                                      .arg(resolved, probe.lastError().text().trimmed());
                    } else {
                        const bool isNew = probe.value(0).toInt() == 0;
                        probe.finish();

                        if (isNew) {
                            if (!db.transaction()) {
                                failure = QStringLiteral("open: cannot begin schema creation: ")
                                          + db.lastError().text().trimmed();
                            } else if (!execAll(db, schemaStatements(), &failure)
                                       || !writeSchemaVersion(db, kSchemaVersion, &failure)) {
                                db.rollback();
                                failure.prepend(QStringLiteral("open: cannot create schema: "));
                            } else if (!db.commit()) {
                                db.rollback();
                                failure = QStringLiteral("open: cannot commit new schema: ")
                                          + db.lastError().text().trimmed();
                            }
                        } else {
                            QSqlQuery ver(db);
                            bool parsed  = false;
                            int  version = 0;
                            if (ver.exec(QStringLiteral(
                                    "SELECT value FROM schema_info WHERE key = 'version'"))
                                && ver.next()) {
                                version = ver.value(0).toInt(&parsed);
                            }
                            ver.finish();

                            if (!parsed) {
                                failure = QStringLiteral(
                                    "open: %1 has no readable schema version; it is either "
                                    "corrupt or not a Lumen catalog").arg(resolved);
                            } else if (version > kSchemaVersion) {
                                // Refuse rather than risk writing rows a newer
                                // build's constraints would reject.
                                failure =
                                    QStringLiteral("open: catalog schema version %1 is newer than "
                                                   "this build supports (%2); upgrade Lumen Photo "
                                                   "Studio to open %3")
                                        .arg(version)
                                        .arg(kSchemaVersion)
                                        .arg(resolved);
                            } else if (version < kSchemaVersion) {
                                if (!db.transaction()) {
                                    failure = QStringLiteral("open: cannot begin migration: ")
                                              + db.lastError().text().trimmed();
                                } else if (!migrate(db, version, &failure)) {
                                    db.rollback();
                                    failure.prepend(QStringLiteral("open: migration failed: "));
                                } else if (!db.commit()) {
                                    db.rollback();
                                    failure = QStringLiteral("open: cannot commit migration: ")
                                              + db.lastError().text().trimmed();
                                }
                            }
                        }
                    }
                }
            }
            if (!failure.isEmpty())
                db.close();
        }
    }

    if (!failure.isEmpty()) {
        QSqlDatabase::removeDatabase(connection);
        return d->fail(failure);
    }

    d->connectionName = connection;
    d->path           = resolved;
    d->open           = true;
    d->lastError.clear();
    return true;
}

void CatalogDatabase::close()
{
    // Prepared statements hold a reference to the driver; they have to go
    // first, and the QSqlDatabase copy has to be out of scope before
    // removeDatabase(), or Qt logs "connection is still in use".
    d->releaseStatements();

    if (!d->connectionName.isEmpty()) {
        {
            QSqlDatabase db = QSqlDatabase::database(d->connectionName, false);
            if (db.isValid() && db.isOpen())
                db.close();
        }
        QSqlDatabase::removeDatabase(d->connectionName);
        d->connectionName.clear();
    }
    d->open = false;
}

bool CatalogDatabase::isOpen() const
{
    return d->open;
}

QString CatalogDatabase::databasePath() const
{
    return d->path;
}

QString CatalogDatabase::lastError() const
{
    return d->lastError;
}

// ==============================================================================
// Folders
// ==============================================================================
qint64 CatalogDatabase::addFolder(const QString& absolutePath)
{
    if (!d->requireOpen(QStringLiteral("addFolder")))
        return -1;

    const QString normalized = normalizeFolderPath(absolutePath);
    if (normalized.isEmpty()) {
        d->fail(QStringLiteral("addFolder: empty path"));
        return -1;
    }

    QSqlDatabase db = d->handle();

    // Idempotent by contract: an existing root returns its id rather than an
    // error, because "add this folder" is also how the UI re-selects one.
    QSqlQuery existing(db);
    existing.prepare(QStringLiteral("SELECT id FROM folders WHERE path = :path"));
    existing.bindValue(QStringLiteral(":path"), normalized);
    if (!existing.exec()) {
        d->fail(QStringLiteral("addFolder: lookup failed"), existing);
        return -1;
    }
    if (existing.next())
        return existing.value(0).toLongLong();
    existing.finish();

    QSqlQuery insert(db);
    insert.prepare(QStringLiteral(
        "INSERT INTO folders (path, added_at) VALUES (:path, :added_at)"));
    insert.bindValue(QStringLiteral(":path"), normalized);
    insert.bindValue(QStringLiteral(":added_at"),
                     QDateTime::currentDateTime().toSecsSinceEpoch());
    if (!insert.exec()) {
        d->fail(QStringLiteral("addFolder: insert failed"), insert);
        return -1;
    }
    return insert.lastInsertId().toLongLong();
}

bool CatalogDatabase::removeFolder(qint64 folderId)
{
    if (!d->requireOpen(QStringLiteral("removeFolder")))
        return false;

    QSqlDatabase db = d->handle();
    QSqlQuery    q(db);
    q.prepare(QStringLiteral("DELETE FROM folders WHERE id = :id"));
    q.bindValue(QStringLiteral(":id"), folderId);
    if (!q.exec())
        return d->fail(QStringLiteral("removeFolder: delete failed"), q);
    if (q.numRowsAffected() == 0) {
        return d->fail(
            QStringLiteral("removeFolder: no folder with id %1").arg(folderId));
    }
    return true;
}

QVector<CatalogFolder> CatalogDatabase::folders(bool withCounts) const
{
    QVector<CatalogFolder> result;
    if (!d->requireOpen(QStringLiteral("folders")))
        return result;

    // One grouped query rather than a COUNT per folder: the folder tree is
    // rebuilt on every import-finished signal.
    const QString sql =
        withCounts
            ? QStringLiteral(
                  "SELECT f.id, f.path, f.added_at, COUNT(i.id) "
                  "FROM folders f LEFT JOIN images i ON i.folder_id = f.id "
                  "GROUP BY f.id, f.path, f.added_at "
                  "ORDER BY f.path COLLATE NOCASE")
            : QStringLiteral(
                  "SELECT f.id, f.path, f.added_at, 0 "
                  "FROM folders f ORDER BY f.path COLLATE NOCASE");

    QSqlDatabase db = d->handle();
    QSqlQuery    q(db);
    q.setForwardOnly(true);
    if (!q.exec(sql)) {
        d->fail(QStringLiteral("folders: query failed"), q);
        return result;
    }
    while (q.next()) {
        CatalogFolder folder;
        folder.id         = q.value(0).toLongLong();
        folder.path       = q.value(1).toString();
        folder.addedAt    = fromEpoch(q.value(2).toLongLong());
        folder.imageCount = q.value(3).toInt();
        result.push_back(folder);
    }
    return result;
}

// ==============================================================================
// Images
// ==============================================================================
qint64 CatalogDatabase::upsertImage(const CatalogImage& image)
{
    if (!d->requireOpen(QStringLiteral("upsertImage")))
        return -1;
    if (image.folderId < 0) {
        d->fail(QStringLiteral("upsertImage: folderId is not set"));
        return -1;
    }

    const QString relativePath = normalizeRelativePath(image.relativePath);
    if (relativePath.isEmpty()) {
        d->fail(QStringLiteral("upsertImage: relativePath is empty"));
        return -1;
    }
    // `filename` is the last segment of `relative_path` by definition, and the
    // grid both sorts and searches on it. Derive it rather than trusting the
    // field: a caller that fills fileName from an unnormalised path hands over
    // "sub\photo.jpg", which would then sort under 's' instead of 'p'. The
    // caller's value is used only when it is already a bare name, so a
    // deliberate display override still survives.
    const QString derivedName = relativePath.section(QLatin1Char('/'), -1);
    const QString suppliedName = normalizeRelativePath(image.fileName);
    const QString fileName = (!suppliedName.isEmpty()
                              && !suppliedName.contains(QLatin1Char('/')))
        ? suppliedName
        : derivedName;

    QSqlDatabase db = d->handle();

    if (!d->upsertStmt) {
        d->upsertStmt = std::make_unique<QSqlQuery>(db);
        // The DO UPDATE list is the whole point of this class's "re-import is
        // safe" promise: it names only file and EXIF columns. rating, flag,
        // color_label, look_json and imported_at are absent, so an existing row
        // keeps them; keywords and collection membership live in other tables
        // and are never touched here at all.
        if (!d->upsertStmt->prepare(QStringLiteral(
                "INSERT INTO images ("
                "  folder_id, filename, relative_path, file_size, file_mtime, width, height,"
                "  is_raw, capture_time, camera_model, lens_model, iso, aperture,"
                "  shutter_speed, focal_length, rating, flag, color_label, look_json,"
                "  imported_at) "
                "VALUES ("
                "  :folder_id, :filename, :relative_path, :file_size, :file_mtime, :width,"
                "  :height, :is_raw, :capture_time, :camera_model, :lens_model, :iso,"
                "  :aperture, :shutter_speed, :focal_length, :rating, :flag, :color_label,"
                "  :look_json, :imported_at) "
                "ON CONFLICT (folder_id, relative_path) DO UPDATE SET"
                "  filename      = excluded.filename,"
                "  file_size     = excluded.file_size,"
                "  file_mtime    = excluded.file_mtime,"
                "  width         = excluded.width,"
                "  height        = excluded.height,"
                "  is_raw        = excluded.is_raw,"
                "  capture_time  = excluded.capture_time,"
                "  camera_model  = excluded.camera_model,"
                "  lens_model    = excluded.lens_model,"
                "  iso           = excluded.iso,"
                "  aperture      = excluded.aperture,"
                "  shutter_speed = excluded.shutter_speed,"
                "  focal_length  = excluded.focal_length"))) {
            d->fail(QStringLiteral("upsertImage: prepare failed"), *d->upsertStmt);
            d->upsertStmt.reset();
            return -1;
        }
    }

    QSqlQuery& up = *d->upsertStmt;
    up.bindValue(QStringLiteral(":folder_id"), image.folderId);
    up.bindValue(QStringLiteral(":filename"), textOrEmpty(fileName));
    up.bindValue(QStringLiteral(":relative_path"), relativePath);
    up.bindValue(QStringLiteral(":file_size"), image.fileSize);
    up.bindValue(QStringLiteral(":file_mtime"), toEpoch(image.fileModified));
    up.bindValue(QStringLiteral(":width"), image.width);
    up.bindValue(QStringLiteral(":height"), image.height);
    up.bindValue(QStringLiteral(":is_raw"), image.isRaw ? 1 : 0);
    up.bindValue(QStringLiteral(":capture_time"), toEpoch(image.captureTime));
    up.bindValue(QStringLiteral(":camera_model"), textOrEmpty(image.cameraModel));
    up.bindValue(QStringLiteral(":lens_model"), textOrEmpty(image.lensModel));
    up.bindValue(QStringLiteral(":iso"), textOrEmpty(image.iso));
    up.bindValue(QStringLiteral(":aperture"), textOrEmpty(image.aperture));
    up.bindValue(QStringLiteral(":shutter_speed"), textOrEmpty(image.shutterSpeed));
    up.bindValue(QStringLiteral(":focal_length"), textOrEmpty(image.focalLength));
    // Only ever reach the table on the INSERT path, but still clamped so a
    // caller with a bogus value gets a stored 5 rather than a CHECK failure.
    up.bindValue(QStringLiteral(":rating"), std::clamp(image.rating, 0, 5));
    up.bindValue(QStringLiteral(":flag"),
                 std::clamp(static_cast<int>(image.flag), -1, 1));
    up.bindValue(QStringLiteral(":color_label"),
                 std::clamp(static_cast<int>(image.colorLabel), 0, 5));
    up.bindValue(QStringLiteral(":look_json"), textOrEmpty(image.lookJson));
    up.bindValue(QStringLiteral(":imported_at"),
                 image.importedAt.isValid() ? image.importedAt.toSecsSinceEpoch()
                                            : QDateTime::currentDateTime().toSecsSinceEpoch());

    if (!up.exec()) {
        d->fail(QStringLiteral("upsertImage: %1 failed").arg(relativePath), up);
        up.finish();
        return -1;
    }
    up.finish();

    // lastInsertId() is only meaningful on the INSERT path, so ask for the id
    // explicitly. This is an equality probe on the UNIQUE index, not a scan.
    if (!d->selectIdStmt) {
        d->selectIdStmt = std::make_unique<QSqlQuery>(db);
        if (!d->selectIdStmt->prepare(QStringLiteral(
                "SELECT id FROM images "
                "WHERE folder_id = :folder_id AND relative_path = :relative_path"))) {
            d->fail(QStringLiteral("upsertImage: id lookup prepare failed"), *d->selectIdStmt);
            d->selectIdStmt.reset();
            return -1;
        }
    }

    QSqlQuery& sel = *d->selectIdStmt;
    sel.bindValue(QStringLiteral(":folder_id"), image.folderId);
    sel.bindValue(QStringLiteral(":relative_path"), relativePath);
    if (!sel.exec() || !sel.next()) {
        d->fail(QStringLiteral("upsertImage: id lookup failed"), sel);
        sel.finish();
        return -1;
    }
    const qint64 id = sel.value(0).toLongLong();
    sel.finish();
    return id;
}

bool CatalogDatabase::removeImage(qint64 imageId)
{
    if (!d->requireOpen(QStringLiteral("removeImage")))
        return false;

    QSqlDatabase db = d->handle();
    QSqlQuery    q(db);
    q.prepare(QStringLiteral("DELETE FROM images WHERE id = :id"));
    q.bindValue(QStringLiteral(":id"), imageId);
    if (!q.exec())
        return d->fail(QStringLiteral("removeImage: delete failed"), q);
    if (q.numRowsAffected() == 0)
        return d->fail(QStringLiteral("removeImage: no image with id %1").arg(imageId));
    return true;
}

CatalogImage CatalogDatabase::image(qint64 imageId) const
{
    CatalogImage result;
    if (!d->requireOpen(QStringLiteral("image")))
        return result;

    QSqlDatabase db = d->handle();
    QSqlQuery    q(db);
    q.setForwardOnly(true);
    q.prepare(QStringLiteral("SELECT ") + imageColumnList() + imageFromClause()
              + QStringLiteral(" WHERE i.id = :id"));
    q.bindValue(QStringLiteral(":id"), imageId);
    if (!q.exec()) {
        d->fail(QStringLiteral("image: query failed"), q);
        return result;
    }
    if (!q.next()) {
        d->fail(QStringLiteral("image: no image with id %1").arg(imageId));
        return result;
    }
    result          = readImageRow(q);
    result.keywords = keywordsFor(result.id);
    return result;
}

CatalogImage CatalogDatabase::imageByPath(const QString& absolutePath) const
{
    CatalogImage result;
    if (!d->requireOpen(QStringLiteral("imageByPath")))
        return result;

    const QString normalized = normalizeFilePath(absolutePath);
    if (normalized.isEmpty()) {
        d->fail(QStringLiteral("imageByPath: empty path"));
        return result;
    }

    // Rather than matching on (folders.path || '/' || relative_path), which no
    // index can serve, split the path against the (few) known roots and do an
    // equality probe on UNIQUE(folder_id, relative_path). Longest root first so
    // that nested roots resolve to the most specific one.
    QVector<CatalogFolder> roots = folders(false);
    std::sort(roots.begin(), roots.end(),
              [](const CatalogFolder& a, const CatalogFolder& b) {
                  return a.path.length() > b.path.length();
              });

    // On Windows the stored relative path may differ in case from what the
    // caller passed; NOCASE there matches the filesystem's own semantics.
    const QString comparison = (kPathCase == Qt::CaseInsensitive)
        ? QStringLiteral(" AND i.relative_path = :relative_path COLLATE NOCASE")
        : QStringLiteral(" AND i.relative_path = :relative_path");

    QSqlDatabase db = d->handle();

    for (const CatalogFolder& root : roots) {
        if (root.path.isEmpty())
            continue;
        const QString prefix = root.path.endsWith(QLatin1Char('/'))
            ? root.path
            : root.path + QLatin1Char('/');
        if (!normalized.startsWith(prefix, kPathCase))
            continue;

        const QString relative = normalizeRelativePath(normalized.mid(prefix.length()));
        if (relative.isEmpty())
            continue;

        QSqlQuery q(db);
        q.setForwardOnly(true);
        q.prepare(QStringLiteral("SELECT ") + imageColumnList() + imageFromClause()
                  + QStringLiteral(" WHERE i.folder_id = :folder_id") + comparison);
        q.bindValue(QStringLiteral(":folder_id"), root.id);
        q.bindValue(QStringLiteral(":relative_path"), relative);
        if (!q.exec()) {
            d->fail(QStringLiteral("imageByPath: query failed"), q);
            return result;
        }
        if (q.next()) {
            result          = readImageRow(q);
            result.keywords = keywordsFor(result.id);
            return result;
        }
    }

    d->fail(QStringLiteral("imageByPath: %1 is not in the catalog").arg(normalized));
    return result;
}

bool CatalogDatabase::hasImage(qint64 folderId, const QString& relativePath) const
{
    if (!d->requireOpen(QStringLiteral("hasImage")))
        return false;

    const QString normalized = normalizeRelativePath(relativePath);
    if (normalized.isEmpty())
        return d->fail(QStringLiteral("hasImage: relativePath is empty"));

    QSqlDatabase db = d->handle();
    if (!d->hasImageStmt) {
        d->hasImageStmt = std::make_unique<QSqlQuery>(db);
        d->hasImageStmt->setForwardOnly(true);
        if (!d->hasImageStmt->prepare(QStringLiteral(
                "SELECT 1 FROM images "
                "WHERE folder_id = :folder_id AND relative_path = :relative_path LIMIT 1"))) {
            d->fail(QStringLiteral("hasImage: prepare failed"), *d->hasImageStmt);
            d->hasImageStmt.reset();
            return false;
        }
    }

    QSqlQuery& q = *d->hasImageStmt;
    q.bindValue(QStringLiteral(":folder_id"), folderId);
    q.bindValue(QStringLiteral(":relative_path"), normalized);
    if (!q.exec()) {
        d->fail(QStringLiteral("hasImage: query failed"), q);
        q.finish();
        return false;
    }
    const bool found = q.next();
    q.finish();
    return found;
}

// ==============================================================================
// The grid query
// ==============================================================================
QVector<CatalogImage> CatalogDatabase::query(const CatalogFilter& filter,
                                             int limit,
                                             int offset) const
{
    QVector<CatalogImage> result;
    if (!d->requireOpen(QStringLiteral("query")))
        return result;

    const Predicate predicate = buildPredicate(filter);
    const QString   direction =
        filter.ascending ? QStringLiteral(" ASC") : QStringLiteral(" DESC");

    // The trailing id keeps the order total, so paging with LIMIT/OFFSET can
    // never repeat or skip a row when two photos share a capture time. It is
    // free: the rowid is implicitly the last column of every SQLite index, so
    // the sort still resolves inside the index.
    const QString sql = QStringLiteral("SELECT ") + imageColumnList() + imageFromClause()
        + predicate.sql
        + QStringLiteral(" ORDER BY ") + sortExpression(filter.sortKey) + direction
        + QStringLiteral(", i.id") + direction
        + QStringLiteral(" LIMIT ? OFFSET ?");

    QSqlDatabase db = d->handle();
    QSqlQuery    q(db);
    // Forward-only stops the SQLite driver caching every fetched record, which
    // matters when the caller asks for all 40,000 rows.
    q.setForwardOnly(true);
    if (!q.prepare(sql)) {
        d->fail(QStringLiteral("query: prepare failed"), q);
        return result;
    }
    bindAll(q, predicate.binds);
    // SQLite reads a negative LIMIT as "no limit", which is exactly the
    // contract's limit == -1.
    q.addBindValue(limit < 0 ? -1 : limit);
    q.addBindValue(std::max(0, offset));

    if (!q.exec()) {
        d->fail(QStringLiteral("query: exec failed"), q);
        return result;
    }
    if (limit > 0)
        result.reserve(limit);
    while (q.next())
        result.push_back(readImageRow(q));

    d->attachKeywords(result);
    return result;
}

int CatalogDatabase::queryCount(const CatalogFilter& filter) const
{
    if (!d->requireOpen(QStringLiteral("queryCount")))
        return 0;

    // Same predicate builder, same FROM clause. The LEFT JOIN cannot change the
    // row count (folders.id is unique), and SQLite drops it from the plan here
    // because no output column needs it, so this stays a cheap count.
    const Predicate predicate = buildPredicate(filter);
    const QString   sql =
        QStringLiteral("SELECT COUNT(*)") + imageFromClause() + predicate.sql;

    QSqlDatabase db = d->handle();
    QSqlQuery    q(db);
    q.setForwardOnly(true);
    if (!q.prepare(sql)) {
        d->fail(QStringLiteral("queryCount: prepare failed"), q);
        return 0;
    }
    bindAll(q, predicate.binds);
    if (!q.exec() || !q.next()) {
        d->fail(QStringLiteral("queryCount: exec failed"), q);
        return 0;
    }
    return q.value(0).toInt();
}

// ==============================================================================
// User data
// ==============================================================================
bool CatalogDatabase::setRating(qint64 imageId, int rating)
{
    if (!d->requireOpen(QStringLiteral("setRating")))
        return false;

    QSqlDatabase db = d->handle();
    QSqlQuery    q(db);
    q.prepare(QStringLiteral("UPDATE images SET rating = :rating WHERE id = :id"));
    q.bindValue(QStringLiteral(":rating"), std::clamp(rating, 0, 5));
    q.bindValue(QStringLiteral(":id"), imageId);
    if (!q.exec())
        return d->fail(QStringLiteral("setRating: update failed"), q);
    if (q.numRowsAffected() == 0)
        return d->fail(QStringLiteral("setRating: no image with id %1").arg(imageId));
    return true;
}

bool CatalogDatabase::setFlag(qint64 imageId, ImageFlag flag)
{
    if (!d->requireOpen(QStringLiteral("setFlag")))
        return false;

    QSqlDatabase db = d->handle();
    QSqlQuery    q(db);
    q.prepare(QStringLiteral("UPDATE images SET flag = :flag WHERE id = :id"));
    q.bindValue(QStringLiteral(":flag"), std::clamp(static_cast<int>(flag), -1, 1));
    q.bindValue(QStringLiteral(":id"), imageId);
    if (!q.exec())
        return d->fail(QStringLiteral("setFlag: update failed"), q);
    if (q.numRowsAffected() == 0)
        return d->fail(QStringLiteral("setFlag: no image with id %1").arg(imageId));
    return true;
}

bool CatalogDatabase::setColorLabel(qint64 imageId, ColorLabel label)
{
    if (!d->requireOpen(QStringLiteral("setColorLabel")))
        return false;

    QSqlDatabase db = d->handle();
    QSqlQuery    q(db);
    q.prepare(QStringLiteral("UPDATE images SET color_label = :label WHERE id = :id"));
    q.bindValue(QStringLiteral(":label"), std::clamp(static_cast<int>(label), 0, 5));
    q.bindValue(QStringLiteral(":id"), imageId);
    if (!q.exec())
        return d->fail(QStringLiteral("setColorLabel: update failed"), q);
    if (q.numRowsAffected() == 0)
        return d->fail(QStringLiteral("setColorLabel: no image with id %1").arg(imageId));
    return true;
}

bool CatalogDatabase::setLookJson(qint64 imageId, const QString& lookJson)
{
    if (!d->requireOpen(QStringLiteral("setLookJson")))
        return false;

    QSqlDatabase db = d->handle();
    QSqlQuery    q(db);
    q.prepare(QStringLiteral("UPDATE images SET look_json = :look WHERE id = :id"));
    // Clearing a look is legitimate, and a null QString would bind as SQL NULL
    // against a NOT NULL column.
    q.bindValue(QStringLiteral(":look"), textOrEmpty(lookJson));
    q.bindValue(QStringLiteral(":id"), imageId);
    if (!q.exec())
        return d->fail(QStringLiteral("setLookJson: update failed"), q);
    if (q.numRowsAffected() == 0)
        return d->fail(QStringLiteral("setLookJson: no image with id %1").arg(imageId));
    return true;
}

// ==============================================================================
// Keywords
// ==============================================================================
bool CatalogDatabase::addKeyword(qint64 imageId, const QString& keyword)
{
    if (!d->requireOpen(QStringLiteral("addKeyword")))
        return false;

    const QString name = keyword.trimmed();
    if (name.isEmpty())
        return d->fail(QStringLiteral("addKeyword: empty keyword"));

    QSqlDatabase db = d->handle();

    QSqlQuery insertKeyword(db);
    insertKeyword.prepare(
        QStringLiteral("INSERT OR IGNORE INTO keywords (name) VALUES (:name)"));
    insertKeyword.bindValue(QStringLiteral(":name"), name);
    if (!insertKeyword.exec())
        return d->fail(QStringLiteral("addKeyword: cannot create keyword"), insertKeyword);

    // Re-read rather than trusting lastInsertId(): OR IGNORE inserts nothing
    // when the keyword already exists, and the column is NOCASE, so the stored
    // spelling may differ from what was passed in.
    QSqlQuery lookup(db);
    lookup.prepare(QStringLiteral("SELECT id FROM keywords WHERE name = :name"));
    lookup.bindValue(QStringLiteral(":name"), name);
    if (!lookup.exec() || !lookup.next())
        return d->fail(QStringLiteral("addKeyword: cannot resolve keyword id"), lookup);
    const qint64 keywordId = lookup.value(0).toLongLong();
    lookup.finish();

    // OR IGNORE covers "already tagged". A bad imageId is a foreign-key
    // violation, which OR IGNORE does *not* suppress, so it still errors.
    QSqlQuery link(db);
    link.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO image_keywords (image_id, keyword_id) "
        "VALUES (:image_id, :keyword_id)"));
    link.bindValue(QStringLiteral(":image_id"), imageId);
    link.bindValue(QStringLiteral(":keyword_id"), keywordId);
    if (!link.exec())
        return d->fail(QStringLiteral("addKeyword: cannot tag image %1").arg(imageId), link);
    return true;
}

bool CatalogDatabase::removeKeyword(qint64 imageId, const QString& keyword)
{
    if (!d->requireOpen(QStringLiteral("removeKeyword")))
        return false;

    const QString name = keyword.trimmed();
    if (name.isEmpty())
        return d->fail(QStringLiteral("removeKeyword: empty keyword"));

    QSqlDatabase db = d->handle();

    QSqlQuery unlink(db);
    unlink.prepare(QStringLiteral(
        "DELETE FROM image_keywords "
        "WHERE image_id = :image_id "
        "  AND keyword_id = (SELECT id FROM keywords WHERE name = :name)"));
    unlink.bindValue(QStringLiteral(":image_id"), imageId);
    unlink.bindValue(QStringLiteral(":name"), name);
    if (!unlink.exec())
        return d->fail(QStringLiteral("removeKeyword: delete failed"), unlink);
    if (unlink.numRowsAffected() == 0) {
        return d->fail(QStringLiteral("removeKeyword: image %1 is not tagged '%2'")
                           .arg(imageId)
                           .arg(name));
    }

    // Drop the keyword itself once nothing references it, so the keyword filter
    // in the UI never fills up with tags the user has already removed.
    QSqlQuery gc(db);
    gc.prepare(QStringLiteral(
        "DELETE FROM keywords WHERE name = :name "
        "AND NOT EXISTS (SELECT 1 FROM image_keywords WHERE keyword_id = keywords.id)"));
    gc.bindValue(QStringLiteral(":name"), name);
    if (!gc.exec())
        return d->fail(QStringLiteral("removeKeyword: cleanup failed"), gc);
    return true;
}

QStringList CatalogDatabase::keywordsFor(qint64 imageId) const
{
    QStringList result;
    if (!d->requireOpen(QStringLiteral("keywordsFor")))
        return result;

    QSqlDatabase db = d->handle();
    QSqlQuery    q(db);
    q.setForwardOnly(true);
    q.prepare(QStringLiteral(
        "SELECT k.name FROM image_keywords ik "
        "JOIN keywords k ON k.id = ik.keyword_id "
        "WHERE ik.image_id = :image_id ORDER BY k.name COLLATE NOCASE"));
    q.bindValue(QStringLiteral(":image_id"), imageId);
    if (!q.exec()) {
        d->fail(QStringLiteral("keywordsFor: query failed"), q);
        return result;
    }
    while (q.next())
        result.append(q.value(0).toString());
    return result;
}

QStringList CatalogDatabase::allKeywords() const
{
    QStringList result;
    if (!d->requireOpen(QStringLiteral("allKeywords")))
        return result;

    QSqlDatabase db = d->handle();
    QSqlQuery    q(db);
    q.setForwardOnly(true);
    if (!q.exec(QStringLiteral("SELECT name FROM keywords ORDER BY name COLLATE NOCASE"))) {
        d->fail(QStringLiteral("allKeywords: query failed"), q);
        return result;
    }
    while (q.next())
        result.append(q.value(0).toString());
    return result;
}

// ==============================================================================
// Collections
// ==============================================================================
qint64 CatalogDatabase::createCollection(const QString& name)
{
    if (!d->requireOpen(QStringLiteral("createCollection")))
        return -1;

    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) {
        d->fail(QStringLiteral("createCollection: empty name"));
        return -1;
    }

    QSqlDatabase db = d->handle();

    // Idempotent, like addFolder(): an existing name returns its id. A UI that
    // wants to reject duplicates can check with the id it gets back.
    QSqlQuery existing(db);
    existing.prepare(QStringLiteral("SELECT id FROM collections WHERE name = :name"));
    existing.bindValue(QStringLiteral(":name"), trimmed);
    if (!existing.exec()) {
        d->fail(QStringLiteral("createCollection: lookup failed"), existing);
        return -1;
    }
    if (existing.next())
        return existing.value(0).toLongLong();
    existing.finish();

    QSqlQuery insert(db);
    insert.prepare(QStringLiteral(
        "INSERT INTO collections (name, created_at) VALUES (:name, :created_at)"));
    insert.bindValue(QStringLiteral(":name"), trimmed);
    insert.bindValue(QStringLiteral(":created_at"),
                     QDateTime::currentDateTime().toSecsSinceEpoch());
    if (!insert.exec()) {
        d->fail(QStringLiteral("createCollection: insert failed"), insert);
        return -1;
    }
    return insert.lastInsertId().toLongLong();
}

bool CatalogDatabase::deleteCollection(qint64 collectionId)
{
    if (!d->requireOpen(QStringLiteral("deleteCollection")))
        return false;

    QSqlDatabase db = d->handle();
    QSqlQuery    q(db);
    q.prepare(QStringLiteral("DELETE FROM collections WHERE id = :id"));
    q.bindValue(QStringLiteral(":id"), collectionId);
    if (!q.exec())
        return d->fail(QStringLiteral("deleteCollection: delete failed"), q);
    if (q.numRowsAffected() == 0) {
        return d->fail(
            QStringLiteral("deleteCollection: no collection with id %1").arg(collectionId));
    }
    return true;
}

bool CatalogDatabase::addToCollection(qint64 collectionId, qint64 imageId)
{
    if (!d->requireOpen(QStringLiteral("addToCollection")))
        return false;

    QSqlDatabase db = d->handle();
    QSqlQuery    q(db);
    q.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO collection_images (collection_id, image_id) "
        "VALUES (:collection_id, :image_id)"));
    q.bindValue(QStringLiteral(":collection_id"), collectionId);
    q.bindValue(QStringLiteral(":image_id"), imageId);
    if (!q.exec()) {
        return d->fail(QStringLiteral("addToCollection: cannot add image %1 to collection %2")
                           .arg(imageId)
                           .arg(collectionId),
                       q);
    }
    return true;
}

bool CatalogDatabase::removeFromCollection(qint64 collectionId, qint64 imageId)
{
    if (!d->requireOpen(QStringLiteral("removeFromCollection")))
        return false;

    QSqlDatabase db = d->handle();
    QSqlQuery    q(db);
    q.prepare(QStringLiteral(
        "DELETE FROM collection_images "
        "WHERE collection_id = :collection_id AND image_id = :image_id"));
    q.bindValue(QStringLiteral(":collection_id"), collectionId);
    q.bindValue(QStringLiteral(":image_id"), imageId);
    if (!q.exec())
        return d->fail(QStringLiteral("removeFromCollection: delete failed"), q);
    if (q.numRowsAffected() == 0) {
        return d->fail(QStringLiteral("removeFromCollection: image %1 is not in collection %2")
                           .arg(imageId)
                           .arg(collectionId));
    }
    return true;
}

// ==============================================================================
// Misc
// ==============================================================================
CatalogStats CatalogDatabase::stats() const
{
    CatalogStats result;
    if (!d->requireOpen(QStringLiteral("stats")))
        return result;

    QSqlDatabase db = d->handle();

    QSqlQuery folderCount(db);
    folderCount.setForwardOnly(true);
    if (!folderCount.exec(QStringLiteral("SELECT COUNT(*) FROM folders"))
        || !folderCount.next()) {
        d->fail(QStringLiteral("stats: folder count failed"), folderCount);
        return result;
    }
    result.folderCount = folderCount.value(0).toInt();
    folderCount.finish();

    // One pass over images; SUM over an empty table is NULL, hence COALESCE.
    QSqlQuery imageCounts(db);
    imageCounts.setForwardOnly(true);
    if (!imageCounts.exec(QStringLiteral(
            "SELECT COUNT(*),"
            "       COALESCE(SUM(is_raw <> 0), 0),"
            "       COALESCE(SUM(flag = 1), 0),"
            "       COALESCE(SUM(flag = -1), 0) "
            "FROM images"))
        || !imageCounts.next()) {
        d->fail(QStringLiteral("stats: image counts failed"), imageCounts);
        return result;
    }
    result.imageCount    = imageCounts.value(0).toInt();
    result.rawCount      = imageCounts.value(1).toInt();
    result.pickedCount   = imageCounts.value(2).toInt();
    result.rejectedCount = imageCounts.value(3).toInt();
    return result;
}

bool CatalogDatabase::transaction()
{
    if (!d->requireOpen(QStringLiteral("transaction")))
        return false;

    QSqlDatabase db = d->handle();
    if (!db.transaction())
        return d->fail(QStringLiteral("transaction: begin failed"), db.lastError());
    return true;
}

bool CatalogDatabase::commit()
{
    if (!d->requireOpen(QStringLiteral("commit")))
        return false;

    QSqlDatabase db = d->handle();
    if (!db.commit())
        return d->fail(QStringLiteral("commit failed"), db.lastError());
    return true;
}

bool CatalogDatabase::rollback()
{
    if (!d->requireOpen(QStringLiteral("rollback")))
        return false;

    QSqlDatabase db = d->handle();
    if (!db.rollback())
        return d->fail(QStringLiteral("rollback failed"), db.lastError());
    return true;
}

bool CatalogFilter::isDefault() const
{
    return folderId < 0 && minRating == 0 && !pickedOnly && hideRejected
        && colorLabel == ColorLabel::None && searchText.isEmpty()
        && keywords.isEmpty() && sortKey == SortKey::CaptureTime && !ascending;
}

} // namespace lps
