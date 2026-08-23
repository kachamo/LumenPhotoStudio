// ==============================================================================
// tests/tst_thumbnailcache.cpp
// The on-disk thumbnail cache.
//
// Two things in this class are load-bearing and neither is visible to the user
// until it goes wrong.
//
// The first is keyFor(). The cache has no invalidation step and no "clear
// cache" button: a thumbnail stops being used because the key stopped matching,
// full stop. If the key ever ignores the mtime or the size, every user who
// edits a file in another application keeps looking at the old tile forever and
// has no way to fix it short of deleting a directory they do not know about.
// So the key is tested from both sides -- stable when nothing changed, and
// different when either input changed.
//
// The second is that generation and reading are separate runs of the program in
// practice: the grid generates a tile today and reads it back on tomorrow's
// launch, from a brand-new ThumbnailCache with an empty in-memory cache. The
// reload test below overwrites the stored file with a recognisable sentinel
// before reading it, because a test that merely calls get() and gets *an* image
// back cannot tell a disk hit from a silent regeneration.
//
// Everything here runs inside a QTemporaryDir. Nothing may touch the real cache
// directory, which is why test mode is switched on as well.
// ==============================================================================
#include "catalog/ThumbnailCache.h"

#include <QtTest>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QSemaphore>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>

#include <cmath>
#include <memory>
#include <vector>

using namespace lps;

namespace {

using Size = ThumbnailCache::Size;

// A gradient rather than a flat fill: a solid colour survives a wrong stride, a
// transposed scale or a half-decoded image without complaint, and the point of
// these fixtures is that a mistake shows up.
QImage makePattern(int width, int height)
{
    QImage image(width, height, QImage::Format_RGB888);
    for (int y = 0; y < height; ++y) {
        uchar* line = image.scanLine(y);
        for (int x = 0; x < width; ++x) {
            line[x * 3 + 0] = static_cast<uchar>((x * 255) / qMax(1, width - 1));
            line[x * 3 + 1] = static_cast<uchar>((y * 255) / qMax(1, height - 1));
            line[x * 3 + 2] = static_cast<uchar>(64 + ((x + y) % 128));
        }
    }
    return image;
}

// Left half fully transparent, right half opaque. A matte bug fills the
// transparent half with black or white and takes the alpha channel with it,
// which is exactly what the sample in the test looks for.
QImage makeHalfTransparent(int width, int height)
{
    QImage image(width, height, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    for (int y = 0; y < height; ++y) {
        for (int x = width / 2; x < width; ++x)
            image.setPixel(x, y, qRgba(220, 40, 30, 255));
    }
    return image;
}

} // namespace

class TstThumbnailCache : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    // ---- key derivation: the whole invalidation strategy ---------------------
    void keyIsStableForAnUnchangedFile();
    void keyChangesWhenTheFileSizeChanges();
    void keyChangesWhenTheModificationTimeChanges();
    void keyIsEmptyWhenThereIsNoFile();

    // ---- store and reload ----------------------------------------------------
    void generateWritesAShardedFileToDisk();
    void aFreshInstanceReadsTheStoredThumbnail();

    // ---- scaling rules -------------------------------------------------------
    void fitsInsideTheBoxAndKeepsTheAspectRatio();
    void neverUpscalesASmallSource();
    void keepsTransparencyInsteadOfBakingInAMatte();

    // ---- failure paths must not crash ---------------------------------------
    void corruptSourceYieldsANullImage();
    void missingSourceYieldsANullImage();
    void rawSourceYieldsANullImageWithoutLibRaw();

    // ---- maintenance and threading ------------------------------------------
    void clearEmptiesTheCacheAndTheSizeReport();
    void concurrentGetOrGenerateProducesOneIntactFile();

private:
    QString     sourcePath(const QString& name) const;
    QString     writePattern(const QString& name, int width, int height, const char* format);
    QStringList cacheFiles() const;

    std::unique_ptr<QTemporaryDir> m_temp;
    QString                        m_sources;
    QString                        m_cacheDir;
};

