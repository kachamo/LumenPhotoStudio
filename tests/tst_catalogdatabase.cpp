// ==============================================================================
// tests/tst_catalogdatabase.cpp
// The SQLite catalog: every folder, image, rating, keyword and edit the user has.
//
// This is the only part of the application that owns data the user cannot
// regenerate. A render bug makes a picture look wrong and a fix repairs it; a
// catalog bug silently eats a week of culling and no fix brings it back. So the
// weight here is on the two ways that actually happens: a re-import overwriting
// user data, and a query returning the wrong rows — or a count that disagrees
// with the rows — so the grid quietly hides photos that are really there.
//
// A message handler is installed for the whole run and fails any test that
// provokes Qt's "connection is still in use" or "duplicate connection"
// warnings. Connection teardown is the easiest part of Qt SQL to get wrong and
// the hardest to notice: it leaks a driver handle per open and only bites in a
// long session, so it is checked after every test rather than left to a
// reviewer's eye.
//
// init()/cleanup() hand every test its own QTemporaryDir and its own catalog.
// Nothing is shared, so a failure names one behaviour rather than an ordering.
// ==============================================================================
#include "catalog/CatalogDatabase.h"
#include "catalog/CatalogTypes.h"

#include <QtTest>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>
#include <QTimeZone>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <utility>

using namespace lps;

namespace {

// ==============================================================================
// Qt log capture
//
// Only the two substrings that mean "a QSqlDatabase connection was mishandled"
// are recorded; everything else is passed through so a real diagnostic still
// reaches the CTest log. Recording rather than asserting is deliberate: the
// handler runs on whichever thread emitted the message, and Qt Test's failure
// machinery is only safe on the main thread.
// ==============================================================================
QMutex        g_logMutex;
QStringList   g_suspectMessages;
QtMessageHandler g_previousHandler = nullptr;

void captureSqlWarnings(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    // Qt's text is "connection 'lps_catalog_...' is still in use", with the
    // connection name in the middle, so the needle cannot include the word
    // "connection" — matching on the tail is what actually catches it.
    if (message.contains(QLatin1String("is still in use"), Qt::CaseInsensitive)
        || message.contains(QLatin1String("duplicate connection"), Qt::CaseInsensitive)) {
        const QMutexLocker locker(&g_logMutex);
        g_suspectMessages.append(message);
    }

    if (g_previousHandler)
        g_previousHandler(type, context, message);
    else
        std::fprintf(stderr, "%s\n", message.toLocal8Bit().constData());
}

QStringList takeSuspectMessages()
{
    const QMutexLocker locker(&g_logMutex);
    QStringList taken;
    taken.swap(g_suspectMessages);
    return taken;
}

// ==============================================================================
// Small helpers
// ==============================================================================

QStringList reversed(QStringList list)
{
    std::reverse(list.begin(), list.end());
    return list;
}

QStringList namesOf(const QVector<CatalogImage>& images)
{
    QStringList names;
    names.reserve(images.size());
    for (const CatalogImage& image : images)
        names.append(image.fileName);
    return names;
}

QVector<qint64> idsOf(const QVector<CatalogImage>& images)
{
    QVector<qint64> ids;
    ids.reserve(images.size());
    for (const CatalogImage& image : images)
        ids.append(image.id);
    return ids;
}

// Runs a filter both ways and describes any disagreement. It returns a string
// instead of asserting because QCOMPARE inside a helper only returns from the
// helper, which would let a mismatch slip past the caller.
QString filterMismatch(const CatalogDatabase& db,
                       const QString& label,
                       const CatalogFilter& filter,
                       const QStringList& expectedNames)
{
    const QVector<CatalogImage> rows = db.query(filter);

    // Sorted, so this test says nothing about ordering — that is a different
    // test, and coupling the two makes both harder to read when one breaks.
    QStringList got = namesOf(rows);
    got.sort(Qt::CaseInsensitive);
    QStringList want = expectedNames;
    want.sort(Qt::CaseInsensitive);

    if (got != want) {
        return QStringLiteral("%1: query() returned [%2], expected [%3]")
            .arg(label, got.join(QStringLiteral(", ")), want.join(QStringLiteral(", ")));
    }

    // queryCount() is a separate statement over the same predicate builder, so
    // the two can and do drift. Every filter in this file checks the pair.
    const int counted = db.queryCount(filter);
    if (counted != static_cast<int>(rows.size())) {
        return QStringLiteral("%1: queryCount() said %2 but query() returned %3 rows")
            .arg(label).arg(counted).arg(rows.size());
    }
    return QString();
}

// Rewrites schema_info behind the class's back, which is the only way to
// produce a catalog written by a future build.
bool writeRawSchemaVersion(const QString& path, int version, QString* errorOut)
{
    static int serial = 0;
    const QString name = QStringLiteral("tst_raw_%1").arg(serial++);

    bool ok = false;
    {
        QSqlDatabase raw = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        raw.setDatabaseName(path);
        if (!raw.open()) {
            if (errorOut)
                *errorOut = raw.lastError().text();
        } else {
            {
                QSqlQuery q(raw);
                q.prepare(QStringLiteral(
                    "UPDATE schema_info SET value = ? WHERE key = 'version'"));
                q.addBindValue(QString::number(version));
                ok = q.exec() && q.numRowsAffected() == 1;
                if (!ok && errorOut)
                    *errorOut = q.lastError().text();
            }
            raw.close();
        }
    }
    // The QSqlDatabase copy and the QSqlQuery are both out of scope by now,
    // which is exactly the discipline the message handler above is watching for.
    QSqlDatabase::removeDatabase(name);
    return ok;
}

QByteArray fileContents(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QByteArray();
    return file.readAll();
}

// A worker that owns its own CatalogDatabase, which is the contract the header
// states and the shape CatalogImporter uses. No Q_OBJECT: it needs no signals,
// and keeping moc out of it keeps the failure mode obvious.
class WriterThread : public QThread
{
public:
    WriterThread(QString path, qint64 folderId, int rowCount)
        : m_path(std::move(path)), m_folderId(folderId), m_rowCount(rowCount)
    {
    }

    QString error;   // read only after wait()

protected:
    void run() override
    {
        CatalogDatabase writer;
        if (!writer.open(m_path)) {
            error = QStringLiteral("worker open: ") + writer.lastError();
            return;
        }
        if (!writer.transaction()) {
            error = QStringLiteral("worker transaction: ") + writer.lastError();
            return;
        }
        for (int i = 0; i < m_rowCount; ++i) {
            CatalogImage image;
            image.folderId     = m_folderId;
            image.relativePath = QStringLiteral("worker/img_%1.arw").arg(i, 4, 10, QLatin1Char('0'));
            image.fileSize     = 1000 + i;
            image.isRaw        = true;
            if (writer.upsertImage(image) < 0) {
                error = QStringLiteral("worker upsert %1: ").arg(i) + writer.lastError();
                writer.rollback();
                return;
            }
        }
        if (!writer.commit())
            error = QStringLiteral("worker commit: ") + writer.lastError();
    }

private:
    QString m_path;
    qint64  m_folderId;
    int     m_rowCount;
};

} // namespace

// Checks a filter's rows and its count in one step and stops the test on the
// first disagreement. A macro rather than a function so QVERIFY2 aborts the
// *test*, and so the failure carries the call site's line number.
#define CHECK_FILTER(label, filter, expected)                                            \
    do {                                                                                 \
        const QString mismatch_ =                                                        \
            filterMismatch(*m_db, QStringLiteral(label), (filter), (expected));           \
        QVERIFY2(mismatch_.isEmpty(), qPrintable(mismatch_));                            \
    } while (false)

class TstCatalogDatabase : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    // ---- lifecycle ----------------------------------------------------------
    void openCreatesDirectoriesAndDatabase();
    void reopeningPreservesData();
    void refusesANewerSchemaVersion();
    void rejectsANonSqliteFile();
    void closedDatabaseFailsEveryCallSafely();

    // ---- folders and images -------------------------------------------------
    void addFolderIsIdempotentAndNormalizes();
    void upsertInsertsThenUpdatesTheSameRow();
    void reimportPreservesUserData();
    void acceptsEntirelyEmptyExifStrings();
    void derivesABareFileName();
    void captureTimeRoundTrips();
    void removeFolderCascadesToItsImages();
    void removeImageCascadesToItsKeywords();
    void hasImageNormalizesRelativePaths();
    void imageByPathResolvesAnAbsolutePath();

    // ---- querying -----------------------------------------------------------
    void sortsByEveryKeyInBothDirections();
    void filtersSelectTheExpectedRows();
    void queryCountAgreesWithQueryForEveryFilter();
    void pagingReproducesTheUnpagedOrder();
    void resistsSqlInjectionInSearchText();
    void treatsLikeMetacharactersLiterally();

    // ---- user data, keywords, collections, misc -----------------------------
    void userDataSettersClampAndReportMissingRows();
    void statsCountsCorrectly();
    void rollbackUndoesEverySinceBegin();
    void keywordsAreCaseInsensitive();
    void createCollectionIsIdempotentOnName();

    // ---- concurrency --------------------------------------------------------
    void secondInstanceOnTheSameThreadWorks();
    void workerThreadWritesWhileMainThreadReads();

private:
    QString folderPath(const QString& name) const;
    QString catalogPath(const QString& name) const;
    bool    seedFilterFixture();

    std::unique_ptr<QTemporaryDir>   m_dir;
    std::unique_ptr<CatalogDatabase> m_db;

    // Filled by seedFilterFixture(); meaningless otherwise.
    qint64                 m_folderA = -1;
    qint64                 m_folderB = -1;
    QHash<QString, qint64> m_ids;
};

// The epoch used by every fixture that needs timestamps. Fixed rather than
// "now" so a failure message is reproducible and no test depends on the clock.
static const qint64 kBaseEpoch = 1700000000;

static QDateTime baseTime(int offsetSeconds)
{
    return QDateTime::fromSecsSinceEpoch(kBaseEpoch + offsetSeconds);
}

// ==============================================================================
// Fixture
// ==============================================================================
void TstCatalogDatabase::initTestCase()
{
    g_previousHandler = qInstallMessageHandler(captureSqlWarnings);
}

void TstCatalogDatabase::cleanupTestCase()
{
    qInstallMessageHandler(g_previousHandler);
    g_previousHandler = nullptr;
}

void TstCatalogDatabase::init()
{
    takeSuspectMessages();   // discard anything left over from a failed test

    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY2(m_dir->isValid(), qPrintable(m_dir->errorString()));

    // Under a subdirectory that does not exist yet, so every single test also
    // exercises open()'s mkpath.
    m_db = std::make_unique<CatalogDatabase>();
    QVERIFY2(m_db->open(catalogPath(QStringLiteral("catalog/lumen-catalog.db"))),
             qPrintable(m_db->lastError()));
}

void TstCatalogDatabase::cleanup()
{
    // Destroy the catalog first: close() is where the prepared statements are
    // released and removeDatabase() runs, so it is what the check below judges.
    m_db.reset();
    m_dir.reset();
    m_ids.clear();
    m_folderA = -1;
    m_folderB = -1;

    const QStringList suspect = takeSuspectMessages();
    QVERIFY2(suspect.isEmpty(),
             qPrintable(QStringLiteral("Qt complained about SQL connection handling:\n  ")
                        + suspect.join(QStringLiteral("\n  "))));
}

QString TstCatalogDatabase::folderPath(const QString& name) const
{
    const QString path = m_dir->filePath(name);
    QDir().mkpath(path);
    return path;
}

QString TstCatalogDatabase::catalogPath(const QString& name) const
{
    return m_dir->filePath(name);
}

