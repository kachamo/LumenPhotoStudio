// ==============================================================================
// tests/tst_catalogimporter.cpp
// The background folder scan and import.
//
// This is the class a user meets first and trusts least: they point it at a
// folder of 40,000 photographs and walk away. Four of its promises are worth
// more than all the others put together, and each has a test below that fails
// if the promise is withdrawn.
//
//   * A single unreadable file must not abort the run. One truncated JPEG at
//     image 12,000 that costs someone the other 28,000 is the difference
//     between a tool people use and a tool people uninstall.
//   * Re-importing a folder must skip what has not changed. If it does not,
//     every re-import costs a full import and nobody ever presses the button.
//   * Cancel means stop, not undo. The rows already committed stay committed.
//   * finished() fires exactly once, whatever happened -- including on a folder
//     that does not exist. Any caller that re-enables a button there would
//     otherwise be stuck forever.
//
// The importer is asynchronous, so every wait here is a signal wait or a
// QTRY_* poll with a generous timeout. There is not one fixed sleep used to
// wait for work to complete: that is how a suite starts failing once a week on
// a loaded CI runner, and once a suite does that people stop reading it.
// ==============================================================================
#include "catalog/CatalogImporter.h"

#include "catalog/CatalogDatabase.h"
#include "catalog/CatalogTypes.h"
#include "io/RawImageLoader.h"

#include <QtTest>

#include <QAtomicInt>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QVariant>
#include <QVector>

#include <memory>

using namespace lps;

namespace {

// The four numbers finished() reports, unpacked once so each test can say what
// it means rather than indexing into a QVariantList.
struct FinishResult
{
    int  imported  = -1;
    int  skipped   = -1;
    int  failed    = -1;
    bool cancelled = false;
};

// Generous by design. On an idle machine every import in this file completes in
// well under a second; the timeout exists only so a genuinely stuck worker
// fails the run instead of hanging it, and a shared CI runner under load must
// never get close to it.
constexpr int kFinishTimeoutMs = 30000;

// A macro rather than a helper function so a timeout is reported at the call
// site, and so QTRY_COMPARE's own failure handling still applies.
#define LPS_AWAIT_FINISHED(spy, result)                                            \
    do {                                                                           \
        QTRY_COMPARE_WITH_TIMEOUT((spy).size(), qsizetype(1), kFinishTimeoutMs);   \
        const QList<QVariant> lpsArgs = (spy).at(0);                               \
        (result) = FinishResult{ lpsArgs.at(0).toInt(), lpsArgs.at(1).toInt(),     \
                                 lpsArgs.at(2).toInt(), lpsArgs.at(3).toBool() };  \
    } while (false)

// Small, deterministic, and cheap to encode: a few hundred of these still cost
// less than one real photograph.
QImage makePattern(int width, int height)
{
    QImage image(width, height, QImage::Format_RGB888);
    for (int y = 0; y < height; ++y) {
        uchar* line = image.scanLine(y);
        for (int x = 0; x < width; ++x) {
            line[x * 3 + 0] = static_cast<uchar>((x * 7) % 256);
            line[x * 3 + 1] = static_cast<uchar>((y * 11) % 256);
            line[x * 3 + 2] = static_cast<uchar>((x + y) % 256);
        }
    }
    return image;
}

} // namespace

class TstCatalogImporter : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    // ---- static file-type surface -------------------------------------------
    void supportedExtensionsAreTheDocumentedSet();
    void isSupportedFileAcceptsAndRejects();

    // ---- a real import -------------------------------------------------------
    void importsAFolderAndRecordsWhatItFound();
    void signalsTellACoherentStory();

    // ---- the promises --------------------------------------------------------
    void reimportSkipsUnchangedFilesAndPicksUpTheModifiedOne();
    void cancelStopsEarlyAndKeepsWhatWasCommitted();
    void oneCorruptFileDoesNotAbortTheImport();

    // ---- boundaries ----------------------------------------------------------
    void nonRecursiveImportDoesNotDescend();
    void anEmptyFolderStillFinishes();
    void aMissingFolderFailsCleanly();
    void aSecondStartWhileRunningIsIgnored();

