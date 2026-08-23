// ==============================================================================
// catalog/ThumbnailCache.cpp
// On-disk thumbnail cache.
//
// Layout
//   <cacheDir>/<first 2 hex chars of key>/<key>_<sizeTag>.<jpg|png>
//
// The two-character shard directory is not cosmetic. A 40,000 image library
// produces 80,000 thumbnail files across the two sizes; dropping them all into
// one directory makes every lookup, every listing and every delete slow on
// NTFS, ext4 and APFS alike. 256 shards keeps a large catalog at a few hundred
// entries per directory.
//
// Key derivation
//   SHA-1 over (absolutePath, fileModified-in-msecs-UTC, fileSize), each field
//   NUL-separated so a path ending in digits cannot collide with a different
//   path plus a different timestamp. Because the mtime and size are part of the
//   key, editing or replacing a file changes its key and the stale thumbnail is
//   simply never asked for again — no explicit invalidation step exists, and
//   none is needed. (The stale file is reclaimed by clear().)
//
// Format
//   JPEG at quality 85 for opaque images. PNG when the source carries an alpha
//   channel: a grid tile for a cut-out PNG or a 16-bit TIFF with transparency
//   would otherwise get a black or white matte baked in, which looks like a
//   rendering bug rather than a compression artefact. PNG thumbnails are larger
//   but they are a small minority of a photographic catalog.
//
// Threading
//   has()/get()/generate() are callable from any thread. cacheDir() is fixed at
//   construction so it needs no lock; the in-memory hot cache and the set of
//   shard directories already created are guarded by a mutex. Writes go through
//   QSaveFile, so two threads generating the same thumbnail concurrently
//   produce one intact file rather than an interleaved one.
// ==============================================================================
#include "catalog/ThumbnailCache.h"

#include "io/RawImageLoader.h"

#include <QByteArray>
#include <QCache>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QLatin1String>
#include <QMutex>
#include <QMutexLocker>
#include <QSaveFile>
#include <QSet>
#include <QSize>
#include <QStandardPaths>

namespace {

constexpr int kJpegQuality = 85;

// Budget for the in-memory hot cache, in KiB. A 256px grid tile is ~256 KiB
// decoded, so this holds roughly 250 tiles — enough for several screens of
// smooth scrolling without a disk round trip, and small enough to be invisible
// next to a single full-resolution edit buffer.
constexpr int kMemoryBudgetKiB = 64 * 1024;

// QStandardPaths can legitimately return nothing on a stripped-down system.
QString fallbackCacheRoot()
{
    return QDir::homePath() + QStringLiteral("/.lumen-photo-studio/cache");
}

QString sizeTag(lps::ThumbnailCache::Size size)
{
    switch (size) {
    case lps::ThumbnailCache::Size::Grid:    return QStringLiteral("g");
    case lps::ThumbnailCache::Size::Preview: return QStringLiteral("p");
    }
    return QString::number(static_cast<int>(size));
}

int sizePixels(lps::ThumbnailCache::Size size)
{
    return static_cast<int>(size);
}

// The bounding box a thumbnail must fit inside. Square, so it is independent of
// the source orientation — which matters because QImageReader applies the EXIF
// transform after scaling, in the untransformed coordinate system.
QSize boundingBox(lps::ThumbnailCache::Size size)
{
    const int edge = sizePixels(size);
    return QSize(edge, edge);
}

// Scales to fit `box` preserving aspect ratio, but never upscales: a 120px
// source stored as a 256px tile would be blurrier and four times the bytes.
QSize fitInside(const QSize& source, const QSize& box)
{
    if (!source.isValid() || source.isEmpty())
        return QSize();
    if (source.width() <= box.width() && source.height() <= box.height())
        return source;

    QSize scaled = source;
    scaled.scale(box, Qt::KeepAspectRatio);
    // scale() can round a very elongated panorama down to zero on one axis.
    return QSize(qMax(1, scaled.width()), qMax(1, scaled.height()));
}

// Writes through QSaveFile so a reader in another thread (or a crash mid-write)
// never sees a half-written thumbnail.
bool writeAtomically(const QString& path,
                     const QImage&  image,
                     const char*    format,
                     int            quality)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;

    QImageWriter writer(&file, QByteArray(format));
    if (quality >= 0)
        writer.setQuality(quality);

    if (!writer.write(image)) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

} // namespace

namespace lps {

struct ThumbnailCache::Impl
{
    QString dir;

    // Guards everything below it. cacheDir()/keyFor() touch none of it.
    mutable QMutex          mutex;
    mutable QCache<QString, QImage> memory{ kMemoryBudgetKiB };
    QSet<QString>           shardsCreated;

    QString shardDir(const QString& key) const
    {
        return QDir(dir).filePath(key.left(2));
    }