// The five-image, two-folder set the filter tests reason about. The values are
// chosen so that no two filters select the same subset — a fixture where they
// overlapped would pass even if two clauses were swapped.
bool TstCatalogDatabase::seedFilterFixture()
{
    m_folderA = m_db->addFolder(folderPath(QStringLiteral("photos/a")));
    m_folderB = m_db->addFolder(folderPath(QStringLiteral("photos/b")));
    if (m_folderA < 0 || m_folderB < 0)
        return false;

    struct Seed
    {
        qint64      folder;
        const char* name;
        const char* camera;
        const char* lens;
        int         rating;
        ImageFlag   flag;
        ColorLabel  label;
        bool        raw;
        int         captureOffset;
        const char* keywords;   // comma separated, empty for none
    };

    const Seed seeds[] = {
        {m_folderA, "one.jpg",   "Canon EOS R5", "RF 50mm F1.2", 5, ImageFlag::Picked,
         ColorLabel::Red,   true,  1, "portrait,studio"},
        {m_folderA, "two.jpg",   "Nikon Z9",     "Z 24-70mm",    3, ImageFlag::None,
         ColorLabel::Green, false, 2, "portrait"},
        {m_folderA, "three.jpg", "Canon EOS R5", "EF 85mm",      0, ImageFlag::Rejected,
         ColorLabel::None,  true,  3, "landscape"},
        {m_folderA, "four.jpg",  "Sony A7 IV",   "FE 35mm",      1, ImageFlag::None,
         ColorLabel::Red,   false, 4, ""},
        {m_folderB, "five.jpg",  "Leica M11",    "Summicron 35", 4, ImageFlag::Picked,
         ColorLabel::Blue,  false, 5, "street,portrait"},
    };

    for (const Seed& seed : seeds) {
        CatalogImage image;
        image.folderId     = seed.folder;
        image.relativePath = QLatin1String(seed.name);
        image.cameraModel  = QLatin1String(seed.camera);
        image.lensModel    = QLatin1String(seed.lens);
        image.rating       = seed.rating;
        image.flag         = seed.flag;
        image.colorLabel   = seed.label;
        image.isRaw        = seed.raw;
        image.fileSize     = 1000LL * seed.captureOffset;
        image.captureTime  = baseTime(seed.captureOffset);

        const qint64 id = m_db->upsertImage(image);
        if (id < 0)
            return false;
        m_ids.insert(QString::fromLatin1(seed.name), id);

        const QString keywords = QString::fromLatin1(seed.keywords);
        if (keywords.isEmpty())
            continue;
        const QStringList parts = keywords.split(QLatin1Char(','));
        for (const QString& keyword : parts) {
            if (!m_db->addKeyword(id, keyword))
                return false;
        }
    }
    return m_ids.size() == 5;
}

// ==============================================================================
// Lifecycle
// ==============================================================================
void TstCatalogDatabase::openCreatesDirectoriesAndDatabase()
{
    // init() already opened one under "catalog/", which did not exist. A deeper
    // nesting proves mkpath rather than a single mkdir.
    const QString path = catalogPath(QStringLiteral("deep/deeper/deepest/lps.db"));
    QVERIFY(!QFileInfo::exists(QFileInfo(path).absolutePath()));

    CatalogDatabase fresh;
    QVERIFY2(fresh.open(path), qPrintable(fresh.lastError()));
    QVERIFY(fresh.isOpen());
    QCOMPARE(fresh.databasePath(), QDir::cleanPath(path));
    QVERIFY(QFileInfo::exists(path));
    QVERIFY(fresh.lastError().isEmpty());

    // Usable, not merely present.
    const qint64 folder = fresh.addFolder(folderPath(QStringLiteral("photos")));
    QVERIFY2(folder > 0, qPrintable(fresh.lastError()));
    QCOMPARE(static_cast<int>(fresh.folders().size()), 1);

    // Opening twice on one instance is a programming error, not a no-op.
    QVERIFY(!fresh.open(path));
    QVERIFY2(fresh.lastError().contains(QLatin1String("already open")),
             qPrintable(fresh.lastError()));
    QVERIFY(fresh.isOpen());   // and the failed second call left it working

    fresh.close();
    QVERIFY(!fresh.isOpen());
    fresh.close();             // idempotent: a double close must not crash
}

void TstCatalogDatabase::reopeningPreservesData()
{
    const QString path = catalogPath(QStringLiteral("persist/lps.db"));
    const QString photos = folderPath(QStringLiteral("photos"));

    qint64 imageId = -1;
    {
        CatalogDatabase first;
        QVERIFY2(first.open(path), qPrintable(first.lastError()));
        const qint64 folder = first.addFolder(photos);
        QVERIFY(folder > 0);

        CatalogImage image;
        image.folderId     = folder;
        image.relativePath = QStringLiteral("trip/dsc_0001.arw");
        image.cameraModel  = QStringLiteral("Canon EOS R5");
        image.captureTime  = baseTime(0);
        imageId = first.upsertImage(image);
        QVERIFY2(imageId > 0, qPrintable(first.lastError()));

        QVERIFY(first.setRating(imageId, 3));
        QVERIFY(first.addKeyword(imageId, QStringLiteral("sunset")));
        QVERIFY(first.createCollection(QStringLiteral("Keepers")) > 0);
    }

    CatalogDatabase second;
    QVERIFY2(second.open(path), qPrintable(second.lastError()));

    const CatalogImage stored = second.image(imageId);
    QVERIFY2(stored.isValid(), qPrintable(second.lastError()));
    QCOMPARE(stored.rating, 3);
    QCOMPARE(stored.cameraModel, QStringLiteral("Canon EOS R5"));
    QCOMPARE(stored.captureTime, baseTime(0));
    QCOMPARE(stored.keywords, (QStringList{QStringLiteral("sunset")}));
    QCOMPARE(static_cast<int>(second.folders().size()), 1);
    // Idempotent create returns the id of the collection written by the first
    // instance, which only holds if the row actually survived.
    QVERIFY(second.createCollection(QStringLiteral("Keepers")) > 0);
    QCOMPARE(second.stats().imageCount, 1);
}

void TstCatalogDatabase::refusesANewerSchemaVersion()
{
    const QString path = catalogPath(QStringLiteral("future/lps.db"));
    {
        CatalogDatabase seed;
        QVERIFY2(seed.open(path), qPrintable(seed.lastError()));
    }

    const int future = CatalogDatabase::kSchemaVersion + 1;
    QString rawError;
    QVERIFY2(writeRawSchemaVersion(path, future, &rawError), qPrintable(rawError));

    CatalogDatabase later;
    QVERIFY2(!later.open(path), "a catalog from a newer build must not be opened");
    QVERIFY(!later.isOpen());

    // The message has to tell a user what to do about it, so both version
    // numbers and the offending path belong in it.
    const QString error = later.lastError();
    QVERIFY2(error.contains(QLatin1String("newer")), qPrintable(error));
    QVERIFY2(error.contains(QString::number(future)), qPrintable(error));
    QVERIFY2(error.contains(QString::number(CatalogDatabase::kSchemaVersion)),
             qPrintable(error));

    // Refusing must not have damaged it: put the version back and it opens.
    QVERIFY2(writeRawSchemaVersion(path, CatalogDatabase::kSchemaVersion, &rawError),
             qPrintable(rawError));
    CatalogDatabase recovered;
    QVERIFY2(recovered.open(path), qPrintable(recovered.lastError()));
}

void TstCatalogDatabase::rejectsANonSqliteFile()
{
    // The realistic way to reach this: a file picker pointed at the wrong file.
    const QString path = catalogPath(QStringLiteral("junk/notes.txt"));
    QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write(QByteArray("this is not a database, it is a note\n").repeated(64)) > 0);
    }
    const QByteArray before = fileContents(path);
    QVERIFY(!before.isEmpty());

    CatalogDatabase junk;
    QVERIFY2(!junk.open(path), "a non-SQLite file must be refused");
    QVERIFY(!junk.isOpen());
    QVERIFY(!junk.lastError().isEmpty());

    // Failing is not enough: a refused open must not have written over whatever
    // the user actually picked.
    QCOMPARE(fileContents(path), before);
}

void TstCatalogDatabase::closedDatabaseFailsEveryCallSafely()
{
    // Never opened at all — the state the UI is in before the user picks a
    // catalog, and the state every instance returns to after close().
    CatalogDatabase closed;
    QVERIFY(!closed.isOpen());
    QVERIFY(closed.databasePath().isEmpty());

    // Both halves matter. The method name is what makes the log actionable;
    // "not open" is what proves the call was turned away by the guard rather
    // than having gone on to fail somewhere deeper in the driver, which would
    // be the same result reached by luck.
    const auto mentions = [&closed](const char* method) {
        const QString error = closed.lastError();
        return error.contains(QLatin1String(method))
            && error.contains(QLatin1String("not open"));
    };

    QCOMPARE(closed.addFolder(QStringLiteral("/tmp/photos")), static_cast<qint64>(-1));
    QVERIFY2(mentions("addFolder"), qPrintable(closed.lastError()));

    QVERIFY(!closed.removeFolder(1));
    QVERIFY2(mentions("removeFolder"), qPrintable(closed.lastError()));

    QVERIFY(closed.folders().isEmpty());
    QVERIFY2(mentions("folders"), qPrintable(closed.lastError()));

    CatalogImage image;
    image.folderId     = 1;
    image.relativePath = QStringLiteral("a.jpg");
    QCOMPARE(closed.upsertImage(image), static_cast<qint64>(-1));
    QVERIFY2(mentions("upsertImage"), qPrintable(closed.lastError()));

    QVERIFY(!closed.removeImage(1));
    QVERIFY2(mentions("removeImage"), qPrintable(closed.lastError()));

    QVERIFY(!closed.image(1).isValid());
    QVERIFY2(mentions("image"), qPrintable(closed.lastError()));

    QVERIFY(!closed.imageByPath(QStringLiteral("/tmp/photos/a.jpg")).isValid());
    QVERIFY2(mentions("imageByPath"), qPrintable(closed.lastError()));

    QVERIFY(!closed.hasImage(1, QStringLiteral("a.jpg")));
    QVERIFY2(mentions("hasImage"), qPrintable(closed.lastError()));

    QVERIFY(closed.query(CatalogFilter{}).isEmpty());
    QVERIFY2(mentions("query"), qPrintable(closed.lastError()));

    QCOMPARE(closed.queryCount(CatalogFilter{}), 0);
    QVERIFY2(mentions("queryCount"), qPrintable(closed.lastError()));

    QVERIFY(!closed.setRating(1, 3));
    QVERIFY2(mentions("setRating"), qPrintable(closed.lastError()));

    QVERIFY(!closed.setFlag(1, ImageFlag::Picked));
    QVERIFY2(mentions("setFlag"), qPrintable(closed.lastError()));

    QVERIFY(!closed.setColorLabel(1, ColorLabel::Red));
    QVERIFY2(mentions("setColorLabel"), qPrintable(closed.lastError()));

    QVERIFY(!closed.setLookJson(1, QStringLiteral("{}")));
    QVERIFY2(mentions("setLookJson"), qPrintable(closed.lastError()));

    QVERIFY(!closed.addKeyword(1, QStringLiteral("x")));
    QVERIFY2(mentions("addKeyword"), qPrintable(closed.lastError()));

    QVERIFY(!closed.removeKeyword(1, QStringLiteral("x")));
    QVERIFY2(mentions("removeKeyword"), qPrintable(closed.lastError()));

    QVERIFY(closed.keywordsFor(1).isEmpty());
    QVERIFY2(mentions("keywordsFor"), qPrintable(closed.lastError()));

    QVERIFY(closed.allKeywords().isEmpty());
    QVERIFY2(mentions("allKeywords"), qPrintable(closed.lastError()));

    QCOMPARE(closed.createCollection(QStringLiteral("c")), static_cast<qint64>(-1));
    QVERIFY2(mentions("createCollection"), qPrintable(closed.lastError()));

    QVERIFY(!closed.deleteCollection(1));
    QVERIFY2(mentions("deleteCollection"), qPrintable(closed.lastError()));

    QVERIFY(!closed.addToCollection(1, 1));
    QVERIFY2(mentions("addToCollection"), qPrintable(closed.lastError()));

    QVERIFY(!closed.removeFromCollection(1, 1));
    QVERIFY2(mentions("removeFromCollection"), qPrintable(closed.lastError()));

    const CatalogStats stats = closed.stats();
    QCOMPARE(stats.folderCount, 0);
    QCOMPARE(stats.imageCount, 0);
    QCOMPARE(stats.rawCount, 0);
    QCOMPARE(stats.pickedCount, 0);
    QCOMPARE(stats.rejectedCount, 0);
    QVERIFY2(mentions("stats"), qPrintable(closed.lastError()));

    QVERIFY(!closed.transaction());
    QVERIFY2(mentions("transaction"), qPrintable(closed.lastError()));
    QVERIFY(!closed.commit());
    QVERIFY2(mentions("commit"), qPrintable(closed.lastError()));
    QVERIFY(!closed.rollback());
    QVERIFY2(mentions("rollback"), qPrintable(closed.lastError()));

    closed.close();            // closing something never opened must be safe
    QVERIFY(!closed.isOpen());

    // An instance that *was* open must behave the same way afterwards, or the
    // "not open" guard only covers half the states it claims to.
    CatalogDatabase reopened;
    QVERIFY2(reopened.open(catalogPath(QStringLiteral("shut/lps.db"))),
             qPrintable(reopened.lastError()));
    reopened.close();
    QVERIFY(!reopened.isOpen());
    QCOMPARE(reopened.addFolder(QStringLiteral("/tmp/photos")), static_cast<qint64>(-1));
    QVERIFY(reopened.query(CatalogFilter{}).isEmpty());
    QCOMPARE(reopened.queryCount(CatalogFilter{}), 0);
}