private:
    QString photosDir() const;
    QString catalogPath() const;
    QString writeJpeg(const QString& relativePath, int width, int height);
    QString writeGarbage(const QString& relativePath, const QByteArray& bytes);

    QVector<CatalogImage> catalogRows() const;
    int                   rowCount() const;

    std::unique_ptr<QTemporaryDir> m_temp;
    QHash<QString, QSize>          m_expected;   // file name -> dimensions written
};

// ==============================================================================
// Fixture
// ==============================================================================

void TstCatalogImporter::initTestCase()
{
    // The importer only writes where it is told, but CatalogDatabase falls back
    // to AppDataLocation when no path is set. Test mode keeps even a mistake
    // out of the developer's real catalog.
    QStandardPaths::setTestModeEnabled(true);
}

void TstCatalogImporter::init()
{
    m_temp = std::make_unique<QTemporaryDir>();
    QVERIFY2(m_temp->isValid(), qPrintable(m_temp->errorString()));
    m_expected.clear();
    QVERIFY(QDir().mkpath(photosDir()));
}

void TstCatalogImporter::cleanup()
{
    // The importer deletes its joined worker with deleteLater(). Flush those
    // now, while the event loop is still ours, rather than leaving them for a
    // later test to trip over.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    m_temp.reset();
}

QString TstCatalogImporter::photosDir() const
{
    return m_temp->filePath(QStringLiteral("photos"));
}

QString TstCatalogImporter::catalogPath() const
{
    return m_temp->filePath(QStringLiteral("catalog/lumen-test.db"));
}

QString TstCatalogImporter::writeJpeg(const QString& relativePath, int width, int height)
{
    const QString  path = QDir(photosDir()).filePath(relativePath);
    const QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath()))
        return QString();
    if (!makePattern(width, height).save(path, "JPEG", 80))
        return QString();
    m_expected.insert(info.fileName(), QSize(width, height));
    return path;
}

QString TstCatalogImporter::writeGarbage(const QString& relativePath, const QByteArray& bytes)
{
    const QString   path = QDir(photosDir()).filePath(relativePath);
    const QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath()))
        return QString();
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return QString();
    return file.write(bytes) == bytes.size() ? path : QString();
}

// Reads the catalog back from this thread with its own connection, the way the
// Library view would. Returns an empty vector if the catalog cannot be opened.
QVector<CatalogImage> TstCatalogImporter::catalogRows() const
{
    CatalogDatabase db;
    if (!db.open(catalogPath()))
        return {};
    return db.query(CatalogFilter{}, -1, 0);
}

int TstCatalogImporter::rowCount() const
{
    CatalogDatabase db;
    if (!db.open(catalogPath()))
        return -1;
    return db.queryCount(CatalogFilter{});
}

// ==============================================================================
// Static file-type surface
// ==============================================================================

void TstCatalogImporter::supportedExtensionsAreTheDocumentedSet()
{
    const QStringList extensions = CatalogImporter::supportedExtensions();

    // The documented list, spelled out rather than read back from the class, so
    // that quietly dropping a format is a test failure and not a silent change
    // in which of a user's files get imported.
    const QStringList documented = {
        QStringLiteral("jpg"),  QStringLiteral("jpeg"), QStringLiteral("png"),
        QStringLiteral("tif"),  QStringLiteral("tiff"), QStringLiteral("bmp"),
        QStringLiteral("webp"), QStringLiteral("cr2"),  QStringLiteral("cr3"),
        QStringLiteral("nef"),  QStringLiteral("arw"),  QStringLiteral("dng"),
        QStringLiteral("raf"),  QStringLiteral("orf"),  QStringLiteral("rw2"),
    };
    for (const QString& extension : documented) {
        QVERIFY2(extensions.contains(extension),
                 qPrintable(QStringLiteral("supportedExtensions() dropped .%1").arg(extension)));
    }
    QCOMPARE(extensions.size(), documented.size());

    // The header says lower-case and no dot, and isSupportedFile() compares
    // against a lower-cased suffix -- an upper-case entry here would simply
    // never match anything.
    for (const QString& extension : extensions) {
        QCOMPARE(extension, extension.toLower());
        QVERIFY(!extension.startsWith(QLatin1Char('.')));
        QVERIFY(!extension.isEmpty());
    }
    QCOMPARE(QSet<QString>(extensions.cbegin(), extensions.cend()).size(), extensions.size());

    // Every RAW extension the loader claims to handle must be one the importer
    // will actually pick up; otherwise a supported camera's files are invisible
    // in the Library and nothing anywhere says why.
    for (const QString& raw : { QStringLiteral("cr2"), QStringLiteral("cr3"),
                                QStringLiteral("nef"), QStringLiteral("arw"),
                                QStringLiteral("dng"), QStringLiteral("raf"),
                                QStringLiteral("orf"), QStringLiteral("rw2") }) {
        QVERIFY(RawImageLoader::isRawExtension(QStringLiteral("x.") + raw));
        QVERIFY(extensions.contains(raw));
    }
}