    // <cacheDir>/<shard>/<key>_<tag>
    QString stem(const QString& key, Size size) const
    {
        return QDir(shardDir(key)).filePath(key + QLatin1Char('_') + sizeTag(size));
    }

    QString jpegPath(const QString& key, Size size) const
    {
        return stem(key, size) + QStringLiteral(".jpg");
    }

    QString pngPath(const QString& key, Size size) const
    {
        return stem(key, size) + QStringLiteral(".png");
    }

    // Whichever of the two encodings is actually on disk, or an empty string.
    QString existingPath(const QString& key, Size size) const
    {
        const QString jpeg = jpegPath(key, size);
        if (QFileInfo::exists(jpeg))
            return jpeg;
        const QString png = pngPath(key, size);
        if (QFileInfo::exists(png))
            return png;
        return QString();
    }

    static QString memoryKey(const QString& key, Size size)
    {
        return key + QLatin1Char('/') + sizeTag(size);
    }

    QImage memoryLookup(const QString& key, Size size) const
    {
        QMutexLocker locker(&mutex);
        if (const QImage* hit = memory.object(memoryKey(key, size)))
            return *hit;
        return QImage();
    }

    void memoryStore(const QString& key, Size size, const QImage& image)
    {
        if (image.isNull())
            return;
        const int costKiB = qMax(1, static_cast<int>(image.sizeInBytes() / 1024));
        if (costKiB > kMemoryBudgetKiB)
            return;                     // Never evict the whole cache for one entry.
        QMutexLocker locker(&mutex);
        memory.insert(memoryKey(key, size), new QImage(image), costKiB);
    }

    // mkpath() is a syscall; a 40,000 file import would otherwise make 40,000
    // redundant ones. Remembering the shards we have already created reduces
    // that to at most 256.
    bool ensureShard(const QString& key)
    {
        const QString shard = shardDir(key);
        {
            QMutexLocker locker(&mutex);
            if (shardsCreated.contains(shard))
                return true;
        }
        if (!QDir().mkpath(shard))
            return false;
        QMutexLocker locker(&mutex);
        shardsCreated.insert(shard);
        return true;
    }
};

// ==============================================================================
// Construction
// ==============================================================================

ThumbnailCache::ThumbnailCache(const QString& cacheDir) : d(new Impl)
{
    d->dir = cacheDir.isEmpty() ? defaultCacheDir() : cacheDir;
}

ThumbnailCache::~ThumbnailCache() { delete d; }

QString ThumbnailCache::defaultCacheDir()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (base.isEmpty())
        base = fallbackCacheRoot();
    return QDir(base).filePath(QStringLiteral("thumbnails"));
}

QString ThumbnailCache::cacheDir() const { return d->dir; }

// ==============================================================================
// Key derivation
// ==============================================================================

QString ThumbnailCache::keyFor(const QString& absolutePath)
{
    const QFileInfo info(absolutePath);
    if (!info.exists() || !info.isFile())
        return QString();

    const QDateTime modified = info.lastModified();
    const qint64 modifiedMs =
        modified.isValid() ? modified.toUTC().toMSecsSinceEpoch() : qint64(0);

    // NUL-separated so "/a/b1" + mtime 234 cannot hash the same as
    // "/a/b" + mtime 1234.
    QByteArray payload = info.absoluteFilePath().toUtf8();
    payload.append('\0');
    payload.append(QByteArray::number(modifiedMs));
    payload.append('\0');
    payload.append(QByteArray::number(info.size()));

    QCryptographicHash hash(QCryptographicHash::Sha1);
    hash.addData(payload);
    return QString::fromLatin1(hash.result().toHex());
}

// ==============================================================================
// Lookup
// ==============================================================================

bool ThumbnailCache::has(const QString& absolutePath, Size size) const
{
    const QString key = keyFor(absolutePath);
    if (key.isEmpty())
        return false;

    {
        QMutexLocker locker(&d->mutex);
        if (d->memory.contains(Impl::memoryKey(key, size)))
            return true;
    }
    return !d->existingPath(key, size).isEmpty();
}

QImage ThumbnailCache::get(const QString& absolutePath, Size size) const
{
    const QString key = keyFor(absolutePath);
    if (key.isEmpty())
        return QImage();

    const QImage hot = d->memoryLookup(key, size);
    if (!hot.isNull())
        return hot;

    const QString path = d->existingPath(key, size);
    if (path.isEmpty())
        return QImage();

    QImage image;
    if (!image.load(path))
        return QImage();          // Truncated or corrupt cache entry: regenerate.

    d->memoryStore(key, size, image);
    return image;
}

// ==============================================================================
// Generation
// ==============================================================================