// ==============================================================================
// Folders and images
// ==============================================================================
void TstCatalogDatabase::addFolderIsIdempotentAndNormalizes()
{
    const QString base = folderPath(QStringLiteral("photos/library"));

    const qint64 first = m_db->addFolder(base);
    QVERIFY2(first > 0, qPrintable(m_db->lastError()));

    // Every spelling the UI and the recent-folders list can produce.
    QCOMPARE(m_db->addFolder(base), first);
    QCOMPARE(m_db->addFolder(base + QLatin1Char('/')), first);
    QCOMPARE(m_db->addFolder(base + QLatin1String("//")), first);
    QCOMPARE(m_db->addFolder(base + QLatin1String("/./")), first);
    QCOMPARE(m_db->addFolder(base + QLatin1String("/nested/..")), first);
    QCOMPARE(m_db->addFolder(QDir::toNativeSeparators(base)), first);
    QCOMPARE(m_db->addFolder(QDir::toNativeSeparators(base) + QDir::separator()), first);

    const QVector<CatalogFolder> stored = m_db->folders();
    QCOMPARE(static_cast<int>(stored.size()), 1);
    QVERIFY2(!stored[0].path.contains(QLatin1Char('\\')),
             qPrintable(stored[0].path));
    QVERIFY2(!stored[0].path.endsWith(QLatin1Char('/')), qPrintable(stored[0].path));
    QVERIFY(stored[0].addedAt.isValid());
    QCOMPARE(stored[0].imageCount, 0);

    // A different folder is a different row, even one nested inside the first.
    const qint64 nested = m_db->addFolder(folderPath(QStringLiteral("photos/library/2026")));
    QVERIFY(nested > 0);
    QVERIFY(nested != first);
    QCOMPARE(static_cast<int>(m_db->folders().size()), 2);

    // An empty path is a caller bug, not a silent no-op.
    QCOMPARE(m_db->addFolder(QString()), static_cast<qint64>(-1));
    QVERIFY2(m_db->lastError().contains(QLatin1String("empty")),
             qPrintable(m_db->lastError()));
    QCOMPARE(static_cast<int>(m_db->folders().size()), 2);

    // withCounts is what the folder tree draws, so it has to be right.
    CatalogImage image;
    image.folderId     = first;
    image.relativePath = QStringLiteral("a.jpg");
    QVERIFY(m_db->upsertImage(image) > 0);
    image.relativePath = QStringLiteral("b.jpg");
    QVERIFY(m_db->upsertImage(image) > 0);

    for (const CatalogFolder& folder : m_db->folders(true))
        QCOMPARE(folder.imageCount, folder.id == first ? 2 : 0);
    for (const CatalogFolder& folder : m_db->folders(false))
        QCOMPARE(folder.imageCount, 0);
}

void TstCatalogDatabase::upsertInsertsThenUpdatesTheSameRow()
{
    const qint64 folderA = m_db->addFolder(folderPath(QStringLiteral("photos/a")));
    const qint64 folderB = m_db->addFolder(folderPath(QStringLiteral("photos/b")));
    QVERIFY(folderA > 0 && folderB > 0);

    CatalogImage image;
    image.folderId     = folderA;
    image.relativePath = QStringLiteral("trip/dsc_0001.arw");
    image.fileSize     = 111;
    image.width        = 10;
    image.height       = 20;
    image.cameraModel  = QStringLiteral("Canon EOS R5");

    const qint64 id = m_db->upsertImage(image);
    QVERIFY2(id > 0, qPrintable(m_db->lastError()));
    QVERIFY(m_db->hasImage(folderA, QStringLiteral("trip/dsc_0001.arw")));

    // Same (folderId, relativePath) -> same row, whatever separators are used.
    image.relativePath = QDir::toNativeSeparators(QStringLiteral("trip/dsc_0001.arw"));
    image.fileSize     = 222;
    image.width        = 30;
    QCOMPARE(m_db->upsertImage(image), id);
    QCOMPARE(m_db->queryCount(CatalogFilter{}), 1);

    const CatalogImage stored = m_db->image(id);
    QCOMPARE(stored.fileSize, static_cast<qint64>(222));
    QCOMPARE(stored.width, 30);
    QCOMPARE(stored.relativePath, QStringLiteral("trip/dsc_0001.arw"));

    // The same relative path under a different root is a different photo.
    image.folderId = folderB;
    const qint64 other = m_db->upsertImage(image);
    QVERIFY(other > 0);
    QVERIFY(other != id);
    QCOMPARE(m_db->queryCount(CatalogFilter{}), 2);

    // Callers that get it wrong are told so rather than writing a broken row.
    CatalogImage bad;
    bad.relativePath = QStringLiteral("x.jpg");
    QCOMPARE(m_db->upsertImage(bad), static_cast<qint64>(-1));
    QVERIFY2(m_db->lastError().contains(QLatin1String("folderId")),
             qPrintable(m_db->lastError()));

    bad.folderId     = folderA;
    bad.relativePath = QString();
    QCOMPARE(m_db->upsertImage(bad), static_cast<qint64>(-1));
    QVERIFY2(m_db->lastError().contains(QLatin1String("relativePath")),
             qPrintable(m_db->lastError()));

    QCOMPARE(m_db->queryCount(CatalogFilter{}), 2);
}

// The one test in this file that would justify the file on its own. A catalog
// that loses a rating once is never trusted again, and re-importing a folder is
// something the user does casually and often.
void TstCatalogDatabase::reimportPreservesUserData()
{
    const qint64 folder = m_db->addFolder(folderPath(QStringLiteral("photos")));
    QVERIFY(folder > 0);

    CatalogImage first;
    first.folderId     = folder;
    first.relativePath = QStringLiteral("iceland/dsc_0042.arw");
    first.fileSize     = 20 * 1024 * 1024;
    first.fileModified = baseTime(0);
    first.width        = 8192;
    first.height       = 5464;
    first.isRaw        = true;
    first.cameraModel  = QStringLiteral("Canon EOS R5");
    first.captureTime  = baseTime(-3600);

    const qint64 id = m_db->upsertImage(first);
    QVERIFY2(id > 0, qPrintable(m_db->lastError()));

    // Everything a user can invest in a photo without touching the file.
    const QString look = QStringLiteral(R"({"exposure":0.75,"contrast":-0.2})");
    QVERIFY(m_db->setRating(id, 4));
    QVERIFY(m_db->setFlag(id, ImageFlag::Picked));
    QVERIFY(m_db->setColorLabel(id, ColorLabel::Purple));
    QVERIFY(m_db->setLookJson(id, look));
    QVERIFY(m_db->addKeyword(id, QStringLiteral("keeper")));
    QVERIFY(m_db->addKeyword(id, QStringLiteral("iceland")));
    const qint64 collection = m_db->createCollection(QStringLiteral("Portfolio"));
    QVERIFY(collection > 0);
    QVERIFY(m_db->addToCollection(collection, id));

    const QDateTime originalImport = m_db->image(id).importedAt;
    QVERIFY(originalImport.isValid());

    // A second import pass. The scanner produces a plain CatalogImage from the
    // file on disk, so it carries no user data at all — these defaults are
    // precisely what would wipe the user's work if the UPDATE list were wrong.
    CatalogImage again;
    again.folderId     = folder;
    again.relativePath = QStringLiteral("iceland/dsc_0042.arw");
    again.fileSize     = 24 * 1024 * 1024;          // the file was re-saved
    again.fileModified = baseTime(86400);
    again.width        = 8192;
    again.height       = 5464;
    again.isRaw        = true;
    again.cameraModel  = QStringLiteral("Canon EOS R5");
    again.lensModel    = QStringLiteral("RF 15-35mm F2.8");   // newly readable
    again.iso          = QStringLiteral("400");
    again.captureTime  = baseTime(-3600);
    QCOMPARE(again.rating, 0);
    QCOMPARE(static_cast<int>(again.flag), static_cast<int>(ImageFlag::None));
    QCOMPARE(static_cast<int>(again.colorLabel), static_cast<int>(ColorLabel::None));
    QVERIFY(again.lookJson.isEmpty());
    QVERIFY(again.keywords.isEmpty());

    QCOMPARE(m_db->upsertImage(again), id);

    const CatalogImage stored = m_db->image(id);
    QVERIFY2(stored.isValid(), qPrintable(m_db->lastError()));

    // ---- the user's work, all of it -----------------------------------------
    QCOMPARE(stored.rating, 4);
    QCOMPARE(static_cast<int>(stored.flag), static_cast<int>(ImageFlag::Picked));
    QCOMPARE(static_cast<int>(stored.colorLabel), static_cast<int>(ColorLabel::Purple));
    QCOMPARE(stored.lookJson, look);
    QCOMPARE(stored.keywords, (QStringList{QStringLiteral("iceland"), QStringLiteral("keeper")}));
    // imported_at is when *this catalog* first saw the file; a re-scan is not a
    // new import, and "recently added" would otherwise reshuffle on every scan.
    QCOMPARE(stored.importedAt, originalImport);
    // Collection membership lives in another table; removeFromCollection only
    // succeeds if the row is still there, which is the cheapest way to prove it.
    QVERIFY2(m_db->removeFromCollection(collection, id), qPrintable(m_db->lastError()));

    // ---- and the file facts really were refreshed ---------------------------
    QCOMPARE(stored.fileSize, static_cast<qint64>(24 * 1024 * 1024));
    QCOMPARE(stored.fileModified, baseTime(86400));
    QCOMPARE(stored.lensModel, QStringLiteral("RF 15-35mm F2.8"));
    QCOMPARE(stored.iso, QStringLiteral("400"));

    // The same must hold for the rows the grid actually renders, not just for
    // image(): query() reads through a different column path.
    const QVector<CatalogImage> rows = m_db->query(CatalogFilter{});
    QCOMPARE(static_cast<int>(rows.size()), 1);
    QCOMPARE(rows[0].rating, 4);
    QCOMPARE(static_cast<int>(rows[0].colorLabel), static_cast<int>(ColorLabel::Purple));
    QCOMPARE(rows[0].lookJson, look);
    QCOMPARE(rows[0].keywords, (QStringList{QStringLiteral("iceland"), QStringLiteral("keeper")}));
    QCOMPARE(rows[0].fileSize, static_cast<qint64>(24 * 1024 * 1024));
}