// ==============================================================================
// Fixture
// ==============================================================================

void TstThumbnailCache::initTestCase()
{
    // Belt and braces. Every test passes an explicit cache directory, but a
    // future test that forgets one must still not scribble into the developer's
    // real thumbnail cache.
    QStandardPaths::setTestModeEnabled(true);
}

void TstThumbnailCache::init()
{
    // A new temporary directory per test, so no test can see what another one
    // left behind and "is the cache empty?" is always a meaningful question.
    m_temp = std::make_unique<QTemporaryDir>();
    QVERIFY2(m_temp->isValid(), qPrintable(m_temp->errorString()));

    m_sources  = m_temp->filePath(QStringLiteral("sources"));
    m_cacheDir = m_temp->filePath(QStringLiteral("cache"));
    QVERIFY(QDir().mkpath(m_sources));
}

void TstThumbnailCache::cleanup()
{
    m_temp.reset();
}

QString TstThumbnailCache::sourcePath(const QString& name) const
{
    return QDir(m_sources).filePath(name);
}

QString TstThumbnailCache::writePattern(const QString& name,
                                        int            width,
                                        int            height,
                                        const char*    format)
{
    const QString path = sourcePath(name);
    // Empty on failure, so a broken fixture reports itself at the call site
    // instead of surfacing as a confusing assertion three lines later.
    return makePattern(width, height).save(path, format, 92) ? path : QString();
}

QStringList TstThumbnailCache::cacheFiles() const
{
    QStringList files;
    QDirIterator it(m_cacheDir,
                    QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden,
                    QDirIterator::Subdirectories);
    while (it.hasNext())
        files << it.next();
    files.sort();
    return files;
}

// ==============================================================================
// Key derivation
// ==============================================================================

void TstThumbnailCache::keyIsStableForAnUnchangedFile()
{
    const QString src = writePattern(QStringLiteral("stable.jpg"), 320, 240, "JPEG");
    QVERIFY(!src.isEmpty());

    const QString first = ThumbnailCache::keyFor(src);
    QVERIFY(!first.isEmpty());
    // SHA-1 hex. The cache uses the first two characters as a shard directory
    // name, so anything shorter would silently break the on-disk layout.
    QCOMPARE(first.size(), 40);

    // Reading the file must not change its key, and neither must asking twice.
    QImage probe;
    QVERIFY(probe.load(src));
    QCOMPARE(ThumbnailCache::keyFor(src), first);

    // The same bytes at a different path are a different cache entry: the path
    // is part of the key, so two copies of one photo do not share a tile.
    const QString copy = sourcePath(QStringLiteral("stable-copy.jpg"));
    QVERIFY(QFile::copy(src, copy));
    QVERIFY(ThumbnailCache::keyFor(copy) != first);
}

void TstThumbnailCache::keyChangesWhenTheFileSizeChanges()
{
    const QString src = writePattern(QStringLiteral("resized.jpg"), 320, 240, "JPEG");
    QVERIFY(!src.isEmpty());

    const QString before = ThumbnailCache::keyFor(src);
    QVERIFY(!before.isEmpty());

    // Re-save at a different resolution and quality: this is what "the user
    // edited the file in another application" looks like on disk.
    QVERIFY(makePattern(200, 150).save(src, "JPEG", 60));
    QVERIFY(QFileInfo(src).size() > 0);

    const QString after = ThumbnailCache::keyFor(src);
    QVERIFY(!after.isEmpty());
    QVERIFY2(after != before,
             "a modified file kept its cache key: every thumbnail of an edited "
             "image would stay stale forever, with no way to clear it");
}

