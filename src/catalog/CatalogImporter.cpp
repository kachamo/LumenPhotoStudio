// ==============================================================================
// catalog/CatalogImporter.cpp
// Background folder scan and import.
//
// Shape of a run
//   startImport() validates nothing but the "already running" case, emits
//   started() synchronously on the calling thread, then hands everything to a
//   worker thread. The worker opens ITS OWN CatalogDatabase — a QSqlDatabase
//   connection belongs to the thread that opened it, so borrowing the UI's
//   instance would be a data race dressed up as a shortcut — scans, imports,
//   and exits. finished() is emitted from the UI thread once the worker has
//   been joined, so a slot connected to finished() may immediately start
//   another import.
//
// Two phases
//   Scanning first, so `total` is a real number for the whole of the import
//   phase and the progress bar does not have to guess. During the scan the
//   header contract is progress(current, 0, file); `total` only becomes
//   non-zero once counting is done.
//
// Transactions
//   Committed every kBatchRows inserts. One transaction per row costs an fsync
//   each and turns a 30-second import into a ten-minute one; one transaction
//   for all 40,000 rows means a cancel (or a power cut) at 39,000 throws away
//   everything. A few hundred rows per commit gets almost all of the speed and
//   bounds the loss to the current batch.
//
// Cancellation
//   An atomic flag, tested once per file. cancel() is a plain store, so it is
//   safe from any thread and never blocks the caller. The in-flight transaction
//   is committed — not rolled back — so the work already done survives.
//
// Thumbnails are deliberately NOT generated here. Decoding 40,000 images would
// dominate the import by orders of magnitude for tiles the user may never
// scroll to; ThumbnailCache is driven lazily by the grid instead.
// ==============================================================================
#include "catalog/CatalogImporter.h"

#include "catalog/CatalogDatabase.h"
#include "io/ImageMetadataReader.h"
#include "io/RawImageLoader.h"

#include <QAtomicInt>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImageIOHandler>
#include <QImageReader>
#include <QSet>
#include <QSize>
#include <QThread>

namespace {

// Rows per transaction. See the banner comment.
constexpr int kBatchRows = 250;

// Minimum gap between progress() emissions. A 40,000 file import would
// otherwise post 40,000 queued events at the UI thread, and any receiver that
// repaints on each one turns the import into a slideshow. The first and last
// tick of each phase are always emitted so the bar starts at a sane place and
// finishes exactly on `total`.
constexpr int kProgressIntervalMs = 40;

// File timestamps survive a database round trip with whatever precision the
// storage format has — typically whole seconds. Comparing QDateTime exactly
// would then report every file as modified and make re-import as expensive as
// a first import, which is the one thing the skip path exists to prevent.
bool sameFileStamp(const QDateTime& a, const QDateTime& b)
{
    if (!a.isValid() || !b.isValid())
        return false;
    return qAbs(a.toUTC().toSecsSinceEpoch() - b.toUTC().toSecsSinceEpoch()) <= 1;
}

// ImageMetadataReader hands back strings, and what is in an EXIF DateTimeOriginal
// field is whatever the camera firmware felt like writing. Try the formats we
// know, then give up and leave the QDateTime invalid — CatalogTypes documents
// captureTime as "invalid if unknown", and a wrong date is worse than none.
QDateTime parseCaptureTime(const QString& text)
{
    const QString value = text.trimmed();
    if (value.isEmpty())
        return QDateTime();

    static const char* const kFormats[] = {
        "yyyy-MM-dd HH:mm:ss",     // what ImageMetadataReader normalizes to
        "yyyy:MM:dd HH:mm:ss",     // raw EXIF
        "yyyy-MM-ddTHH:mm:ss",
        "yyyy-MM-dd HH:mm",
        "yyyy:MM:dd",
        "yyyy-MM-dd",
    };
    for (const char* format : kFormats) {
        const QDateTime parsed =
            QDateTime::fromString(value, QString::fromLatin1(format));
        if (parsed.isValid())
            return parsed;
    }

    QDateTime parsed = QDateTime::fromString(value, Qt::ISODate);
    if (parsed.isValid())
        return parsed;
    return QDateTime::fromString(value, Qt::ISODateWithMs);
}

} // namespace