void TstCatalogDatabase::acceptsEntirelyEmptyExifStrings()
{
    const qint64 folder = m_db->addFolder(folderPath(QStringLiteral("photos")));
    QVERIFY(folder > 0);

    // A scanned print, a screenshot, a JPEG stripped of metadata: nothing but a
    // path. The trap is that a default-constructed QString is *null*, and Qt
    // binds null as SQL NULL, which every NOT NULL text column here rejects.
    CatalogImage bare;
    bare.folderId     = folder;
    bare.relativePath = QStringLiteral("scan.tif");
    QVERIFY(bare.cameraModel.isNull());
    QVERIFY(bare.lensModel.isNull());
    QVERIFY(bare.iso.isNull());
    QVERIFY(bare.aperture.isNull());
    QVERIFY(bare.shutterSpeed.isNull());
    QVERIFY(bare.focalLength.isNull());
    QVERIFY(bare.lookJson.isNull());
    QVERIFY(bare.fileName.isNull());

    const qint64 id = m_db->upsertImage(bare);
    QVERIFY2(id > 0, qPrintable(m_db->lastError()));

    const CatalogImage stored = m_db->image(id);
    QVERIFY2(stored.isValid(), qPrintable(m_db->lastError()));
    QCOMPARE(stored.cameraModel, QString());
    QCOMPARE(stored.lensModel, QString());
    QCOMPARE(stored.iso, QString());
    QCOMPARE(stored.aperture, QString());
    QCOMPARE(stored.shutterSpeed, QString());
    QCOMPARE(stored.focalLength, QString());
    QCOMPARE(stored.lookJson, QString());
    QCOMPARE(stored.fileName, QStringLiteral("scan.tif"));
    QCOMPARE(m_db->queryCount(CatalogFilter{}), 1);

    // Explicitly empty (but non-null) must behave identically on the update
    // path, which binds the same columns through a different code path.
    CatalogImage blanked = bare;
    blanked.cameraModel = QStringLiteral("");
    blanked.lookJson    = QStringLiteral("");
    QCOMPARE(m_db->upsertImage(blanked), id);

    // Clearing an edit is a normal user action and must not hit NOT NULL either.
    QVERIFY2(m_db->setLookJson(id, QString()), qPrintable(m_db->lastError()));
    QCOMPARE(m_db->image(id).lookJson, QString());
}

void TstCatalogDatabase::derivesABareFileName()
{
    const qint64 folder = m_db->addFolder(folderPath(QStringLiteral("photos")));
    QVERIFY(folder > 0);

    // A caller that fills fileName straight from the relative path. Stored as
    // "sub/photo.jpg" it would sort under 's' and the user would look for it
    // under 'p' and conclude the import dropped it.
    CatalogImage withPath;
    withPath.folderId     = folder;
    withPath.relativePath = QStringLiteral("sub/photo.jpg");
    withPath.fileName     = QStringLiteral("sub/photo.jpg");
    const qint64 id = m_db->upsertImage(withPath);
    QVERIFY2(id > 0, qPrintable(m_db->lastError()));
    QCOMPARE(m_db->image(id).fileName, QStringLiteral("photo.jpg"));

    // A missing fileName is derived from the relative path too.
    CatalogImage derived;
    derived.folderId     = folder;
    derived.relativePath = QStringLiteral("deep/deeper/quebec.jpg");
    QVERIFY(m_db->upsertImage(derived) > 0);

    CatalogImage plain;
    plain.folderId     = folder;
    plain.relativePath = QStringLiteral("alpha.jpg");
    QVERIFY(m_db->upsertImage(plain) > 0);

    // The point of all of the above: the sort lands under the right letter.
    CatalogFilter filter;
    filter.sortKey   = SortKey::FileName;
    filter.ascending = true;
    QCOMPARE(namesOf(m_db->query(filter)),
             (QStringList{QStringLiteral("alpha.jpg"),
                          QStringLiteral("photo.jpg"),
                          QStringLiteral("quebec.jpg")}));

    // A bare name that is *not* the last path segment is a deliberate display
    // override, and the implementation documents that it survives. Pinned here
    // so a future "always derive it" change is a conscious decision.
    CatalogImage override_;
    override_.folderId     = folder;
    override_.relativePath = QStringLiteral("raw/IMG_9999.CR3");
    override_.fileName     = QStringLiteral("Cover shot.cr3");
    const qint64 overrideId = m_db->upsertImage(override_);
    QVERIFY(overrideId > 0);
    QCOMPARE(m_db->image(overrideId).fileName, QStringLiteral("Cover shot.cr3"));
}

void TstCatalogDatabase::captureTimeRoundTrips()
{
    const qint64 folder = m_db->addFolder(folderPath(QStringLiteral("photos")));
    QVERIFY(folder > 0);

    // Unknown: plenty of files have no EXIF date, and it must come back
    // unknown rather than as 1970 or as "now".
    CatalogImage unknown;
    unknown.folderId     = folder;
    unknown.relativePath = QStringLiteral("no-exif.jpg");
    QVERIFY(!unknown.captureTime.isValid());
    const qint64 unknownId = m_db->upsertImage(unknown);
    QVERIFY2(unknownId > 0, qPrintable(m_db->lastError()));
    QVERIFY(!m_db->image(unknownId).captureTime.isValid());

    // Deliberately garbage, as a corrupt EXIF block produces.
    CatalogImage garbage;
    garbage.folderId     = folder;
    garbage.relativePath = QStringLiteral("bad-exif.jpg");
    garbage.captureTime  = QDateTime::fromString(QStringLiteral("not a date"), Qt::ISODate);
    QVERIFY(!garbage.captureTime.isValid());
    const qint64 garbageId = m_db->upsertImage(garbage);
    QVERIFY(garbageId > 0);
    QVERIFY(!m_db->image(garbageId).captureTime.isValid());

    // A scanned print from before the epoch. This is why the "unknown" test in
    // the source is `!= 0` and not `> 0`: a negative epoch is a real date.
    const QDateTime scanned(QDate(1965, 4, 3), QTime(11, 30, 0), QTimeZone::UTC);
    QVERIFY(scanned.toSecsSinceEpoch() < 0);

    CatalogImage old;
    old.folderId     = folder;
    old.relativePath = QStringLiteral("1965-print.tif");
    old.captureTime  = scanned;
    const qint64 oldId = m_db->upsertImage(old);
    QVERIFY(oldId > 0);
    QCOMPARE(m_db->image(oldId).captureTime, scanned);

    // And it sorts as the oldest thing in the catalog rather than wrapping.
    CatalogImage recent;
    recent.folderId     = folder;
    recent.relativePath = QStringLiteral("today.jpg");
    recent.captureTime  = baseTime(0);
    QVERIFY(m_db->upsertImage(recent) > 0);

    CatalogFilter filter;
    filter.sortKey   = SortKey::CaptureTime;
    filter.ascending = true;
    const QVector<CatalogImage> rows = m_db->query(filter);
    QCOMPARE(static_cast<int>(rows.size()), 4);
    // The two unknowns store 0, so they sit between 1965 and 2023.
    QCOMPARE(rows.first().fileName, QStringLiteral("1965-print.tif"));
    QCOMPARE(rows.last().fileName, QStringLiteral("today.jpg"));

    // fileModified travels the same path and must survive the same way.
    CatalogImage mtime;
    mtime.folderId     = folder;
    mtime.relativePath = QStringLiteral("mtime.jpg");
    mtime.fileModified = scanned;
    const qint64 mtimeId = m_db->upsertImage(mtime);
    QVERIFY(mtimeId > 0);
    QCOMPARE(m_db->image(mtimeId).fileModified, scanned);
}

void TstCatalogDatabase::removeFolderCascadesToItsImages()
{
    QVERIFY2(seedFilterFixture(), qPrintable(m_db->lastError()));

    const qint64 doomed = m_ids.value(QStringLiteral("one.jpg"));
    QVERIFY(doomed > 0);

    CatalogFilter everything;
    everything.hideRejected = false;
    QCOMPARE(m_db->queryCount(everything), 5);

    QVERIFY2(m_db->removeFolder(m_folderA), qPrintable(m_db->lastError()));

    // Folder A held four of the five. Nothing may be left orphaned behind the
    // folder tree, where the user can neither see it nor delete it.
    QCOMPARE(static_cast<int>(m_db->folders().size()), 1);
    QCOMPARE(m_db->queryCount(everything), 1);
    QCOMPARE(static_cast<int>(m_db->query(everything).size()), 1);
    QCOMPARE(m_db->query(everything).first().fileName, QStringLiteral("five.jpg"));
    QVERIFY(!m_db->image(doomed).isValid());
    QVERIFY(!m_db->hasImage(m_folderA, QStringLiteral("one.jpg")));
    QCOMPARE(m_db->stats().imageCount, 1);

    // Its keyword links went with it; "street" survives because five.jpg does.
    CatalogFilter byKeyword;
    byKeyword.hideRejected = false;
    byKeyword.keywords     = QStringList{QStringLiteral("studio")};
    QCOMPARE(m_db->queryCount(byKeyword), 0);
    byKeyword.keywords = QStringList{QStringLiteral("street")};
    QCOMPARE(m_db->queryCount(byKeyword), 1);

    // Removing it again is an error the caller is told about, not a silent yes.
    QVERIFY(!m_db->removeFolder(m_folderA));
    QVERIFY2(m_db->lastError().contains(QLatin1String("no folder")),
             qPrintable(m_db->lastError()));
    QVERIFY(!m_db->removeFolder(999999));
}

void TstCatalogDatabase::removeImageCascadesToItsKeywords()
{
    const qint64 folder = m_db->addFolder(folderPath(QStringLiteral("photos")));
    QVERIFY(folder > 0);

    CatalogImage a;
    a.folderId     = folder;
    a.relativePath = QStringLiteral("a.jpg");
    const qint64 aId = m_db->upsertImage(a);
    CatalogImage b;
    b.folderId     = folder;
    b.relativePath = QStringLiteral("b.jpg");
    const qint64 bId = m_db->upsertImage(b);
    QVERIFY(aId > 0 && bId > 0);

    QVERIFY(m_db->addKeyword(aId, QStringLiteral("shared")));
    QVERIFY(m_db->addKeyword(bId, QStringLiteral("shared")));
    QVERIFY(m_db->addKeyword(aId, QStringLiteral("solo")));

    const qint64 collection = m_db->createCollection(QStringLiteral("Set"));
    QVERIFY(m_db->addToCollection(collection, aId));

    QVERIFY2(m_db->removeImage(aId), qPrintable(m_db->lastError()));
    QVERIFY(!m_db->image(aId).isValid());
    QVERIFY(m_db->keywordsFor(aId).isEmpty());

    // The link rows are what matter: a stale image_keywords row would make the
    // keyword filter count a photo that no longer exists.
    CatalogFilter byKeyword;
    byKeyword.keywords = QStringList{QStringLiteral("solo")};
    QCOMPARE(m_db->queryCount(byKeyword), 0);
    QCOMPARE(static_cast<int>(m_db->query(byKeyword).size()), 0);

    byKeyword.keywords = QStringList{QStringLiteral("shared")};
    QCOMPARE(m_db->queryCount(byKeyword), 1);
    QCOMPARE(m_db->keywordsFor(bId), (QStringList{QStringLiteral("shared")}));

    // Collection membership went too, so removing it again must fail.
    QVERIFY(!m_db->removeFromCollection(collection, aId));

    QVERIFY(!m_db->removeImage(aId));
    QVERIFY2(m_db->lastError().contains(QLatin1String("no image")),
             qPrintable(m_db->lastError()));
}