void TstCatalogImporter::isSupportedFileAcceptsAndRejects()
{
    QVERIFY(CatalogImporter::isSupportedFile(QStringLiteral("/photos/a.jpg")));
    QVERIFY(CatalogImporter::isSupportedFile(QStringLiteral("/photos/a.jpeg")));
    QVERIFY(CatalogImporter::isSupportedFile(QStringLiteral("/photos/a.png")));
    QVERIFY(CatalogImporter::isSupportedFile(QStringLiteral("/photos/a.tiff")));
    QVERIFY(CatalogImporter::isSupportedFile(QStringLiteral("/photos/a.webp")));
    QVERIFY(CatalogImporter::isSupportedFile(QStringLiteral("/photos/IMG_0001.CR2")));
    QVERIFY(CatalogImporter::isSupportedFile(QStringLiteral("relative/DSC1.NeF")));
    // Cameras write upper case as often as not, so the comparison has to be
    // case-insensitive or half a card comes back empty.
    QVERIFY(CatalogImporter::isSupportedFile(QStringLiteral("/photos/A.JPG")));

    QVERIFY(!CatalogImporter::isSupportedFile(QStringLiteral("/photos/notes.txt")));
    QVERIFY(!CatalogImporter::isSupportedFile(QStringLiteral("/photos/layered.psd")));
    QVERIFY(!CatalogImporter::isSupportedFile(QStringLiteral("/photos/clip.mp4")));
    QVERIFY(!CatalogImporter::isSupportedFile(QStringLiteral("/photos/sidecar.xmp")));
    QVERIFY(!CatalogImporter::isSupportedFile(QStringLiteral("/photos/README")));
    QVERIFY(!CatalogImporter::isSupportedFile(QString()));
    // The suffix is the last component: a .txt file is not an image however it
    // is named.
    QVERIFY(!CatalogImporter::isSupportedFile(QStringLiteral("/photos/a.jpg.txt")));
}

// ==============================================================================
// A real import
// ==============================================================================