void TstThumbnailCache::keyChangesWhenTheModificationTimeChanges()
{
    // The harder half of the invalidation contract. A file can be replaced by
    // one of exactly the same length -- a re-export at the same settings, a
    // metadata-only rewrite -- and then only the timestamp gives it away.
    const QString src = writePattern(QStringLiteral("touched.png"), 160, 120, "PNG");
    QVERIFY(!src.isEmpty());

    const QFileInfo info(src);
    const qint64    sizeBefore  = info.size();
    const QDateTime mtimeBefore = info.lastModified();
    const QString   before      = ThumbnailCache::keyFor(src);
    QVERIFY(!before.isEmpty());

    {
        // ReadWrite, not WriteOnly: WriteOnly truncates, and the size has to
        // stay identical for this test to be about the timestamp alone.
        QFile file(src);
        QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY2(file.setFileTime(mtimeBefore.addSecs(7200),
                                  QFileDevice::FileModificationTime),
                 "could not set the file modification time");
    }

    const QFileInfo touched(src);
    QCOMPARE(touched.size(), sizeBefore);            // the size really is unchanged
    QVERIFY(touched.lastModified() != mtimeBefore);  // ...and the clock really moved

    QVERIFY2(ThumbnailCache::keyFor(src) != before,
             "the cache key ignored the modification time: a same-size rewrite "
             "would never invalidate its thumbnail");
}

void TstThumbnailCache::keyIsEmptyWhenThereIsNoFile()
{
    QVERIFY(ThumbnailCache::keyFor(sourcePath(QStringLiteral("nope.jpg"))).isEmpty());
    QVERIFY(ThumbnailCache::keyFor(QString()).isEmpty());
    // A directory is not a file, and hashing one would mint a key for something
    // that can never be decoded.
    QVERIFY(ThumbnailCache::keyFor(m_sources).isEmpty());
}

// ==============================================================================
// Store and reload
// ==============================================================================

void TstThumbnailCache::generateWritesAShardedFileToDisk()
{
    const QString src = writePattern(QStringLiteral("grid.jpg"), 800, 400, "JPEG");
    QVERIFY(!src.isEmpty());

    ThumbnailCache cache(m_cacheDir);
    QCOMPARE(cache.cacheDir(), m_cacheDir);

    // Nothing there yet.
    QVERIFY(!cache.has(src, Size::Grid));
    QVERIFY(cache.get(src, Size::Grid).isNull());
    QCOMPARE(cache.cacheSizeBytes(), qint64(0));

    const QImage thumb = cache.generate(src, Size::Grid);
    QVERIFY(!thumb.isNull());
    QVERIFY(cache.has(src, Size::Grid));
    QVERIFY(cache.cacheSizeBytes() > 0);

    // Exactly one file, and it is where the documented layout says it is:
    // <cacheDir>/<first two hex chars>/<key>_<tag>.<ext>. The shard directory
    // is a performance contract -- 80,000 files in one directory is slow on
    // every filesystem the project ships on -- so it is worth pinning.
    const QStringList files = cacheFiles();
    QCOMPARE(files.size(), 1);

    const QString   key = ThumbnailCache::keyFor(src);
    const QFileInfo stored(files.first());
    QCOMPARE(stored.dir().dirName(), key.left(2));
    QVERIFY2(stored.fileName().startsWith(key), qPrintable(stored.fileName()));

    // The other size is a separate entry, not an overwrite of the first.
    QVERIFY(!cache.generate(src, Size::Preview).isNull());
    QCOMPARE(cacheFiles().size(), 2);
}

void TstThumbnailCache::aFreshInstanceReadsTheStoredThumbnail()
{
    const QString src = writePattern(QStringLiteral("persist.jpg"), 640, 480, "JPEG");
    QVERIFY(!src.isEmpty());

    {
        ThumbnailCache writer(m_cacheDir);
        QVERIFY(!writer.generate(src, Size::Grid).isNull());
    }   // destroyed: the in-memory hot cache goes with it, exactly as it does on quit

    const QStringList files = cacheFiles();
    QCOMPARE(files.size(), 1);

    // Replace the stored tile with an unmistakable sentinel. A reader that
    // regenerates instead of reading gets 256x192 back; only a reader that
    // actually opened the file on disk can return 7x5. Without this the test
    // could not tell the two apart and "the cache persists" would be unproven.
    QVERIFY(QImage(QSize(7, 5), QImage::Format_RGB888).save(files.first(), "JPEG", 90));

    ThumbnailCache reader(m_cacheDir);
    QVERIFY2(reader.has(src, Size::Grid),
             "a fresh instance did not see the thumbnail the previous one wrote");

    const QImage reloaded = reader.get(src, Size::Grid);
    QVERIFY(!reloaded.isNull());
    QCOMPARE(reloaded.size(), QSize(7, 5));

    // getOrGenerate() must prefer the stored entry too, not quietly redo the work.
    QCOMPARE(reader.getOrGenerate(src, Size::Grid).size(), QSize(7, 5));
    QCOMPARE(cacheFiles().size(), 1);
}