void TstCatalogDatabase::hasImageNormalizesRelativePaths()
{
    const qint64 folder = m_db->addFolder(folderPath(QStringLiteral("photos")));
    QVERIFY(folder > 0);

    CatalogImage image;
    image.folderId     = folder;
    image.relativePath = QStringLiteral("trip/day 1/dsc_0001.arw");
    QVERIFY(m_db->upsertImage(image) > 0);

    // Every spelling the importer might hand over for the same file.
    QVERIFY(m_db->hasImage(folder, QStringLiteral("trip/day 1/dsc_0001.arw")));
    QVERIFY(m_db->hasImage(folder, QStringLiteral("./trip/day 1/dsc_0001.arw")));
    QVERIFY(m_db->hasImage(folder, QStringLiteral("/trip/day 1/dsc_0001.arw")));
    QVERIFY(m_db->hasImage(folder, QStringLiteral("trip/./day 1/dsc_0001.arw")));
    QVERIFY(m_db->hasImage(folder, QStringLiteral("trip/x/../day 1/dsc_0001.arw")));
    QVERIFY(m_db->hasImage(
        folder, QDir::toNativeSeparators(QStringLiteral("trip/day 1/dsc_0001.arw"))));

    QVERIFY(!m_db->hasImage(folder, QStringLiteral("trip/day 1/dsc_0002.arw")));
    QVERIFY(!m_db->hasImage(folder + 1, QStringLiteral("trip/day 1/dsc_0001.arw")));

    QVERIFY(!m_db->hasImage(folder, QString()));
    QVERIFY2(m_db->lastError().contains(QLatin1String("relativePath")),
             qPrintable(m_db->lastError()));
}

void TstCatalogDatabase::imageByPathResolvesAnAbsolutePath()
{
    const QString outer = folderPath(QStringLiteral("photos"));
    const QString inner = folderPath(QStringLiteral("photos/trip"));
    const qint64 outerId = m_db->addFolder(outer);
    const qint64 innerId = m_db->addFolder(inner);
    QVERIFY(outerId > 0 && innerId > 0);
    QVERIFY(outerId != innerId);

    CatalogImage underOuter;
    underOuter.folderId     = outerId;
    underOuter.relativePath = QStringLiteral("archive/dsc_0001.arw");
    const qint64 outerImage = m_db->upsertImage(underOuter);

    CatalogImage underInner;
    underInner.folderId     = innerId;
    underInner.relativePath = QStringLiteral("dsc_0002.arw");
    const qint64 innerImage = m_db->upsertImage(underInner);
    QVERIFY(outerImage > 0 && innerImage > 0);

    // absolutePath is assembled by the reader, so round-tripping it through
    // imageByPath() also checks the two agree on what a path looks like.
    const CatalogImage stored = m_db->image(outerImage);
    QVERIFY(!stored.absolutePath.isEmpty());
    QCOMPARE(m_db->imageByPath(stored.absolutePath).id, outerImage);
    QCOMPARE(m_db->imageByPath(QDir::toNativeSeparators(stored.absolutePath)).id, outerImage);

    // A file under the nested root must resolve to the nested root, which only
    // works because the roots are tried longest-first.
    const CatalogImage nested = m_db->image(innerImage);
    const CatalogImage found  = m_db->imageByPath(nested.absolutePath);
    QCOMPARE(found.id, innerImage);
    QCOMPARE(found.folderId, innerId);

    // ...and a path that only *looks* like it is under a root must not resolve
    // to a row the nested probe happened to find first.
    QVERIFY(!m_db->imageByPath(m_dir->filePath(QStringLiteral("elsewhere/dsc_0001.arw")))
                 .isValid());
    QVERIFY2(m_db->lastError().contains(QLatin1String("not in the catalog")),
             qPrintable(m_db->lastError()));

    QVERIFY(!m_db->imageByPath(QString()).isValid());
    QVERIFY2(m_db->lastError().contains(QLatin1String("empty")), qPrintable(m_db->lastError()));

    // Keywords come along, because the editor opens a photo by path and then
    // shows its tags.
    QVERIFY(m_db->addKeyword(outerImage, QStringLiteral("iceland")));
    QCOMPARE(m_db->imageByPath(stored.absolutePath).keywords,
             (QStringList{QStringLiteral("iceland")}));
}

// ==============================================================================
// Querying
// ==============================================================================
void TstCatalogDatabase::sortsByEveryKeyInBothDirections()
{
    const qint64 folder = m_db->addFolder(folderPath(QStringLiteral("photos")));
    QVERIFY(folder > 0);

    // The five sort keys are deliberately uncorrelated: each one produces a
    // different order, and none matches insertion order. A fixture where two
    // orders agreed would pass with the ORDER BY expressions swapped.
    struct Row
    {
        const char* name;
        qint64      size;
        int         captureOffset;
        int         rating;
        int         importOffset;
    };
    const Row rows[] = {
        {"delta.jpg",   500, 40, 3, 10},
        {"alpha.jpg",   900, 10, 5, 50},
        {"charlie.jpg", 100, 30, 1, 20},
        {"bravo.jpg",   300, 20, 4, 40},
        {"echo.jpg",    700,  0, 2, 30},
    };

    for (const Row& row : rows) {
        CatalogImage image;
        image.folderId     = folder;
        image.relativePath = QLatin1String(row.name);
        image.fileSize     = row.size;
        image.captureTime  = baseTime(row.captureOffset);
        image.rating       = row.rating;
        image.importedAt   = baseTime(row.importOffset);
        QVERIFY2(m_db->upsertImage(image) > 0, qPrintable(m_db->lastError()));
    }

    struct Expectation
    {
        SortKey     key;
        QStringList ascending;
    };
    const QVector<Expectation> expectations = {
        {SortKey::FileName,
         {QStringLiteral("alpha.jpg"), QStringLiteral("bravo.jpg"),
          QStringLiteral("charlie.jpg"), QStringLiteral("delta.jpg"),
          QStringLiteral("echo.jpg")}},
        {SortKey::FileSize,
         {QStringLiteral("charlie.jpg"), QStringLiteral("bravo.jpg"),
          QStringLiteral("delta.jpg"), QStringLiteral("echo.jpg"),
          QStringLiteral("alpha.jpg")}},
        {SortKey::CaptureTime,
         {QStringLiteral("echo.jpg"), QStringLiteral("alpha.jpg"),
          QStringLiteral("bravo.jpg"), QStringLiteral("charlie.jpg"),
          QStringLiteral("delta.jpg")}},
        {SortKey::Rating,
         {QStringLiteral("charlie.jpg"), QStringLiteral("echo.jpg"),
          QStringLiteral("delta.jpg"), QStringLiteral("bravo.jpg"),
          QStringLiteral("alpha.jpg")}},
        {SortKey::ImportedAt,
         {QStringLiteral("delta.jpg"), QStringLiteral("charlie.jpg"),
          QStringLiteral("echo.jpg"), QStringLiteral("bravo.jpg"),
          QStringLiteral("alpha.jpg")}},
    };

    for (const Expectation& expected : expectations) {
        CatalogFilter filter;
        filter.sortKey = expected.key;

        filter.ascending = true;
        QVERIFY2(namesOf(m_db->query(filter)) == expected.ascending,
                 qPrintable(QStringLiteral("sortKey %1 ascending: got [%2] want [%3]")
                                .arg(static_cast<int>(expected.key))
                                .arg(namesOf(m_db->query(filter)).join(QStringLiteral(", ")),
                                     expected.ascending.join(QStringLiteral(", ")))));

        // No ties exist in this fixture, so descending is exactly the reverse.
        filter.ascending = false;
        QVERIFY2(namesOf(m_db->query(filter)) == reversed(expected.ascending),
                 qPrintable(QStringLiteral("sortKey %1 descending: got [%2] want [%3]")
                                .arg(static_cast<int>(expected.key))
                                .arg(namesOf(m_db->query(filter)).join(QStringLiteral(", ")),
                                     reversed(expected.ascending).join(QStringLiteral(", ")))));
    }

    // A default-constructed filter is the Library's opening view, and the
    // header promises it is newest capture first.
    CatalogFilter def;
    QVERIFY(def.isDefault());
    QCOMPARE(namesOf(m_db->query(def)),
             reversed(expectations[2].ascending));

    // FileName sorting is NOCASE, matching the index it rides on; without that
    // "IMG_10" and "img_9" end up in separate blocks of the grid.
    CatalogImage upper;
    upper.folderId     = folder;
    upper.relativePath = QStringLiteral("Bravo2.jpg");
    QVERIFY(m_db->upsertImage(upper) > 0);
    CatalogFilter byName;
    byName.sortKey   = SortKey::FileName;
    byName.ascending = true;
    const QStringList names = namesOf(m_db->query(byName));
    QCOMPARE(names.indexOf(QStringLiteral("Bravo2.jpg")), 2);   // after bravo.jpg
}