namespace lps {

struct CatalogImporter::Impl
{
    CatalogImporter* q = nullptr;
    QString          catalogPath;

    // Owned outright. Created in startImport(), joined and deleted either in
    // the finished-handler or in ~CatalogImporter, whichever comes first.
    QThread* thread = nullptr;

    QAtomicInt cancelRequested{ 0 };
    QAtomicInt running{ 0 };

    // Written by the worker, read by the UI thread only after the worker has
    // been joined. QThread::finished plus the queued connection that carries it
    // are the synchronisation point, so no lock is required.
    int  imported  = 0;
    int  skipped   = 0;
    int  failed    = 0;
    bool cancelled = false;

    enum class Outcome { Failed, Skipped, Imported };

    void runImport(const QString& folderPath, bool recursive);

    // Everything below runs on the worker thread.
    QStringList scan(const QString& rootPath, bool recursive);
    Outcome     importOne(CatalogDatabase& db,
                          const QDir&      root,
                          qint64           folderId,
                          const QString&   path,
                          bool&            inTransaction);
};

// ==============================================================================
// Construction and teardown
// ==============================================================================

CatalogImporter::CatalogImporter(QObject* parent)
    : QObject(parent), d(new Impl)
{
    d->q = this;
}

CatalogImporter::~CatalogImporter()
{
    // Never leave a detached thread writing to a database while its owner is
    // being destroyed. Ask it to stop, then join it — the flag is checked once
    // per file, so this returns in roughly the time one file takes.
    d->cancelRequested.storeRelease(1);
    if (d->thread) {
        d->thread->wait();
        delete d->thread;
        d->thread = nullptr;
    }
    delete d;
}

// ==============================================================================
// Static file-type helpers
// ==============================================================================

QStringList CatalogImporter::supportedExtensions()
{
    // Raster formats Qt reads, plus the RAW extensions RawImageLoader accepts.
    return {
        QStringLiteral("jpg"),  QStringLiteral("jpeg"), QStringLiteral("png"),
        QStringLiteral("tif"),  QStringLiteral("tiff"), QStringLiteral("bmp"),
        QStringLiteral("webp"),
        QStringLiteral("cr2"),  QStringLiteral("cr3"),  QStringLiteral("nef"),
        QStringLiteral("arw"),  QStringLiteral("dng"),  QStringLiteral("raf"),
        QStringLiteral("orf"),  QStringLiteral("rw2"),
    };
}

bool CatalogImporter::isSupportedFile(const QString& path)
{
    return supportedExtensions().contains(QFileInfo(path).suffix().toLower());
}

// ==============================================================================
// Public control surface
// ==============================================================================

bool CatalogImporter::isRunning() const
{
    return d->running.loadAcquire() != 0;
}

void CatalogImporter::setCatalogPath(const QString& path) { d->catalogPath = path; }

void CatalogImporter::startImport(const QString& folderPath, bool recursive)
{
    if (d->running.loadAcquire() != 0)
        return;                          // Documented: ignored while running.

    d->running.storeRelease(1);
    d->cancelRequested.storeRelease(0);
    d->imported  = 0;
    d->skipped   = 0;
    d->failed    = 0;
    d->cancelled = false;

    emit started(folderPath);

    // The catalog path is snapshotted here, on the caller's thread, so a later
    // setCatalogPath() cannot retarget a run that is already under way.
    const QString folder = folderPath;
    const bool    deep   = recursive;

    d->thread = QThread::create([this, folder, deep]() { d->runImport(folder, deep); });
    d->thread->setObjectName(QStringLiteral("lps-catalog-import"));

    // Queued onto this object's thread, so by the time a client sees
    // finished() the worker is joined, deleted and the importer is idle again —
    // starting a new import from inside a finished() handler is legal.
    QObject::connect(
        d->thread, &QThread::finished, this,
        [this]() {
            QThread* worker = d->thread;
            d->thread = nullptr;
            if (worker) {
                worker->wait();
                worker->deleteLater();
            }
            d->running.storeRelease(0);
            emit finished(d->imported, d->skipped, d->failed, d->cancelled);
        },
        Qt::QueuedConnection);

    d->thread->start();
}

void CatalogImporter::cancel()
{
    // A single relaxed-ish store: callable from any thread, never blocks, and
    // the worker sees it at its next per-file check.
    d->cancelRequested.storeRelease(1);
}

// ==============================================================================
// Worker thread
// ==============================================================================

QStringList CatalogImporter::Impl::scan(const QString& rootPath, bool recursive)
{
    // supportedExtensions() rebuilds a QStringList on every call and
    // isSupportedFile() does a linear scan of it; that is fine for a one-off
    // question and wasteful 40,000 times in a row. Hoist the same list into a
    // set once, so the public helpers stay the single source of truth.
    const QStringList  extensionList = CatalogImporter::supportedExtensions();
    const QSet<QString> extensions(extensionList.cbegin(), extensionList.cend());

    QStringList files;
    files.reserve(4096);

    QElapsedTimer throttle;
    throttle.start();

    // FollowSymlinks is deliberately not set: a symlinked parent directory
    // would otherwise let a 40,000 file scan run forever.
    QDirIterator it(rootPath,
                    QDir::Files | QDir::NoDotAndDotDot,
                    recursive ? QDirIterator::Subdirectories
                              : QDirIterator::NoIteratorFlags);
    while (it.hasNext()) {
        const QString path = it.next();
        if (cancelRequested.loadAcquire()) {
            cancelled = true;
            break;
        }
        if (!extensions.contains(QFileInfo(path).suffix().toLower()))
            continue;

        files.append(path);
        if (throttle.elapsed() >= kProgressIntervalMs) {
            throttle.restart();
            // total == 0: the contract for "still counting".
            emit q->progress(files.size(), 0, path);
        }
    }

    if (!files.isEmpty())
        emit q->progress(files.size(), 0, files.constLast());

    return files;
}

// One file. `inTransaction` is opened lazily, immediately before the first
// write, so a re-import that skips every file never holds a transaction open.
CatalogImporter::Impl::Outcome CatalogImporter::Impl::importOne(
    CatalogDatabase& db,
    const QDir&      root,
    qint64           folderId,
    const QString&   path,
    bool&            inTransaction)
{
    const QFileInfo info(path);
    if (!info.isFile() || !info.isReadable())
        return Outcome::Failed;          // Vanished or permission-denied mid-run.

    const QString   absolutePath = info.absoluteFilePath();
    const qint64    fileSize     = info.size();
    const QDateTime fileModified = info.lastModified();

    // Cheap re-import. imageByPath() answers both "is it there?" and "has it
    // changed?" in one statement; hasImage() would only answer the first and
    // cost a second round trip on exactly the files we most want to skip fast.
    const CatalogImage existing = db.imageByPath(absolutePath);
    if (existing.isValid() && existing.fileSize == fileSize &&
        sameFileStamp(existing.fileModified, fileModified)) {
        return Outcome::Skipped;
    }

    CatalogImage image;
    image.folderId     = folderId;
    image.absolutePath = absolutePath;
    image.fileName     = info.fileName();
    image.relativePath = root.relativeFilePath(absolutePath);
    image.fileSize     = fileSize;
    image.fileModified = fileModified;
    image.isRaw        = RawImageLoader::isRawExtension(absolutePath);

    // Header-only. QImageReader::size() does not decode, which is the whole
    // point — decoding 40,000 images to learn their dimensions would take
    // hours.
    QImageReader reader(absolutePath);
    reader.setAutoTransform(true);
    const QSize dimensions = reader.size();
    if (dimensions.isValid() && !dimensions.isEmpty()) {
        // size() is reported in the file's own coordinate system; a portrait
        // shot from a landscape sensor carries an EXIF rotation the grid will
        // apply, so store the dimensions the user will actually see.
        const bool quarterTurn =
            reader.transformation().testFlag(QImageIOHandler::TransformationRotate90);
        image.width  = quarterTurn ? dimensions.height() : dimensions.width();
        image.height = quarterTurn ? dimensions.width()  : dimensions.height();
    } else if (!image.isRaw && !reader.canRead()) {
        // Truncated, corrupt, or a format this build has no plugin for. One bad
        // file must not abort a 40,000 file import, so report it and move on.
        return Outcome::Failed;
    }
    // RAW keeps width/height at 0 when LibRaw is unavailable: QImageReader
    // cannot parse a CR3 header and guessing would be worse than admitting it.

    const ImageMetadata meta = ImageMetadataReader::read(absolutePath);
    image.cameraModel  = meta.cameraModel;
    image.lensModel    = meta.lensModel;
    image.iso          = meta.iso;
    image.aperture     = meta.aperture;
    image.shutterSpeed = meta.shutterSpeed;
    image.focalLength  = meta.focalLength;
    image.captureTime  = parseCaptureTime(meta.captureDateTime);
    image.importedAt   = QDateTime::currentDateTime();

    // Opened here, not at the top of the loop: skipped files must not drag a
    // transaction along behind them.
    if (!inTransaction)
        inTransaction = db.transaction();   // Unbatched if it refuses; still correct.

    // id is left at -1 on purpose: upsertImage() matches on
    // (folderId, relativePath) and preserves rating/flag/label/keywords/look
    // itself, so handing it an id would only give it a second, redundant way to
    // identify the row.
    const qint64 id = db.upsertImage(image);
    if (id < 0)
        return Outcome::Failed;

    emit q->imageImported(id, absolutePath);
    return Outcome::Imported;
}

void CatalogImporter::Impl::runImport(const QString& folderPath, bool recursive)
{
    const QFileInfo folderInfo(folderPath);
    if (folderPath.isEmpty() || !folderInfo.isDir()) {
        emit q->failed(QStringLiteral("Not a folder: %1").arg(folderPath));
        return;                          // finished() still fires, from the join.
    }

    const QDir    root(folderInfo.absoluteFilePath());
    const QString rootPath = root.absolutePath();

    // Owned by this thread and nothing else: opened here, closed by its own
    // destructor when this function returns. This is the whole reason the
    // importer has a dedicated worker thread rather than a QtConcurrent task on
    // a pool thread — the connection's lifetime has to match the thread's
    // exactly, and a pool thread's lifetime is not ours to control.
    CatalogDatabase db;
    if (!db.open(catalogPath)) {
        emit q->failed(QStringLiteral("Cannot open catalog: %1").arg(db.lastError()));
        return;
    }

    const qint64 folderId = db.addFolder(rootPath);
    if (folderId < 0) {
        emit q->failed(QStringLiteral("Cannot register folder: %1").arg(db.lastError()));
        return;
    }

    // ---- phase 1: scan -------------------------------------------------------
    const QStringList files = scan(rootPath, recursive);
    if (cancelled)
        return;

    // ---- phase 2: import -----------------------------------------------------
    const int total = files.size();
    bool      inTransaction = false;
    int       rowsInBatch   = 0;

    QElapsedTimer throttle;
    throttle.start();

    for (int i = 0; i < total; ++i) {
        if (cancelRequested.loadAcquire()) {
            cancelled = true;
            break;
        }

        const QString& path      = files.at(i);
        const int      processed = i + 1;
        if (processed == 1 || processed == total ||
            throttle.elapsed() >= kProgressIntervalMs) {
            throttle.restart();
            emit q->progress(processed, total, path);
        }

        const Outcome outcome = importOne(db, root, folderId, path, inTransaction);
        switch (outcome) {
        case Outcome::Failed:   ++failed;   break;
        case Outcome::Skipped:  ++skipped;  break;
        case Outcome::Imported: ++imported; break;
        }

        if (outcome == Outcome::Imported && ++rowsInBatch >= kBatchRows) {
            if (inTransaction) {
                db.commit();
                inTransaction = false;
            }
            rowsInBatch = 0;
        }
    }

    // Commit rather than roll back, including on cancel: the user asked to
    // stop, not to undo. Half an imported folder is a useful result.
    if (inTransaction)
        db.commit();
}

} // namespace lps