// ==============================================================================
// Scaling rules
// ==============================================================================

void TstThumbnailCache::fitsInsideTheBoxAndKeepsTheAspectRatio()
{
    ThumbnailCache cache(m_cacheDir);

    // Landscape: 2:1 into a 256 box is exactly 256x128, no rounding involved.
    const QString wide = writePattern(QStringLiteral("wide.jpg"), 800, 400, "JPEG");
    QVERIFY(!wide.isEmpty());
    const QImage wideThumb = cache.generate(wide, Size::Grid);
    QVERIFY(!wideThumb.isNull());
    QCOMPARE(wideThumb.size(), QSize(256, 128));

    // Portrait, with a ratio that does not divide evenly. Pin the long edge
    // exactly -- it must fill the box -- and the short edge to within the one
    // pixel that integer scaling can cost, rather than restating Qt's rounding
    // rule inside the test.
    const QString tall = writePattern(QStringLiteral("tall.jpg"), 300, 900, "JPEG");
    QVERIFY(!tall.isEmpty());
    const QImage tallThumb = cache.generate(tall, Size::Grid);
    QVERIFY(!tallThumb.isNull());
    QCOMPARE(tallThumb.height(), 256);
    QVERIFY(tallThumb.width() <= 256);
    const double wanted = 300.0 * 256.0 / 900.0;
    QVERIFY2(std::abs(tallThumb.width() - wanted) <= 1.0,
             qPrintable(QStringLiteral("aspect ratio lost: got %1x%2")
                            .arg(tallThumb.width())
                            .arg(tallThumb.height())));

    // Preview is a bigger box, same rules. It needs its own source: an 800px
    // wide image is already inside the 1024 box, and the cache would rightly
    // hand it back untouched rather than upscale it.
    const QString big = writePattern(QStringLiteral("big.jpg"), 2048, 1024, "JPEG");
    QVERIFY(!big.isEmpty());
    const QImage preview = cache.generate(big, Size::Preview);
    QVERIFY(!preview.isNull());
    QCOMPARE(preview.size(), QSize(1024, 512));
}

void TstThumbnailCache::neverUpscalesASmallSource()
{
    // A 96x72 source blown up to a 256 tile would be blurry and cost four times
    // the bytes for less detail than the original carries.
    const QString small = writePattern(QStringLiteral("small.png"), 96, 72, "PNG");
    QVERIFY(!small.isEmpty());

    ThumbnailCache cache(m_cacheDir);
    QCOMPARE(cache.generate(small, Size::Grid).size(), QSize(96, 72));
    QCOMPARE(cache.generate(small, Size::Preview).size(), QSize(96, 72));

    // And it survives the round trip at its original size.
    ThumbnailCache reader(m_cacheDir);
    QCOMPARE(reader.get(small, Size::Grid).size(), QSize(96, 72));
}