void TstCatalogDatabase::filtersSelectTheExpectedRows()
{
    QVERIFY2(seedFilterFixture(), qPrintable(m_db->lastError()));

    const QStringList all = {QStringLiteral("one.jpg"), QStringLiteral("two.jpg"),
                             QStringLiteral("three.jpg"), QStringLiteral("four.jpg"),
                             QStringLiteral("five.jpg")};
    const QStringList notRejected = {QStringLiteral("one.jpg"), QStringLiteral("two.jpg"),
                                     QStringLiteral("four.jpg"), QStringLiteral("five.jpg")};

    // ---- hideRejected --------------------------------------------------------
    // Default-on, because a rejected photo is one the user has already decided
    // about and does not want to keep seeing.
    CatalogFilter def;
    QVERIFY(def.hideRejected);
    CHECK_FILTER("default", def, notRejected);

    CatalogFilter everything;
    everything.hideRejected = false;
    CHECK_FILTER("hideRejected=false", everything, all);

    // ---- minRating -----------------------------------------------------------
    CatalogFilter rating3;
    rating3.minRating = 3;
    CHECK_FILTER("minRating=3", rating3,
                 (QStringList{QStringLiteral("one.jpg"), QStringLiteral("two.jpg"),
                              QStringLiteral("five.jpg")}));

    CatalogFilter rating5;
    rating5.minRating = 5;
    CHECK_FILTER("minRating=5", rating5, (QStringList{QStringLiteral("one.jpg")}));

    // 0 means "no rating filter", not "rating >= 0", which would also pull in
    // the rejected row through a different route.
    CatalogFilter rating0;
    rating0.minRating = 0;
    CHECK_FILTER("minRating=0", rating0, notRejected);

    // ---- pickedOnly ----------------------------------------------------------
    CatalogFilter picked;
    picked.pickedOnly = true;
    CHECK_FILTER("pickedOnly", picked,
                 (QStringList{QStringLiteral("one.jpg"), QStringLiteral("five.jpg")}));

    // A picked photo is never rejected, so the two controls cannot conflict.
    CatalogFilter pickedShowAll;
    pickedShowAll.pickedOnly   = true;
    pickedShowAll.hideRejected = false;
    CHECK_FILTER("pickedOnly + show rejected", pickedShowAll,
                 (QStringList{QStringLiteral("one.jpg"), QStringLiteral("five.jpg")}));

    // ---- colorLabel ----------------------------------------------------------
    CatalogFilter red;
    red.colorLabel = ColorLabel::Red;
    CHECK_FILTER("colorLabel=Red", red,
                 (QStringList{QStringLiteral("one.jpg"), QStringLiteral("four.jpg")}));

    CatalogFilter purple;
    purple.colorLabel = ColorLabel::Purple;
    CHECK_FILTER("colorLabel=Purple", purple, QStringList());

    // None means "any label", including unlabelled — not "label = 0".
    CatalogFilter anyLabel;
    anyLabel.colorLabel = ColorLabel::None;
    CHECK_FILTER("colorLabel=None means any", anyLabel, notRejected);

    // ---- folderId ------------------------------------------------------------
    CatalogFilter inA;
    inA.folderId     = m_folderA;
    inA.hideRejected = false;
    CHECK_FILTER("folderId=A", inA,
                 (QStringList{QStringLiteral("one.jpg"), QStringLiteral("two.jpg"),
                              QStringLiteral("three.jpg"), QStringLiteral("four.jpg")}));

    CatalogFilter inB;
    inB.folderId = m_folderB;
    CHECK_FILTER("folderId=B", inB, (QStringList{QStringLiteral("five.jpg")}));

    CatalogFilter noSuchFolder;
    noSuchFolder.folderId = 999999;
    CHECK_FILTER("folderId=missing", noSuchFolder, QStringList());

    // ---- keywords (AND-ed) ---------------------------------------------------
    CatalogFilter portrait;
    portrait.keywords = QStringList{QStringLiteral("portrait")};
    CHECK_FILTER("keyword portrait", portrait,
                 (QStringList{QStringLiteral("one.jpg"), QStringLiteral("two.jpg"),
                              QStringLiteral("five.jpg")}));

    CatalogFilter both;
    both.keywords = QStringList{QStringLiteral("portrait"), QStringLiteral("studio")};
    CHECK_FILTER("keywords AND", both, (QStringList{QStringLiteral("one.jpg")}));

    // AND, not OR. Nothing carries both of these.
    CatalogFilter impossible;
    impossible.hideRejected = false;
    impossible.keywords = QStringList{QStringLiteral("portrait"), QStringLiteral("landscape")};
    CHECK_FILTER("keywords AND is not OR", impossible, QStringList());

    CatalogFilter mixedCase;
    mixedCase.keywords = QStringList{QStringLiteral("PORTRAIT")};
    CHECK_FILTER("keyword case-insensitive", mixedCase,
                 (QStringList{QStringLiteral("one.jpg"), QStringLiteral("two.jpg"),
                              QStringLiteral("five.jpg")}));

    // Blank entries come from a tokenised text field and must be ignored, not
    // matched as an empty keyword that nothing has.
    CatalogFilter padded;
    padded.keywords = QStringList{QStringLiteral("  portrait  "), QString(),
                                  QStringLiteral("   ")};
    CHECK_FILTER("keywords trimmed and blanks dropped", padded,
                 (QStringList{QStringLiteral("one.jpg"), QStringLiteral("two.jpg"),
                              QStringLiteral("five.jpg")}));

    // ---- searchText ----------------------------------------------------------
    CatalogFilter byFilename;
    byFilename.hideRejected = false;
    byFilename.searchText   = QStringLiteral("hree");
    CHECK_FILTER("search filename substring", byFilename,
                 (QStringList{QStringLiteral("three.jpg")}));

    CatalogFilter byCamera;
    byCamera.hideRejected = false;
    byCamera.searchText   = QStringLiteral("canon");
    CHECK_FILTER("search camera, case-insensitive", byCamera,
                 (QStringList{QStringLiteral("one.jpg"), QStringLiteral("three.jpg")}));

    // The same search with the default filter still hides the rejected row: the
    // clauses are AND-ed, and a search must not quietly re-reveal a reject.
    CatalogFilter byCameraDefault;
    byCameraDefault.searchText = QStringLiteral("canon");
    CHECK_FILTER("search obeys hideRejected", byCameraDefault,
                 (QStringList{QStringLiteral("one.jpg")}));

    CatalogFilter byLens;
    byLens.searchText = QStringLiteral("50mm");
    CHECK_FILTER("search lens", byLens, (QStringList{QStringLiteral("one.jpg")}));

    CatalogFilter byKeywordText;
    byKeywordText.searchText = QStringLiteral("street");
    CHECK_FILTER("search keyword", byKeywordText, (QStringList{QStringLiteral("five.jpg")}));

    CatalogFilter byKeywordCase;
    byKeywordCase.searchText = QStringLiteral("STUDIO");
    CHECK_FILTER("search keyword, case-insensitive", byKeywordCase,
                 (QStringList{QStringLiteral("one.jpg")}));

    CatalogFilter noMatch;
    noMatch.hideRejected = false;
    noMatch.searchText   = QStringLiteral("hasselblad");
    CHECK_FILTER("search with no match", noMatch, QStringList());

    // Whitespace is what a text box holds most of the time it is "empty".
    CatalogFilter blankSearch;
    blankSearch.searchText = QStringLiteral("   ");
    CHECK_FILTER("blank search is no search", blankSearch, notRejected);

    // ---- combinations --------------------------------------------------------
    CatalogFilter combined;
    combined.folderId   = m_folderA;
    combined.minRating  = 3;
    combined.searchText = QStringLiteral("canon");
    CHECK_FILTER("folder + rating + search", combined,
                 (QStringList{QStringLiteral("one.jpg")}));

    CatalogFilter narrow;
    narrow.pickedOnly = true;
    narrow.colorLabel = ColorLabel::Blue;
    narrow.keywords   = QStringList{QStringLiteral("portrait")};
    CHECK_FILTER("picked + blue + keyword", narrow,
                 (QStringList{QStringLiteral("five.jpg")}));

    CatalogFilter contradictory;
    contradictory.pickedOnly = true;
    contradictory.minRating  = 5;
    contradictory.folderId   = m_folderB;
    CHECK_FILTER("contradictory combination", contradictory, QStringList());
}

void TstCatalogDatabase::queryCountAgreesWithQueryForEveryFilter()
{
    QVERIFY2(seedFilterFixture(), qPrintable(m_db->lastError()));

    // query() and queryCount() are separate statements built from a shared
    // predicate. If they ever disagree the grid renders one number and scrolls
    // a different one, so the pair is swept rather than spot-checked.
    QVector<CatalogFilter> filters;
    filters.append(CatalogFilter{});

    const auto with = [&filters](auto&& mutate) {
        CatalogFilter filter;
        mutate(filter);
        filters.append(filter);
    };

    with([](CatalogFilter& f) { f.hideRejected = false; });
    with([](CatalogFilter& f) { f.minRating = 1; });
    with([](CatalogFilter& f) { f.minRating = 3; });
    with([](CatalogFilter& f) { f.minRating = 5; });
    with([](CatalogFilter& f) { f.minRating = 6; });           // above the scale
    with([](CatalogFilter& f) { f.pickedOnly = true; });
    with([](CatalogFilter& f) { f.pickedOnly = true; f.hideRejected = false; });
    with([](CatalogFilter& f) { f.colorLabel = ColorLabel::Red; });
    with([](CatalogFilter& f) { f.colorLabel = ColorLabel::Blue; });
    with([](CatalogFilter& f) { f.colorLabel = ColorLabel::Yellow; });
    with([this](CatalogFilter& f) { f.folderId = m_folderA; });
    with([this](CatalogFilter& f) { f.folderId = m_folderB; f.hideRejected = false; });
    with([](CatalogFilter& f) { f.folderId = 987654; });
    with([](CatalogFilter& f) { f.keywords = QStringList{QStringLiteral("portrait")}; });
    with([](CatalogFilter& f) {
        f.keywords = QStringList{QStringLiteral("portrait"), QStringLiteral("studio")};
    });
    with([](CatalogFilter& f) { f.keywords = QStringList{QStringLiteral("nothing")}; });
    with([](CatalogFilter& f) { f.searchText = QStringLiteral("canon"); });
    with([](CatalogFilter& f) { f.searchText = QStringLiteral("e"); });
    with([](CatalogFilter& f) { f.searchText = QStringLiteral("'"); });
    with([](CatalogFilter& f) { f.searchText = QStringLiteral("%"); });
    with([](CatalogFilter& f) { f.searchText = QStringLiteral("_"); });
    with([this](CatalogFilter& f) {
        f.folderId     = m_folderA;
        f.minRating    = 1;
        f.colorLabel   = ColorLabel::Red;
        f.searchText   = QStringLiteral("canon");
        f.keywords     = QStringList{QStringLiteral("portrait")};
        f.hideRejected = false;
    });

    // The sort key changes the ORDER BY, not the row set. Sweeping it here
    // catches a predicate accidentally composed into the sort expression.
    for (const SortKey key : {SortKey::CaptureTime, SortKey::FileName, SortKey::Rating,
                              SortKey::ImportedAt, SortKey::FileSize}) {
        for (const bool ascending : {true, false}) {
            CatalogFilter filter;
            filter.sortKey   = key;
            filter.ascending = ascending;
            filter.minRating = 1;
            filters.append(filter);
        }
    }

    for (int i = 0; i < filters.size(); ++i) {
        const CatalogFilter& filter = filters[i];
        const QVector<CatalogImage> rows = m_db->query(filter);
        const int counted = m_db->queryCount(filter);
        QVERIFY2(counted == static_cast<int>(rows.size()),
                 qPrintable(QStringLiteral("filter #%1: queryCount()=%2 but query() gave %3")
                                .arg(i).arg(counted).arg(rows.size())));

        // Paging must not move the count either — it is the total, not the page.
        QCOMPARE(m_db->queryCount(filter), counted);
        const QVector<CatalogImage> firstPage = m_db->query(filter, 2, 0);
        QVERIFY(static_cast<int>(firstPage.size()) <= 2);
        QVERIFY(static_cast<int>(firstPage.size()) <= counted);
        QCOMPARE(m_db->queryCount(filter), counted);
    }
}

void TstCatalogDatabase::pagingReproducesTheUnpagedOrder()
{
    const qint64 folder = m_db->addFolder(folderPath(QStringLiteral("photos")));
    QVERIFY(folder > 0);

    // Groups of four share a capture time, which is exactly what a burst looks
    // like. Without the id tiebreak, LIMIT/OFFSET is free to reorder within a
    // group and the grid then repeats one photo and drops another.
    constexpr int kRows = 83;
    for (int i = 0; i < kRows; ++i) {
        CatalogImage image;
        image.folderId     = folder;
        image.relativePath = QStringLiteral("burst/img_%1.jpg").arg(i, 4, 10, QLatin1Char('0'));
        image.captureTime  = baseTime(i / 4);
        image.rating       = i % 6;
        image.fileSize     = 100 * (i % 7);
        image.importedAt   = baseTime(i % 5);
        QVERIFY2(m_db->upsertImage(image) > 0, qPrintable(m_db->lastError()));
    }

    for (const SortKey key : {SortKey::CaptureTime, SortKey::Rating, SortKey::FileSize,
                              SortKey::ImportedAt, SortKey::FileName}) {
        for (const bool ascending : {true, false}) {
            CatalogFilter filter;
            filter.sortKey   = key;
            filter.ascending = ascending;

            const QVector<qint64> unpaged = idsOf(m_db->query(filter));
            QCOMPARE(static_cast<int>(unpaged.size()), kRows);
            QCOMPARE(m_db->queryCount(filter), kRows);

            constexpr int kPage = 7;
            QVector<qint64> paged;
            for (int offset = 0; offset < kRows + kPage; offset += kPage) {
                const QVector<CatalogImage> page = m_db->query(filter, kPage, offset);
                QVERIFY(static_cast<int>(page.size()) <= kPage);
                paged.append(idsOf(page));
            }

            QVERIFY2(paged == unpaged,
                     qPrintable(QStringLiteral("sortKey %1 %2: paging changed the order")
                                    .arg(static_cast<int>(key))
                                    .arg(ascending ? QStringLiteral("asc")
                                                   : QStringLiteral("desc"))));
        }
    }

    CatalogFilter filter;
    // Past the end is empty, not an error and not a wrap-around.
    QVERIFY(m_db->query(filter, 10, kRows).isEmpty());
    QVERIFY(m_db->query(filter, 10, kRows * 10).isEmpty());
    // limit < 0 is "everything", which is what the header documents.
    QCOMPARE(static_cast<int>(m_db->query(filter, -1, 0).size()), kRows);
    // A negative offset is clamped rather than handed to SQLite.
    QCOMPARE(idsOf(m_db->query(filter, 5, -10)), idsOf(m_db->query(filter, 5, 0)));
    // A partial last page is exactly the remainder.
    QCOMPARE(static_cast<int>(m_db->query(filter, 10, kRows - 3).size()), 3);
}