QImage ThumbnailCache::generate(const QString& absolutePath, Size size)
{
    const QString key = keyFor(absolutePath);
    if (key.isEmpty())
        return QImage();

    const QSize box = boundingBox(size);
    QImage      decoded;

    if (RawImageLoader::isRawExtension(absolutePath)) {
        // RAW. RawImageLoader is the only sanctioned decoder and it compiles
        // out to a null QImage when LibRaw is absent, so this path must treat a
        // null result as an ordinary miss rather than an error worth crashing
        // over.
        //
        // FOLLOW-UP: most RAW containers embed a full-size JPEG preview, and
        // pulling that out is roughly two orders of magnitude cheaper than a
        // demosaic. RawImageLoader.h exposes no preview entry point today
        // (load() only), and inventing a LibRaw call here would put raw
        // decoding logic in two places. Extracting previews belongs behind a
        // new RawImageLoader::loadPreview() and is left for that change.
        decoded = RawImageLoader::load(absolutePath);

        // Some builds ship a Qt plugin that can read DNG/CR2 headers. Costs one
        // failed probe when they do not.
        if (decoded.isNull()) {
            QImageReader reader(absolutePath);
            reader.setAutoTransform(true);
            const QSize source = reader.size();
            if (source.isValid() && !source.isEmpty()) {
                const QSize target = fitInside(source, box);
                if (target.isValid() && target != source)
                    reader.setScaledSize(target);
                decoded = reader.read();
            }
        }
    } else {
        QImageReader reader(absolutePath);
        reader.setAutoTransform(true);

        // The single most important line in this file. size() reads the header
        // only, and setScaledSize() hands the target down to the format handler
        // so libjpeg does a DCT-domain downscale during decode instead of
        // producing a 60 MP surface we immediately throw away. Without it a
        // grid scroll over large JPEGs allocates and touches hundreds of MB per
        // tile and is unusable.
        const QSize source = reader.size();
        if (source.isValid() && !source.isEmpty()) {
            const QSize target = fitInside(source, box);
            if (target.isValid() && target != source)
                reader.setScaledSize(target);
        }
        decoded = reader.read();
    }

    if (decoded.isNull() || decoded.width() <= 0 || decoded.height() <= 0)
        return QImage();

    // setScaledSize() is a hint: handlers without native scaling ignore it, and
    // libjpeg's DCT scaling only offers a fixed set of ratios, so the decoded
    // image can still be larger than the box. Finish the job here.
    const QSize target = fitInside(decoded.size(), box);
    QImage thumbnail =
        (target.isValid() && target != decoded.size())
            ? decoded.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation)
            : decoded;

    if (thumbnail.isNull())
        return QImage();

    const bool hasAlpha = thumbnail.hasAlphaChannel();

    // JPEG and PNG writers both reject the 16-bit-per-channel formats
    // RawImageLoader produces, and QImage::save() would silently convert
    // anyway. Do it explicitly so the conversion is visible and predictable.
    if (hasAlpha) {
        if (thumbnail.format() != QImage::Format_ARGB32 &&
            thumbnail.format() != QImage::Format_RGBA8888 &&
            thumbnail.format() != QImage::Format_ARGB32_Premultiplied) {
            thumbnail = thumbnail.convertToFormat(QImage::Format_ARGB32);
        }
    } else if (thumbnail.depth() > 32 || thumbnail.format() == QImage::Format_Invalid) {
        thumbnail = thumbnail.convertToFormat(QImage::Format_RGB888);
    }

    if (thumbnail.isNull())
        return QImage();

    if (d->ensureShard(key)) {
        const QString path = hasAlpha ? d->pngPath(key, size) : d->jpegPath(key, size);
        // A failed write is not fatal: the caller still gets its image, the
        // next request simply regenerates. Disk-full must not break the grid.
        writeAtomically(path,
                        thumbnail,
                        hasAlpha ? "png" : "jpeg",
                        hasAlpha ? -1 : kJpegQuality);
    }

    d->memoryStore(key, size, thumbnail);
    return thumbnail;
}

QImage ThumbnailCache::getOrGenerate(const QString& absolutePath, Size size)
{
    const QImage cached = get(absolutePath, size);
    if (!cached.isNull())
        return cached;
    return generate(absolutePath, size);
}

// ==============================================================================
// Maintenance
// ==============================================================================

bool ThumbnailCache::clear()
{
    QMutexLocker locker(&d->mutex);

    d->memory.clear();
    d->shardsCreated.clear();

    QDir dir(d->dir);
    if (!dir.exists())
        return true;

    // removeRecursively() takes the root with it; the cache directory is part
    // of the contract, so put it straight back.
    if (!dir.removeRecursively())
        return false;
    return QDir().mkpath(d->dir);
}

qint64 ThumbnailCache::cacheSizeBytes() const
{
    // Deliberately unlocked: this only reads the filesystem and d->dir, which is
    // fixed at construction. Holding the mutex for a full recursive walk would
    // stall every get() on the grid.
    qint64 total = 0;
    QDirIterator it(d->dir,
                    QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        total += it.fileInfo().size();
    }
    return total;
}

} // namespace lps