void TstThumbnailCache::keepsTransparencyInsteadOfBakingInAMatte()
{
    // Large enough that the scale path runs: a cut-out PNG that only kept its
    // alpha when it happened to be smaller than the box would still be broken
    // in the grid.
    const QString cutout = sourcePath(QStringLiteral("cutout.png"));
    QVERIFY(makeHalfTransparent(600, 400).save(cutout, "PNG"));

    ThumbnailCache cache(m_cacheDir);
    const QImage   thumb = cache.generate(cutout, Size::Grid);
    QVERIFY(!thumb.isNull());
    QVERIFY2(thumb.hasAlphaChannel(), "the thumbnail lost its alpha channel");

    // Well inside the transparent half, so the smooth-scale blend along the
    // boundary between the two halves cannot reach it.
    const QPoint clear(thumb.width() / 8, thumb.height() / 2);
    QCOMPARE(qAlpha(thumb.pixel(clear)), 0);
    // ...and the opaque half is still opaque, i.e. nothing turned the whole
    // image transparent just to make the assertion above pass.
    const QPoint solid(thumb.width() - thumb.width() / 8, thumb.height() / 2);
    QCOMPARE(qAlpha(thumb.pixel(solid)), 255);

    // Stored as PNG, because JPEG cannot carry alpha at all.
    const QStringList files = cacheFiles();
    QCOMPARE(files.size(), 1);
    QVERIFY2(files.first().endsWith(QLatin1String(".png")), qPrintable(files.first()));

    // The reload path must not lose it either.
    ThumbnailCache reader(m_cacheDir);
    const QImage   reloaded = reader.get(cutout, Size::Grid);
    QVERIFY(!reloaded.isNull());
    QVERIFY(reloaded.hasAlphaChannel());
    QCOMPARE(qAlpha(reloaded.pixel(clear)), 0);
}

// ==============================================================================
// Failure paths
// ==============================================================================

void TstThumbnailCache::corruptSourceYieldsANullImage()
{
    // A truncated download, a half-copied card, a text file someone renamed.
    const QString corrupt = sourcePath(QStringLiteral("corrupt.jpg"));
    {
        QFile file(corrupt);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write("this is not a JPEG, it is a sentence pretending to be one\n") > 0);
    }
    const QString empty = sourcePath(QStringLiteral("empty.png"));
    {
        QFile file(empty);
        QVERIFY(file.open(QIODevice::WriteOnly));
    }

    ThumbnailCache cache(m_cacheDir);
    QVERIFY(cache.generate(corrupt, Size::Grid).isNull());
    QVERIFY(cache.generate(empty, Size::Grid).isNull());
    QVERIFY(cache.getOrGenerate(corrupt, Size::Grid).isNull());

    // A failed decode must not leave a zero-byte tile behind that a later
    // has() would happily report as a cache hit.
    QVERIFY(!cache.has(corrupt, Size::Grid));
    QCOMPARE(cacheFiles().size(), 0);
    QCOMPARE(cache.cacheSizeBytes(), qint64(0));
}

void TstThumbnailCache::missingSourceYieldsANullImage()
{
    const QString  missing = sourcePath(QStringLiteral("ghost.jpg"));
    ThumbnailCache cache(m_cacheDir);

    QVERIFY(!cache.has(missing, Size::Grid));
    QVERIFY(cache.get(missing, Size::Grid).isNull());
    QVERIFY(cache.generate(missing, Size::Grid).isNull());
    QVERIFY(cache.getOrGenerate(missing, Size::Preview).isNull());
    QCOMPARE(cache.cacheSizeBytes(), qint64(0));
}

void TstThumbnailCache::rawSourceYieldsANullImageWithoutLibRaw()
{
    // This build has no LibRaw, so RawImageLoader::load() returns a null image
    // and the Qt fallback cannot parse a CR2 header either. The requirement is
    // that the miss is an ordinary miss: no crash, no exception, no half-written
    // tile. The bytes are deliberately not valid RAW, so this holds on a build
    // that does have LibRaw too.
    const QString raw = sourcePath(QStringLiteral("shot.cr2"));
    {
        QFile file(raw);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write(QByteArray(4096, '\x01')) == 4096);
    }

    ThumbnailCache cache(m_cacheDir);
    QVERIFY(!ThumbnailCache::keyFor(raw).isEmpty());     // the file itself is fine
    QVERIFY(cache.generate(raw, Size::Grid).isNull());
    QVERIFY(cache.getOrGenerate(raw, Size::Grid).isNull());
    QVERIFY(!cache.has(raw, Size::Grid));
}