void TstCatalogImporter::importsAFolderAndRecordsWhatItFound()
{
    constexpr int kTopLevel = 24;
    constexpr int kNested   = 4;

    for (int i = 0; i < kTopLevel; ++i) {
        QVERIFY(!writeJpeg(QStringLiteral("shot_%1.jpg").arg(i, 3, 10, QLatin1Char('0')),
                           40 + i, 30 + (i % 7)).isEmpty());
    }
    for (int i = 0; i < kNested; ++i) {
        QVERIFY(!writeJpeg(QStringLiteral("nested/deep_%1.jpg").arg(i), 60 + i, 45).isEmpty());
    }
    // Things that are not images must be ignored rather than counted as failures.
    QVERIFY(!writeGarbage(QStringLiteral("notes.txt"), QByteArray("not a photograph")).isEmpty());
    QVERIFY(!writeGarbage(QStringLiteral("nested/sidecar.xmp"), QByteArray("<x/>")).isEmpty());

    CatalogImporter importer;
    importer.setCatalogPath(catalogPath());
    QSignalSpy finishedSpy(&importer, &CatalogImporter::finished);
    QSignalSpy failedSpy(&importer, &CatalogImporter::failed);
    QVERIFY(finishedSpy.isValid());

    importer.startImport(photosDir(), true);

    FinishResult result;
    LPS_AWAIT_FINISHED(finishedSpy, result);

    QCOMPARE(result.imported, kTopLevel + kNested);
    QCOMPARE(result.skipped, 0);
    QCOMPARE(result.failed, 0);
    QVERIFY(!result.cancelled);
    QCOMPARE(failedSpy.size(), qsizetype(0));
    QVERIFY(!importer.isRunning());

    const QVector<CatalogImage> rows = catalogRows();
    QCOMPARE(rows.size(), kTopLevel + kNested);

    for (const CatalogImage& row : rows) {
        QVERIFY(row.isValid());
        // The stored path is rebuilt from the folder root plus the relative
        // path; if either is wrong the Library shows rows that open nothing.
        QVERIFY2(QFileInfo(row.absolutePath).isFile(),
                 qPrintable(QStringLiteral("path does not resolve: %1").arg(row.absolutePath)));
        QVERIFY(!row.relativePath.isEmpty());
        QVERIFY(!row.relativePath.startsWith(QLatin1Char('/')));
        QCOMPARE(row.fileSize, QFileInfo(row.absolutePath).size());
        QVERIFY(row.fileModified.isValid());
        QVERIFY(row.importedAt.isValid());
        QVERIFY(!row.isRaw);

        // Dimensions are read from the header at import time; the grid sizes
        // its tiles from them long before any pixel is decoded.
        QVERIFY2(m_expected.contains(row.fileName), qPrintable(row.fileName));
        QCOMPARE(QSize(row.width, row.height), m_expected.value(row.fileName));
    }

    // The nested files kept their sub-path rather than being flattened.
    int nested = 0;
    for (const CatalogImage& row : rows) {
        if (row.relativePath.startsWith(QLatin1String("nested/")))
            ++nested;
    }
    QCOMPARE(nested, kNested);
}

void TstCatalogImporter::signalsTellACoherentStory()
{
    constexpr int kFiles = 16;
    for (int i = 0; i < kFiles; ++i)
        QVERIFY(!writeJpeg(QStringLiteral("s_%1.jpg").arg(i), 32 + i, 24).isEmpty());

    CatalogImporter importer;
    importer.setCatalogPath(catalogPath());

    QSignalSpy startedSpy(&importer, &CatalogImporter::started);
    QSignalSpy progressSpy(&importer, &CatalogImporter::progress);
    QSignalSpy importedSpy(&importer, &CatalogImporter::imageImported);
    QSignalSpy finishedSpy(&importer, &CatalogImporter::finished);
    QVERIFY(startedSpy.isValid());
    QVERIFY(progressSpy.isValid());
    QVERIFY(importedSpy.isValid());
    QVERIFY(finishedSpy.isValid());

    importer.startImport(photosDir(), true);

    // started() is documented as synchronous: it is emitted on the calling
    // thread before the worker exists, so a caller can disable its button
    // without waiting for an event loop turn.
    QCOMPARE(startedSpy.size(), qsizetype(1));
    QCOMPARE(startedSpy.at(0).at(0).toString(), photosDir());
    QVERIFY(importer.isRunning());

    FinishResult result;
    LPS_AWAIT_FINISHED(finishedSpy, result);
    QCOMPARE(result.imported, kFiles);

    // The worker is joined before finished() is emitted, so from here on the
    // spies are quiet and can be read without racing the import.
    const QList<QList<QVariant>> ticks = progressSpy;
    QVERIFY2(!ticks.isEmpty(), "no progress at all: the bar would never move");

    int lastScan    = 0;
    int lastImport  = 0;
    int importTotal = -1;
    int importTicks = 0;

    for (const QList<QVariant>& tick : ticks) {
        const int     current = tick.at(0).toInt();
        const int     total   = tick.at(1).toInt();
        const QString file    = tick.at(2).toString();

        QVERIFY(current >= 1);
        QVERIFY2(!file.isEmpty(), "progress() named no file");

        if (total == 0) {
            // Scan phase. total == 0 is the documented "still counting" state;
            // the final tick of the phase can repeat the previous count, so
            // this is non-decreasing rather than strictly increasing.
            QVERIFY2(current >= lastScan, "scan progress went backwards");
            lastScan = current;
        } else {
            // Import phase. Once counting is done the total is fixed, and a
            // progress bar that jumps back is worse than no bar at all.
            if (importTotal < 0)
                importTotal = total;
            QCOMPARE(total, importTotal);
            QVERIFY2(current > lastImport, "import progress repeated or went backwards");
            QVERIFY(current <= total);
            lastImport = current;
            ++importTicks;
        }
    }

    QCOMPARE(lastScan, kFiles);       // the scan counted every file
    QCOMPARE(importTotal, kFiles);    // ...and told the bar the real total
    QCOMPARE(lastImport, kFiles);     // ...and the bar finished exactly on it
    QVERIFY(importTicks >= 2);        // first and last tick are always emitted

    QCOMPARE(importedSpy.size(), qsizetype(kFiles));

    // Exactly once, and it stays exactly once. Spinning the loop after the
    // fact can only ever catch a second emission, never invent one, so this
    // wait cannot make the test flaky -- it can only make it stricter.
    QTest::qWait(150);
    QCOMPARE(finishedSpy.size(), qsizetype(1));
    QCOMPARE(startedSpy.size(), qsizetype(1));
    QVERIFY(!importer.isRunning());
}