void TstCatalogDatabase::resistsSqlInjectionInSearchText()
{
    QVERIFY2(seedFilterFixture(), qPrintable(m_db->lastError()));

    CatalogFilter everything;
    everything.hideRejected = false;
    QCOMPARE(m_db->queryCount(everything), 5);

    // searchText comes straight out of a text box. These have to be data.
    const QStringList payloads = {
        QStringLiteral("'; DROP TABLE images; --"),
        QStringLiteral("' OR '1'='1"),
        QStringLiteral("%' OR '1'='1' --"),
        QStringLiteral("\" OR 1=1 --"),
        QStringLiteral("'); DELETE FROM folders; --"),
        QStringLiteral("' UNION SELECT 1,2,3,4,5,6,7,8,9,10 --"),
        QStringLiteral("1'; UPDATE images SET rating = 0; --"),
    };

    for (const QString& payload : payloads) {
        CatalogFilter filter;
        filter.hideRejected = false;
        filter.searchText   = payload;

        const QVector<CatalogImage> rows = m_db->query(filter);
        QVERIFY2(rows.isEmpty(),
                 qPrintable(QStringLiteral("payload %1 matched %2 rows")
                                .arg(payload).arg(rows.size())));
        QVERIFY2(m_db->queryCount(filter) == 0, qPrintable(payload));

        // And the catalog is untouched after each one, not just at the end.
        QVERIFY2(m_db->queryCount(everything) == 5, qPrintable(payload));
    }

    // Nothing was dropped, deleted or rewritten.
    QCOMPARE(static_cast<int>(m_db->query(everything).size()), 5);
    QCOMPARE(static_cast<int>(m_db->folders().size()), 2);
    QCOMPARE(m_db->image(m_ids.value(QStringLiteral("one.jpg"))).rating, 5);
    const CatalogStats stats = m_db->stats();
    QCOMPARE(stats.imageCount, 5);
    QCOMPARE(stats.folderCount, 2);

    // The same discipline has to hold for the keyword filter, which builds one
    // EXISTS clause per entry.
    CatalogFilter keywordInjection;
    keywordInjection.hideRejected = false;
    keywordInjection.keywords =
        QStringList{QStringLiteral("portrait' OR '1'='1"), QStringLiteral("x'; DROP TABLE keywords; --")};
    QCOMPARE(static_cast<int>(m_db->query(keywordInjection).size()), 0);
    QCOMPARE(m_db->queryCount(keywordInjection), 0);
    QCOMPARE(m_db->queryCount(everything), 5);
    QVERIFY(m_db->allKeywords().contains(QStringLiteral("portrait")));
}

void TstCatalogDatabase::treatsLikeMetacharactersLiterally()
{
    const qint64 folder = m_db->addFolder(folderPath(QStringLiteral("photos")));
    QVERIFY(folder > 0);

    struct Row
    {
        const char* name;
        const char* camera;
    };
    const Row rows[] = {
        {"50%-crop.jpg", "Canon EOS R5"},
        {"50-crop.jpg",  "Canon EOS R6"},
        {"a_b.jpg",      "Nikon Z9"},
        {"axb.jpg",      "Nikon Z8"},
        {"plain.jpg",    "Leica M11"},
    };
    for (const Row& row : rows) {
        CatalogImage image;
        image.folderId     = folder;
        image.relativePath = QLatin1String(row.name);
        image.cameraModel  = QLatin1String(row.camera);
        QVERIFY2(m_db->upsertImage(image) > 0, qPrintable(m_db->lastError()));
    }

    // A search for "50%" must find the one file with a percent sign in its
    // name, not every file whose name starts with "50".
    CatalogFilter percent;
    percent.searchText = QStringLiteral("50%");
    CHECK_FILTER("percent is literal", percent, (QStringList{QStringLiteral("50%-crop.jpg")}));

    CatalogFilter bareSign;
    bareSign.searchText = QStringLiteral("%");
    CHECK_FILTER("a lone percent matches only a literal one", bareSign,
                 (QStringList{QStringLiteral("50%-crop.jpg")}));

    // "_" is LIKE's single-character wildcard; unescaped, "a_b" would also
    // match "axb" and the user would think the search was broken.
    CatalogFilter underscore;
    underscore.searchText = QStringLiteral("a_b");
    CHECK_FILTER("underscore is literal", underscore, (QStringList{QStringLiteral("a_b.jpg")}));

    CatalogFilter bareUnderscore;
    bareUnderscore.searchText = QStringLiteral("_");
    CHECK_FILTER("a lone underscore matches only a literal one", bareUnderscore,
                 (QStringList{QStringLiteral("a_b.jpg")}));

    // The escape character itself. If '\' were passed through unescaped the
    // pattern would end in a dangling escape and SQLite would reject it.
    CatalogImage backslash;
    backslash.folderId     = folder;
    backslash.relativePath = QStringLiteral("meta.jpg");
    backslash.cameraModel  = QStringLiteral("Maker\\Model");
    QVERIFY2(m_db->upsertImage(backslash) > 0, qPrintable(m_db->lastError()));

    CatalogFilter escaped;
    escaped.searchText = QStringLiteral("Maker\\Model");
    CHECK_FILTER("a backslash is literal", escaped, (QStringList{QStringLiteral("meta.jpg")}));

    CatalogFilter loneBackslash;
    loneBackslash.searchText = QStringLiteral("\\");
    CHECK_FILTER("a lone backslash is literal", loneBackslash,
                 (QStringList{QStringLiteral("meta.jpg")}));

    CatalogFilter noWildcardMatch;
    noWildcardMatch.searchText = QStringLiteral("%plain%");
    CHECK_FILTER("wildcards in the needle match nothing", noWildcardMatch, QStringList());
}

// ==============================================================================
// User data, keywords, collections, misc
// ==============================================================================
void TstCatalogDatabase::userDataSettersClampAndReportMissingRows()
{
    const qint64 folder = m_db->addFolder(folderPath(QStringLiteral("photos")));
    CatalogImage image;
    image.folderId     = folder;
    image.relativePath = QStringLiteral("a.jpg");
    const qint64 id = m_db->upsertImage(image);
    QVERIFY2(id > 0, qPrintable(m_db->lastError()));

    // The stars widget can only produce 0..5, but a preset, a keyboard shortcut
    // or a plugin can produce anything, and the column has a CHECK constraint
    // that would otherwise turn a stray value into a failed write.
    QVERIFY(m_db->setRating(id, 3));
    QCOMPARE(m_db->image(id).rating, 3);
    QVERIFY(m_db->setRating(id, 0));
    QCOMPARE(m_db->image(id).rating, 0);
    QVERIFY(m_db->setRating(id, 5));
    QCOMPARE(m_db->image(id).rating, 5);
    QVERIFY(m_db->setRating(id, 6));
    QCOMPARE(m_db->image(id).rating, 5);
    QVERIFY(m_db->setRating(id, 1000));
    QCOMPARE(m_db->image(id).rating, 5);
    QVERIFY(m_db->setRating(id, -1));
    QCOMPARE(m_db->image(id).rating, 0);
    QVERIFY(m_db->setRating(id, -1000));
    QCOMPARE(m_db->image(id).rating, 0);

    // Same for the enums, which a cast can put out of range.
    QVERIFY(m_db->setFlag(id, static_cast<ImageFlag>(42)));
    QCOMPARE(static_cast<int>(m_db->image(id).flag), static_cast<int>(ImageFlag::Picked));
    QVERIFY(m_db->setFlag(id, static_cast<ImageFlag>(-42)));
    QCOMPARE(static_cast<int>(m_db->image(id).flag), static_cast<int>(ImageFlag::Rejected));
    QVERIFY(m_db->setFlag(id, ImageFlag::None));
    QCOMPARE(static_cast<int>(m_db->image(id).flag), static_cast<int>(ImageFlag::None));

    QVERIFY(m_db->setColorLabel(id, static_cast<ColorLabel>(99)));
    QCOMPARE(static_cast<int>(m_db->image(id).colorLabel), static_cast<int>(ColorLabel::Purple));
    QVERIFY(m_db->setColorLabel(id, static_cast<ColorLabel>(-5)));
    QCOMPARE(static_cast<int>(m_db->image(id).colorLabel), static_cast<int>(ColorLabel::None));

    QVERIFY(m_db->setLookJson(id, QStringLiteral("{\"exposure\":1.0}")));
    QCOMPARE(m_db->image(id).lookJson, QStringLiteral("{\"exposure\":1.0}"));

    // A stale id from a deleted photo must be reported, not silently absorbed:
    // the UI would otherwise show a rating that was never stored.
    const qint64 missing = id + 5000;
    QVERIFY(!m_db->setRating(missing, 3));
    QVERIFY2(m_db->lastError().contains(QLatin1String("no image")), qPrintable(m_db->lastError()));
    QVERIFY(!m_db->setFlag(missing, ImageFlag::Picked));
    QVERIFY(!m_db->setColorLabel(missing, ColorLabel::Red));
    QVERIFY(!m_db->setLookJson(missing, QStringLiteral("{}")));
}

void TstCatalogDatabase::statsCountsCorrectly()
{
    // Empty is a real state: the first launch shows it.
    const CatalogStats empty = m_db->stats();
    QCOMPARE(empty.folderCount, 0);
    QCOMPARE(empty.imageCount, 0);
    QCOMPARE(empty.rawCount, 0);
    QCOMPARE(empty.pickedCount, 0);
    QCOMPARE(empty.rejectedCount, 0);

    QVERIFY2(seedFilterFixture(), qPrintable(m_db->lastError()));

    const CatalogStats stats = m_db->stats();
    QCOMPARE(stats.folderCount, 2);
    QCOMPARE(stats.imageCount, 5);
    QCOMPARE(stats.rawCount, 2);        // one.jpg and three.jpg
    QCOMPARE(stats.pickedCount, 2);     // one.jpg and five.jpg
    QCOMPARE(stats.rejectedCount, 1);   // three.jpg

    // Counts are over the whole catalog, not the current filter, and they track
    // edits made after the import.
    QVERIFY(m_db->setFlag(m_ids.value(QStringLiteral("two.jpg")), ImageFlag::Rejected));
    QVERIFY(m_db->setFlag(m_ids.value(QStringLiteral("one.jpg")), ImageFlag::None));
    const CatalogStats after = m_db->stats();
    QCOMPARE(after.pickedCount, 1);
    QCOMPARE(after.rejectedCount, 2);
    QCOMPARE(after.imageCount, 5);

    // A folder with no images still counts as a folder.
    QVERIFY(m_db->addFolder(folderPath(QStringLiteral("photos/empty"))) > 0);
    QCOMPARE(m_db->stats().folderCount, 3);
    QCOMPARE(m_db->stats().imageCount, 5);

    QVERIFY(m_db->removeImage(m_ids.value(QStringLiteral("three.jpg"))));
    const CatalogStats pruned = m_db->stats();
    QCOMPARE(pruned.imageCount, 4);
    QCOMPARE(pruned.rawCount, 1);
    QCOMPARE(pruned.rejectedCount, 1);
}

