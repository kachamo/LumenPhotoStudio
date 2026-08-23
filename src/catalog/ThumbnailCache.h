// ==============================================================================
// catalog/ThumbnailCache.h
// On-disk thumbnail cache.
//
// Grid scrolling must never decode a full image, and RAW decode is far too slow
// to do on demand. Thumbnails are generated once, written to disk as JPEG, and
// keyed by a hash of (absolutePath, fileMtime, fileSize) so a modified file
// invalidates itself without any explicit cache-clear step.
//
// Threading: get()/has() are safe to call from any thread. generate() does the
// decode and is intended to run off the UI thread.
// ==============================================================================
#pragma once

#include <QImage>
#include <QString>

namespace lps {

class ThumbnailCache
{
public:
    // Two sizes: a grid tile and a larger loupe/filmstrip preview.
    enum class Size : int { Grid = 256, Preview = 1024 };

    explicit ThumbnailCache(const QString& cacheDir = QString());
    ~ThumbnailCache();

    ThumbnailCache(const ThumbnailCache&)            = delete;
    ThumbnailCache& operator=(const ThumbnailCache&) = delete;

    // Default: <CacheLocation>/thumbnails
    static QString defaultCacheDir();
    QString cacheDir() const;

    // Cache key for a file's current state. Empty if the file is unreadable.
    static QString keyFor(const QString& absolutePath);

    bool   has(const QString& absolutePath, Size size) const;
    QImage get(const QString& absolutePath, Size size) const;

    // Decodes (RAW-aware via RawImageLoader), scales, stores, and returns it.
    // Returns a null QImage on failure. Blocking — call off the UI thread.
    QImage generate(const QString& absolutePath, Size size);

    // get() if present, otherwise generate().
    QImage getOrGenerate(const QString& absolutePath, Size size);

    bool   clear();
    qint64 cacheSizeBytes() const;

private:
    struct Impl;
    Impl* d;
};

} // namespace lps