// ==============================================================================
// The promises
// ==============================================================================

void TstCatalogImporter::reimportSkipsUnchangedFilesAndPicksUpTheModifiedOne()
{
    constexpr int kFiles = 12;
    for (int i = 0; i < kFiles; ++i)
        QVERIFY(!writeJpeg(QStringLiteral("r_%1.jpg").arg(i), 50 + i, 40).isEmpty());

    CatalogImporter importer;
    importer.setCatalogPath(catalogPath());

    {   // ---- first run: everything is new ------------------------------------
        QSignalSpy finishedSpy(&importer, &CatalogImporter::finished);
        importer.startImport(photosDir(), true);
        FinishResult result;
        LPS_AWAIT_FINISHED(finishedSpy, result);
        QCOMPARE(result.imported, kFiles);
        QCOMPARE(result.skipped, 0);
        QCOMPARE(result.failed, 0);
    }
    QCOMPARE(rowCount(), kFiles);

    // A rating is user data. Re-import must not cost it, which is the reason
    // the skip path exists at all rather than deleting and re-inserting.
    const QString target = QDir(photosDir()).filePath(QStringLiteral("r_5.jpg"));
    qint64        targetId = -1;
    {
        CatalogDatabase db;
        QVERIFY2(db.open(catalogPath()), qPrintable(db.lastError()));
        const CatalogImage row = db.imageByPath(target);
        QVERIFY(row.isValid());
        targetId = row.id;
        QVERIFY(db.setRating(targetId, 4));
    }

    {   // ---- second run: nothing changed -------------------------------------
        QSignalSpy finishedSpy(&importer, &CatalogImporter::finished);
        importer.startImport(photosDir(), true);
        FinishResult result;
        LPS_AWAIT_FINISHED(finishedSpy, result);
        QVERIFY2(result.imported == 0,
                 qPrintable(QStringLiteral("re-import re-imported %1 unchanged files: "
                                           "a re-import would cost as much as a first import")
                                .arg(result.imported)));
        QCOMPARE(result.skipped, kFiles);
        QCOMPARE(result.failed, 0);
    }
    QCOMPARE(rowCount(), kFiles);   // and no duplicate rows appeared

    // ---- now change exactly one file -----------------------------------------
    // A different resolution changes both the size and the mtime, which is what
    // "edited in another application" looks like on disk.
    QVERIFY(makePattern(200, 120).save(target, "JPEG", 70));
    m_expected.insert(QFileInfo(target).fileName(), QSize(200, 120));

    {   // ---- third run: one in, the rest skipped -----------------------------
        QSignalSpy finishedSpy(&importer, &CatalogImporter::finished);
        importer.startImport(photosDir(), true);
        FinishResult result;
        LPS_AWAIT_FINISHED(finishedSpy, result);
        QVERIFY2(result.imported == 1,
                 qPrintable(QStringLiteral("expected exactly the modified file to be "
                                           "re-imported, got %1").arg(result.imported)));
        QCOMPARE(result.skipped, kFiles - 1);
        QCOMPARE(result.failed, 0);
    }

    QCOMPARE(rowCount(), kFiles);   // updated in place, not inserted again

    CatalogDatabase db;
    QVERIFY2(db.open(catalogPath()), qPrintable(db.lastError()));
    const CatalogImage row = db.imageByPath(target);
    QVERIFY(row.isValid());
    QCOMPARE(row.id, targetId);                       // same row
    QCOMPARE(QSize(row.width, row.height), QSize(200, 120));   // new dimensions
    QCOMPARE(row.fileSize, QFileInfo(target).size());
    QCOMPARE(row.rating, 4);                          // user data survived
}