void TstCatalogDatabase::rollbackUndoesEverySinceBegin()
{
    const qint64 folder = m_db->addFolder(folderPath(QStringLiteral("photos")));
    QVERIFY(folder > 0);

    CatalogImage keeper;
    keeper.folderId     = folder;
    keeper.relativePath = QStringLiteral("keeper.jpg");
    const qint64 keeperId = m_db->upsertImage(keeper);
    QVERIFY(keeperId > 0);
    QVERIFY(m_db->setRating(keeperId, 4));

    // The importer wraps a whole folder scan in one transaction; if it aborts
    // half way the catalog must look exactly as it did before it started.
    QVERIFY2(m_db->transaction(), qPrintable(m_db->lastError()));
    for (int i = 0; i < 5; ++i) {
        CatalogImage image;
        image.folderId     = folder;
        image.relativePath = QStringLiteral("doomed_%1.jpg").arg(i);
        QVERIFY(m_db->upsertImage(image) > 0);
    }
    QVERIFY(m_db->setRating(keeperId, 1));
    QVERIFY(m_db->addKeyword(keeperId, QStringLiteral("provisional")));
    // Visible inside the transaction, which is what makes the rollback a real
    // undo rather than a no-op over writes that never landed.
    QCOMPARE(m_db->queryCount(CatalogFilter{}), 6);
    QCOMPARE(m_db->image(keeperId).rating, 1);

    QVERIFY2(m_db->rollback(), qPrintable(m_db->lastError()));

    QCOMPARE(m_db->queryCount(CatalogFilter{}), 1);
    QCOMPARE(static_cast<int>(m_db->query(CatalogFilter{}).size()), 1);
    QCOMPARE(m_db->image(keeperId).rating, 4);
    QVERIFY(m_db->keywordsFor(keeperId).isEmpty());
    QVERIFY(m_db->allKeywords().isEmpty());

    // The prepared statements survive a rollback; the next write must work.
    CatalogImage after;
    after.folderId     = folder;
    after.relativePath = QStringLiteral("after.jpg");
    QVERIFY2(m_db->upsertImage(after) > 0, qPrintable(m_db->lastError()));
    QCOMPARE(m_db->queryCount(CatalogFilter{}), 2);

    // And the committed path really does commit.
    QVERIFY(m_db->transaction());
    CatalogImage committed;
    committed.folderId     = folder;
    committed.relativePath = QStringLiteral("committed.jpg");
    QVERIFY(m_db->upsertImage(committed) > 0);
    QVERIFY2(m_db->commit(), qPrintable(m_db->lastError()));
    QCOMPARE(m_db->queryCount(CatalogFilter{}), 3);
    QVERIFY(!m_db->rollback());   // nothing open to roll back
}

void TstCatalogDatabase::keywordsAreCaseInsensitive()
{
    const qint64 folder = m_db->addFolder(folderPath(QStringLiteral("photos")));
    CatalogImage image;
    image.folderId     = folder;
    image.relativePath = QStringLiteral("a.jpg");
    const qint64 id = m_db->upsertImage(image);
    CatalogImage other;
    other.folderId     = folder;
    other.relativePath = QStringLiteral("b.jpg");
    const qint64 otherId = m_db->upsertImage(other);
    QVERIFY(id > 0 && otherId > 0);

    // "Portrait" and "portrait" are one tag. Two rows would split the keyword
    // list in the UI and make the filter miss half the photos.
    QVERIFY2(m_db->addKeyword(id, QStringLiteral("Portrait")), qPrintable(m_db->lastError()));
    QVERIFY(m_db->addKeyword(id, QStringLiteral("portrait")));
    QVERIFY(m_db->addKeyword(otherId, QStringLiteral("PORTRAIT")));

    QCOMPARE(m_db->keywordsFor(id), (QStringList{QStringLiteral("Portrait")}));
    QCOMPARE(m_db->allKeywords(), (QStringList{QStringLiteral("Portrait")}));

    CatalogFilter filter;
    filter.keywords = QStringList{QStringLiteral("pOrTrAiT")};
    QCOMPARE(m_db->queryCount(filter), 2);
    QCOMPARE(static_cast<int>(m_db->query(filter).size()), 2);

    // Surrounding whitespace comes from a comma-separated tag field.
    QVERIFY(m_db->addKeyword(id, QStringLiteral("   beach   ")));
    QCOMPARE(m_db->keywordsFor(id),
             (QStringList{QStringLiteral("beach"), QStringLiteral("Portrait")}));

    QVERIFY(!m_db->addKeyword(id, QString()));
    QVERIFY(!m_db->addKeyword(id, QStringLiteral("   ")));
    QVERIFY2(m_db->lastError().contains(QLatin1String("empty")), qPrintable(m_db->lastError()));

    // Removal is case-insensitive too, and the tag itself is garbage-collected
    // once nothing carries it — otherwise the keyword list only ever grows.
    QVERIFY2(m_db->removeKeyword(id, QStringLiteral("BEACH")), qPrintable(m_db->lastError()));
    QVERIFY(!m_db->allKeywords().contains(QStringLiteral("beach")));
    QCOMPARE(m_db->keywordsFor(id), (QStringList{QStringLiteral("Portrait")}));

    // Still carried by b.jpg, so it must not be collected.
    QVERIFY(m_db->removeKeyword(id, QStringLiteral("portrait")));
    QCOMPARE(m_db->allKeywords(), (QStringList{QStringLiteral("Portrait")}));
    QCOMPARE(m_db->keywordsFor(otherId), (QStringList{QStringLiteral("Portrait")}));

    QVERIFY(!m_db->removeKeyword(id, QStringLiteral("portrait")));
    QVERIFY2(m_db->lastError().contains(QLatin1String("not tagged")),
             qPrintable(m_db->lastError()));

    QVERIFY(m_db->removeKeyword(otherId, QStringLiteral("Portrait")));
    QVERIFY(m_db->allKeywords().isEmpty());

    // Tagging a photo that does not exist is a foreign-key violation, and
    // INSERT OR IGNORE does not suppress those.
    QVERIFY2(!m_db->addKeyword(id + 5000, QStringLiteral("ghost")),
             "tagging a nonexistent image must fail");
}

void TstCatalogDatabase::createCollectionIsIdempotentOnName()
{
    const qint64 first = m_db->createCollection(QStringLiteral("Best of 2026"));
    QVERIFY2(first > 0, qPrintable(m_db->lastError()));

    // "New collection" in the UI is also how an existing one gets re-selected.
    QCOMPARE(m_db->createCollection(QStringLiteral("Best of 2026")), first);
    QCOMPARE(m_db->createCollection(QStringLiteral("  Best of 2026  ")), first);
    QCOMPARE(m_db->createCollection(QStringLiteral("BEST OF 2026")), first);

    const qint64 second = m_db->createCollection(QStringLiteral("Rejects"));
    QVERIFY(second > 0);
    QVERIFY(second != first);

    QCOMPARE(m_db->createCollection(QString()), static_cast<qint64>(-1));
    QVERIFY2(m_db->lastError().contains(QLatin1String("empty")), qPrintable(m_db->lastError()));
    QCOMPARE(m_db->createCollection(QStringLiteral("    ")), static_cast<qint64>(-1));

    // Membership, which is the part re-import must not disturb.
    const qint64 folder = m_db->addFolder(folderPath(QStringLiteral("photos")));
    CatalogImage image;
    image.folderId     = folder;
    image.relativePath = QStringLiteral("a.jpg");
    const qint64 id = m_db->upsertImage(image);
    QVERIFY(id > 0);

    QVERIFY2(m_db->addToCollection(first, id), qPrintable(m_db->lastError()));
    QVERIFY(m_db->addToCollection(first, id));           // OR IGNORE: adding twice is fine
    QVERIFY(m_db->removeFromCollection(first, id));
    QVERIFY(!m_db->removeFromCollection(first, id));     // but removing twice is not
    QVERIFY2(m_db->lastError().contains(QLatin1String("not in collection")),
             qPrintable(m_db->lastError()));

    // Foreign keys are enforced, so a stale id cannot create a dangling row.
    QVERIFY(!m_db->addToCollection(first, id + 5000));
    QVERIFY(!m_db->addToCollection(first + 5000, id));

    QVERIFY(m_db->addToCollection(first, id));
    QVERIFY2(m_db->deleteCollection(first), qPrintable(m_db->lastError()));
    QVERIFY(!m_db->deleteCollection(first));
    // Deleting the collection must not have taken the photo with it.
    QVERIFY(m_db->image(id).isValid());
    // ...and the name is free again, with a new id.
    const qint64 reborn = m_db->createCollection(QStringLiteral("Best of 2026"));
    QVERIFY(reborn > 0);
    QVERIFY2(reborn != first, "AUTOINCREMENT must not hand back a retired id");
}

// ==============================================================================
// Concurrency
// ==============================================================================
void TstCatalogDatabase::secondInstanceOnTheSameThreadWorks()
{
    // Two instances in one thread is not exotic: the importer is constructed
    // before its thread starts, and a dialog can open the catalog to preview a
    // folder while the Library holds it. The connection names must not collide.
    CatalogDatabase second;
    QVERIFY2(second.open(m_db->databasePath()), qPrintable(second.lastError()));
    QVERIFY(second.isOpen());
    QVERIFY(m_db->isOpen());
    QCOMPARE(second.databasePath(), m_db->databasePath());

    const qint64 folder = m_db->addFolder(folderPath(QStringLiteral("photos")));
    QVERIFY(folder > 0);

    CatalogImage image;
    image.folderId     = folder;
    image.relativePath = QStringLiteral("a.jpg");
    const qint64 id = m_db->upsertImage(image);
    QVERIFY2(id > 0, qPrintable(m_db->lastError()));

    // A committed write through one handle is visible through the other.
    QCOMPARE(second.queryCount(CatalogFilter{}), 1);
    QCOMPARE(second.image(id).id, id);

    QVERIFY2(second.setRating(id, 5), qPrintable(second.lastError()));
    QVERIFY(second.addKeyword(id, QStringLiteral("shared")));
    QCOMPARE(m_db->image(id).rating, 5);
    QCOMPARE(m_db->keywordsFor(id), (QStringList{QStringLiteral("shared")}));

    // Closing one must leave the other completely functional; a shared
    // connection name would take both down here.
    second.close();
    QVERIFY(!second.isOpen());
    QVERIFY(m_db->isOpen());
    QCOMPARE(m_db->queryCount(CatalogFilter{}), 1);
    QVERIFY2(m_db->setRating(id, 2), qPrintable(m_db->lastError()));

    // A third instance opened after the second was closed must work too, which
    // is where a connection name that is reused rather than retired breaks.
    CatalogDatabase third;
    QVERIFY2(third.open(m_db->databasePath()), qPrintable(third.lastError()));
    QCOMPARE(third.image(id).rating, 2);
}

void TstCatalogDatabase::workerThreadWritesWhileMainThreadReads()
{
    const QString path   = m_db->databasePath();
    const qint64  folder = m_db->addFolder(folderPath(QStringLiteral("photos")));
    QVERIFY(folder > 0);

    // The shape of a real import: a worker owning its own CatalogDatabase
    // writes a folder inside one transaction while the Library keeps drawing.
    // Under WAL neither side may block, error or trip a Qt connection warning.
    constexpr int kRows = 300;
    WriterThread worker(path, folder, kRows);
    worker.start();

    int reads = 0;
    // Bounded so a stuck worker fails the test rather than hanging CI.
    constexpr int kMaxReads = 200000;
    while (!worker.isFinished() && reads < kMaxReads) {
        const int counted = m_db->queryCount(CatalogFilter{});
        QVERIFY2(counted >= 0, qPrintable(m_db->lastError()));
        QVERIFY2(counted <= kRows, "the reader saw more rows than the writer ever wrote");
        ++reads;
    }
    QVERIFY2(worker.wait(30000), "the writer thread did not finish");
    QVERIFY2(worker.error.isEmpty(), qPrintable(worker.error));
    QVERIFY2(reads > 0, "the main thread never got a read in");

    QCOMPARE(m_db->queryCount(CatalogFilter{}), kRows);
    QCOMPARE(static_cast<int>(m_db->query(CatalogFilter{}).size()), kRows);
    QCOMPARE(m_db->stats().imageCount, kRows);
    QCOMPARE(m_db->stats().rawCount, kRows);

    // The main thread's handle is still fully usable after the worker's is gone.
    const CatalogImage sample = m_db->query(CatalogFilter{}, 1, 0).value(0);
    QVERIFY(sample.isValid());
    QVERIFY2(m_db->setRating(sample.id, 5), qPrintable(m_db->lastError()));
    QCOMPARE(m_db->image(sample.id).rating, 5);
}

QTEST_MAIN(TstCatalogDatabase)
#include "tst_catalogdatabase.moc"