// ==============================================================================
// Maintenance and threading
// ==============================================================================

void TstThumbnailCache::clearEmptiesTheCacheAndTheSizeReport()
{
    const QString a = writePattern(QStringLiteral("a.jpg"), 500, 500, "JPEG");
    const QString b = writePattern(QStringLiteral("b.jpg"), 400, 300, "JPEG");
    QVERIFY(!a.isEmpty());
    QVERIFY(!b.isEmpty());

    ThumbnailCache cache(m_cacheDir);
    QVERIFY(!cache.generate(a, Size::Grid).isNull());
    QVERIFY(!cache.generate(a, Size::Preview).isNull());
    QVERIFY(!cache.generate(b, Size::Grid).isNull());

    const qint64 filled = cache.cacheSizeBytes();
    QVERIFY2(filled > 0, "cacheSizeBytes() reported nothing after generating three tiles");
    QCOMPARE(cacheFiles().size(), 3);

    QVERIFY(cache.clear());

    QCOMPARE(cache.cacheSizeBytes(), qint64(0));
    QCOMPARE(cacheFiles().size(), 0);
    // The in-memory hot cache has to go too, or has() keeps answering yes for
    // tiles that were just deleted.
    QVERIFY(!cache.has(a, Size::Grid));
    QVERIFY(cache.get(a, Size::Grid).isNull());
    // The directory itself is part of the contract and must survive.
    QVERIFY(QFileInfo(m_cacheDir).isDir());

    // Still usable afterwards.
    QVERIFY(!cache.generate(a, Size::Grid).isNull());
    QVERIFY(cache.cacheSizeBytes() > 0);
}

void TstThumbnailCache::concurrentGetOrGenerateProducesOneIntactFile()
{
    // The real pattern: a grid scrolls, several loader threads land on the same
    // uncached tile at once, and every one of them calls getOrGenerate(). All
    // must come back with a usable image, and the file left on disk must be one
    // whole JPEG rather than two writes interleaved.
    const QString src = writePattern(QStringLiteral("busy.png"), 1200, 900, "PNG");
    QVERIFY(!src.isEmpty());

    ThumbnailCache cache(m_cacheDir);

    constexpr int         kThreads = 8;
    QSemaphore            gate;               // released kThreads times below
    std::vector<QImage>   results(kThreads);  // one slot per thread: nothing shared
    std::vector<QThread*> threads;
    threads.reserve(kThreads);

    for (int i = 0; i < kThreads; ++i) {
        QThread* worker = QThread::create([&, i]() {
            gate.acquire();                   // every thread starts at the same moment
            results[static_cast<size_t>(i)] = cache.getOrGenerate(src, Size::Grid);
        });
        threads.push_back(worker);
        worker->start();
    }
    gate.release(kThreads);

    // Generous join timeout rather than a fixed sleep: on a loaded CI runner
    // eight decodes can take a while, and the only thing worth failing on is a
    // thread that never comes back at all.
    bool joined = true;
    for (QThread* worker : threads) {
        if (worker->wait(60000))
            delete worker;
        else
            joined = false;   // leaked deliberately: deleting a running QThread aborts
    }
    QVERIFY2(joined, "a getOrGenerate() worker never returned");

    for (int i = 0; i < kThreads; ++i) {
        const QImage& image = results[static_cast<size_t>(i)];
        QVERIFY2(!image.isNull(), qPrintable(QStringLiteral("thread %1 got nothing").arg(i)));
        QCOMPARE(image.size(), QSize(256, 192));
    }

    // One source, one size, one file -- not one per thread, and no leftover
    // half-written temporary.
    const QStringList files = cacheFiles();
    QCOMPARE(files.size(), 1);

    QImage stored;
    QVERIFY2(stored.load(files.first()),
             "the file left on disk is not a readable image: concurrent writes "
             "interleaved");
    QCOMPARE(stored.size(), QSize(256, 192));
}

QTEST_MAIN(TstThumbnailCache)
#include "tst_thumbnailcache.moc"