void TstCatalogImporter::cancelStopsEarlyAndKeepsWhatWasCommitted()
{
    constexpr int kFiles = 80;
    for (int i = 0; i < kFiles; ++i)
        QVERIFY(!writeJpeg(QStringLiteral("c_%1.jpg").arg(i, 3, 10, QLatin1Char('0')),
                           48, 36).isEmpty());

    CatalogImporter importer;
    importer.setCatalogPath(catalogPath());
    QSignalSpy finishedSpy(&importer, &CatalogImporter::finished);

    // Direct connection on purpose. This handler runs on the worker thread the
    // instant the first row is in, so the cancel lands in the middle of the
    // import. A queued handler would not run until the test thread next spun
    // its event loop, by which time eighty tiny files are long since done --
    // and the test would quietly stop testing cancellation at all.
    QAtomicInt seen{ 0 };
    connect(&importer, &CatalogImporter::imageImported, this,
            [&importer, &seen](qint64, const QString&) {
                if (seen.fetchAndAddAcquire(1) == 0)
                    importer.cancel();
            },
            Qt::DirectConnection);

    importer.startImport(photosDir(), true);

    FinishResult result;
    LPS_AWAIT_FINISHED(finishedSpy, result);

    QVERIFY2(result.cancelled, "finished() did not report the run as cancelled");
    QVERIFY2(result.imported >= 1, "nothing was imported before the cancel");
    QVERIFY2(result.imported < kFiles,
             qPrintable(QStringLiteral("cancel() did not stop the import: %1 of %2 files "
                                       "were still processed")
                            .arg(result.imported).arg(kFiles)));
    QCOMPARE(result.failed, 0);
    QVERIFY(!importer.isRunning());

    // The point of the test: cancel means stop, not undo. The in-flight
    // transaction is committed rather than rolled back, so every row the user
    // watched go by is still there.
    QVERIFY2(rowCount() == result.imported,
             qPrintable(QStringLiteral("finished() claimed %1 imported but the catalog "
                                       "holds %2 rows")
                            .arg(result.imported).arg(rowCount())));

    // ...and a cancelled run leaves the catalog usable: the files that did make
    // it are skipped rather than duplicated next time round.
    QSignalSpy secondSpy(&importer, &CatalogImporter::finished);
    importer.startImport(photosDir(), true);
    FinishResult second;
    LPS_AWAIT_FINISHED(secondSpy, second);
    QVERIFY(!second.cancelled);
    QCOMPARE(second.skipped, result.imported);
    QCOMPARE(second.imported, kFiles - result.imported);
    QCOMPARE(rowCount(), kFiles);
}

void TstCatalogImporter::oneCorruptFileDoesNotAbortTheImport()
{
    constexpr int kGood = 10;
    for (int i = 0; i < kGood; ++i)
        QVERIFY(!writeJpeg(QStringLiteral("ok_%1.jpg").arg(i), 44 + i, 33).isEmpty());

    // Three ways a file can be unreadable while still looking like a photo:
    // truncated, replaced with something else entirely, and zero length.
    QVERIFY(!writeGarbage(QStringLiteral("truncated.jpg"),
                          QByteArray("\xFF\xD8\xFF\xE0 and then nothing useful", 32)).isEmpty());
    QVERIFY(!writeGarbage(QStringLiteral("mislabelled.jpg"),
                          QByteArray("this is a text file wearing a photograph's name")).isEmpty());
    QVERIFY(!writeGarbage(QStringLiteral("empty.png"), QByteArray()).isEmpty());

    CatalogImporter importer;
    importer.setCatalogPath(catalogPath());
    QSignalSpy finishedSpy(&importer, &CatalogImporter::finished);

    importer.startImport(photosDir(), true);

    FinishResult result;
    LPS_AWAIT_FINISHED(finishedSpy, result);

    // The whole point: the bad files are counted and the good ones still land.
    // A single unreadable file must never cost a user a 40,000 image import.
    QCOMPARE(result.imported, kGood);
    QCOMPARE(result.failed, 3);
    QCOMPARE(result.skipped, 0);
    QVERIFY(!result.cancelled);

    const QVector<CatalogImage> rows = catalogRows();
    QCOMPARE(rows.size(), kGood);
    for (const CatalogImage& row : rows) {
        QVERIFY2(!row.fileName.startsWith(QLatin1String("truncated")), qPrintable(row.fileName));
        QVERIFY2(!row.fileName.startsWith(QLatin1String("mislabelled")), qPrintable(row.fileName));
        QVERIFY2(!row.fileName.startsWith(QLatin1String("empty")), qPrintable(row.fileName));
        QVERIFY(row.width > 0 && row.height > 0);
    }
}

// ==============================================================================
// Boundaries
// ==============================================================================

void TstCatalogImporter::nonRecursiveImportDoesNotDescend()
{
    constexpr int kTop    = 5;
    constexpr int kBuried = 4;
    for (int i = 0; i < kTop; ++i)
        QVERIFY(!writeJpeg(QStringLiteral("top_%1.jpg").arg(i), 40, 30).isEmpty());
    for (int i = 0; i < kBuried; ++i)
        QVERIFY(!writeJpeg(QStringLiteral("sub/buried_%1.jpg").arg(i), 40, 30).isEmpty());
    QVERIFY(!writeJpeg(QStringLiteral("sub/deeper/further_0.jpg"), 40, 30).isEmpty());

    CatalogImporter importer;
    importer.setCatalogPath(catalogPath());
    QSignalSpy finishedSpy(&importer, &CatalogImporter::finished);

    importer.startImport(photosDir(), false);

    FinishResult result;
    LPS_AWAIT_FINISHED(finishedSpy, result);

    QCOMPARE(result.imported, kTop);
    QCOMPARE(result.skipped, 0);
    QCOMPARE(result.failed, 0);
    QCOMPARE(rowCount(), kTop);

    for (const CatalogImage& row : catalogRows())
        QVERIFY2(!row.relativePath.contains(QLatin1Char('/')), qPrintable(row.relativePath));
}

void TstCatalogImporter::anEmptyFolderStillFinishes()
{
    // Nothing to do is not an error, and a caller waiting on finished() to
    // re-enable its button must not be left waiting.
    CatalogImporter importer;
    importer.setCatalogPath(catalogPath());
    QSignalSpy finishedSpy(&importer, &CatalogImporter::finished);
    QSignalSpy failedSpy(&importer, &CatalogImporter::failed);

    importer.startImport(photosDir(), true);

    FinishResult result;
    LPS_AWAIT_FINISHED(finishedSpy, result);

    QCOMPARE(result.imported, 0);
    QCOMPARE(result.skipped, 0);
    QCOMPARE(result.failed, 0);
    QVERIFY(!result.cancelled);
    QCOMPARE(failedSpy.size(), qsizetype(0));
    QVERIFY(!importer.isRunning());
}

void TstCatalogImporter::aMissingFolderFailsCleanly()
{
    const QString missing = m_temp->filePath(QStringLiteral("no-such-folder"));
    QVERIFY(!QFileInfo::exists(missing));

    CatalogImporter importer;
    importer.setCatalogPath(catalogPath());
    QSignalSpy failedSpy(&importer, &CatalogImporter::failed);
    QSignalSpy finishedSpy(&importer, &CatalogImporter::finished);

    importer.startImport(missing, true);

    FinishResult result;
    LPS_AWAIT_FINISHED(finishedSpy, result);

    // An error is reported, and finished() still fires: the two are not
    // alternatives. A UI that only re-enables on finished() must not hang
    // because the user picked a folder that had just been unmounted.
    QCOMPARE(failedSpy.size(), qsizetype(1));
    QVERIFY(!failedSpy.at(0).at(0).toString().isEmpty());
    QCOMPARE(result.imported, 0);
    QCOMPARE(result.skipped, 0);
    QCOMPARE(result.failed, 0);
    QVERIFY(!result.cancelled);
    QVERIFY(!importer.isRunning());

    // The importer is reusable afterwards -- a bad folder does not wedge it.
    QVERIFY(!writeJpeg(QStringLiteral("after.jpg"), 40, 30).isEmpty());
    QSignalSpy secondSpy(&importer, &CatalogImporter::finished);
    importer.startImport(photosDir(), true);
    FinishResult second;
    LPS_AWAIT_FINISHED(secondSpy, second);
    QCOMPARE(second.imported, 1);
}

void TstCatalogImporter::aSecondStartWhileRunningIsIgnored()
{
    constexpr int kFiles = 40;
    for (int i = 0; i < kFiles; ++i)
        QVERIFY(!writeJpeg(QStringLiteral("busy_%1.jpg").arg(i), 40, 30).isEmpty());

    // A second folder with a different, much smaller population, so that the
    // final count says unambiguously which of the two runs actually happened.
    const QString decoy = m_temp->filePath(QStringLiteral("decoy"));
    QVERIFY(QDir().mkpath(decoy));
    for (int i = 0; i < 3; ++i)
        QVERIFY(makePattern(40, 30).save(QDir(decoy).filePath(QStringLiteral("d_%1.jpg").arg(i)),
                                         "JPEG", 80));

    CatalogImporter importer;
    importer.setCatalogPath(catalogPath());
    QSignalSpy startedSpy(&importer, &CatalogImporter::started);
    QSignalSpy finishedSpy(&importer, &CatalogImporter::finished);

    importer.startImport(photosDir(), true);
    // running is set synchronously inside startImport(), before the worker is
    // even created, so this second call is guaranteed to arrive "while
    // running" -- no timing assumption required.
    QVERIFY(importer.isRunning());
    importer.startImport(decoy, true);

    QCOMPARE(startedSpy.size(), qsizetype(1));
    QCOMPARE(startedSpy.at(0).at(0).toString(), photosDir());

    FinishResult result;
    LPS_AWAIT_FINISHED(finishedSpy, result);

    // The second call was dropped, not queued and not allowed to retarget the
    // run: the decoy folder's three files are nowhere in the result.
    QCOMPARE(result.imported, kFiles);
    QCOMPARE(rowCount(), kFiles);

    QTest::qWait(150);
    QCOMPARE(finishedSpy.size(), qsizetype(1));
    QCOMPARE(startedSpy.size(), qsizetype(1));

    // ...and once it is idle the importer accepts the folder it refused before.
    QSignalSpy secondSpy(&importer, &CatalogImporter::finished);
    importer.startImport(decoy, true);
    FinishResult second;
    LPS_AWAIT_FINISHED(secondSpy, second);
    QCOMPARE(second.imported, 3);
    QCOMPARE(rowCount(), kFiles + 3);
}

QTEST_MAIN(TstCatalogImporter)
#include "tst_catalogimporter.moc"
