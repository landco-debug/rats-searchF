/**
 * @file test_manticore_queries.cpp
 * @brief Integration test that starts a real Manticore instance and drives the
 *        new data layer (rats::data::Manticore + Database + TorrentRepository).
 *
 * Verifies the end-to-end CRUD / search / statistics path against a live
 * searchd, replacing the old raw-SphinxQL query test. Requires the bundled
 * searchd executable to be discoverable via the standard import paths.
 */

#include <QDir>
#include <QFileInfo>
#include <QJsonObject>
#include <QSet>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest/QtTest>

#include "data/database.h"
#include "data/manticore.h"
#include "data/query.h"
#include "data/row_id.h"
#include "data/torrent_repository.h"
#include "domain/content.h"
#include "domain/torrent.h"
#include "services/database_dump.h"
#include "services/database_exporter.h"
#include "services/database_importer.h"
#include "services/filter_policy.h"
#include "services/indexing_service.h"

using rats::data::Database;
using rats::data::Manticore;
using rats::data::SelectQuery;
using rats::data::TorrentRepository;
using rats::domain::ContentCategory;
using rats::domain::ContentType;
using rats::domain::File;
using rats::domain::Torrent;

class TestManticoreQueries : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void testConnected();
    void testAddAndGet();
    void testExists();
    void testGetWithFiles();
    void testSearchByName();
    void testSearchByHash();
    void testUpdateTrackerCounts();
    void testMergeInfoSignals();
    void testVerifiedSourceReplacesStaleInfo();
    void testStatistics();
    void testRecent();
    void testTop();
    void testRemove();
    void testSelectQueryAgainstLiveIndex();
    void testPageAfterIdWalksPastMaxMatches();
    void testRowIdIsDerivedFromHash();
    void testFilesRowSharesTheTorrentId();
    void testReAddDoesNotDuplicateRow();
    void testRowIdCollisionIsRefused();
    void testBatchRowIdCollisionIsRefused();
    void testBatchDuplicateHashCollapsesToOneRow();
    void testMigrationReKeysLegacyRows();
    void testUpdateAppliesAndKeepsSearchable();

    void testExportImportRoundTrip();
    void testImportIsAMergeNotADuplicate();
    void testCancelledExportLeavesNoFile();
    void testInterruptedImportResumesWhereItStopped();

private:
    // Build a valid torrent with a distinct 40-hex hash derived from `n`.
    Torrent makeTorrent(
        int n, const QString& name, ContentType type = ContentType::Video, qint64 size = 1024 * 1024, int seeders = 10);
    // Poll get() until the RT row is visible (RT attributes settle asynchronously).
    bool waitForTorrent(const QString& hash, int maxRetries = 50, int delayMs = 20);
    // Same, for the files row. The two tables settle independently, so a
    // torrent being visible says nothing about its file list yet.
    bool waitForFiles(const QString& hash, int expected, int maxRetries = 50, int delayMs = 20);

    QTemporaryDir* tempDir_ = nullptr;
    Manticore* manticore_ = nullptr;
    Database* db_ = nullptr;
    TorrentRepository* repo_ = nullptr;
    rats::service::FilterPolicy* filter_ = nullptr;
    rats::service::IndexingService* indexing_ = nullptr;
};

Torrent TestManticoreQueries::makeTorrent(int n, const QString& name, ContentType type, qint64 size, int seeders)
{
    Torrent t;
    // 40-char hex hash: an 8-hex prefix from n, padded with 'a'.
    t.hash = QString("%1").arg(n, 8, 16, QChar('0')) + QString(32, 'a');
    t.name = name;
    t.size = size;
    t.files = 1;
    t.pieceLength = 262144;
    t.added = QDateTime::currentDateTime();
    t.ipv4 = "192.168.1.1";
    t.port = 6881;
    t.contentType = type;
    t.contentCategory = ContentCategory::Unknown;
    t.seeders = seeders;
    t.leechers = 5;
    t.completed = 100;
    return t;
}

bool TestManticoreQueries::waitForTorrent(const QString& hash, int maxRetries, int delayMs)
{
    for (int i = 0; i < maxRetries; ++i) {
        if (repo_->exists(hash))
            return true;
        QThread::msleep(delayMs);
    }
    return repo_->exists(hash);
}

bool TestManticoreQueries::waitForFiles(const QString& hash, int expected, int maxRetries, int delayMs)
{
    // exists() only asks the torrents table. A file list is a row in `files`,
    // written by a separate statement and made visible by the RT index on its own
    // schedule, so waiting for the torrent says nothing about its files yet.
    for (int i = 0; i < maxRetries; ++i) {
        if (repo_->filesOf(hash).size() == expected)
            return true;
        QThread::msleep(delayMs);
    }
    return repo_->filesOf(hash).size() == expected;
}

void TestManticoreQueries::initTestCase()
{
    tempDir_ = new QTemporaryDir();
    QVERIFY2(tempDir_->isValid(), "Failed to create temporary directory");
    qInfo() << "Test data directory:" << tempDir_->path();

    manticore_ = new Manticore(tempDir_->path());
    const bool started = manticore_->start();
    QVERIFY2(started, "Failed to start Manticore Search. Is searchd available?");
    QVERIFY(manticore_->isRunning());
    qInfo() << "Manticore started on port" << manticore_->port();

    db_ = new Database(manticore_);
    QVERIFY(db_->isConnected());

    repo_ = new TorrentRepository(db_);
    repo_->primeFromDatabase();

    filter_ = new rats::service::FilterPolicy();
    indexing_ = new rats::service::IndexingService(repo_, filter_);
}

void TestManticoreQueries::cleanupTestCase()
{
    delete indexing_;
    indexing_ = nullptr;
    delete filter_;
    filter_ = nullptr;
    delete repo_;
    repo_ = nullptr;
    delete db_;
    db_ = nullptr;
    if (manticore_) {
        manticore_->stop();
        delete manticore_;
        manticore_ = nullptr;
    }
    delete tempDir_;
    tempDir_ = nullptr;
}

void TestManticoreQueries::testConnected()
{
    QVERIFY(db_->isConnected());
    QVERIFY(manticore_->isRunning());
}

void TestManticoreQueries::testAddAndGet()
{
    const Torrent t = makeTorrent(1, "Ubuntu Linux ISO", ContentType::Archive, 4LL * 1024 * 1024 * 1024, 42);
    QVERIFY(repo_->add(t));
    QVERIFY(waitForTorrent(t.hash));

    const auto got = repo_->get(t.hash);
    QVERIFY(got.has_value());
    QCOMPARE(got->hash, t.hash);
    QCOMPARE(got->name, QString("Ubuntu Linux ISO"));
    QCOMPARE(got->size, (qint64)(4LL * 1024 * 1024 * 1024));
    QCOMPARE(got->seeders, 42);
    QCOMPARE(rats::domain::toId(got->contentType), rats::domain::toId(ContentType::Archive));
}

void TestManticoreQueries::testExists()
{
    QVERIFY(repo_->exists(makeTorrent(1, "x").hash)); // added in testAddAndGet
    QVERIFY(!repo_->exists(QString(40, 'f'))); // never inserted
}

void TestManticoreQueries::testGetWithFiles()
{
    Torrent t = makeTorrent(2, "Multi File Pack", ContentType::Video, 2000, 5);
    t.files = 2;
    t.fileList.append(File { "folder/movie.mkv", 1900 });
    t.fileList.append(File { "folder/readme.txt", 100 });
    QVERIFY(repo_->add(t));
    QVERIFY(waitForTorrent(t.hash));
    QVERIFY(waitForFiles(t.hash, 2));

    const auto got = repo_->get(t.hash, /*includeFiles=*/true);
    QVERIFY(got.has_value());
    QCOMPARE(got->fileList.size(), 2);
    QCOMPARE(got->fileList[0].path, QString("folder/movie.mkv"));
    QCOMPARE(got->fileList[0].size, (qint64)1900);
}

void TestManticoreQueries::testSearchByName()
{
    QVERIFY(repo_->add(makeTorrent(3, "Debian Server Edition", ContentType::Archive)));
    QVERIFY(waitForTorrent(makeTorrent(3, "x").hash));

    TorrentRepository::SearchQuery q;
    q.text = "Debian";
    q.limit = 10;
    const auto hits = repo_->searchTorrents(q);
    bool found = false;
    for (const auto& h : hits) {
        if (h.torrent.name.contains("Debian"))
            found = true;
    }
    QVERIFY2(found, "Full-text search should find the 'Debian' torrent");
}

void TestManticoreQueries::testSearchByHash()
{
    const Torrent t = makeTorrent(1, "x"); // Ubuntu torrent's hash
    TorrentRepository::SearchQuery q;
    q.text = t.hash; // a 40-hex string is treated as an exact hash lookup
    const auto hits = repo_->searchTorrents(q);
    QCOMPARE(hits.size(), 1);
    QCOMPARE(hits.first().torrent.hash, t.hash);
}

void TestManticoreQueries::testUpdateTrackerCounts()
{
    const Torrent t = makeTorrent(1, "x");
    QSignalSpy updatedSpy(repo_, &TorrentRepository::torrentUpdated);

    QVERIFY(repo_->updateTrackerCounts(t.hash, 111, 22, 3));
    QCOMPARE(updatedSpy.count(), 1);
    QCOMPARE(updatedSpy.takeFirst().at(0).toString(), t.hash);

    const auto got = repo_->get(t.hash);
    QVERIFY(got.has_value());
    QCOMPARE(got->seeders, 111);
    QCOMPARE(got->leechers, 22);
    QCOMPARE(got->completed, 3);
}

void TestManticoreQueries::testMergeInfoSignals()
{
    const Torrent t = makeTorrent(1, "x");
    QSignalSpy updatedSpy(repo_, &TorrentRepository::torrentUpdated);

    const QJsonObject info {
        { QStringLiteral("description"), QStringLiteral("fallback metadata") },
        { QStringLiteral("metadataSource"), QStringLiteral("test") },
    };
    QVERIFY(repo_->mergeInfo(t.hash, info));
    QCOMPARE(updatedSpy.count(), 1);
    QCOMPARE(updatedSpy.takeFirst().at(0).toString(), t.hash);

    const auto got = repo_->get(t.hash);
    QVERIFY(got.has_value());
    QCOMPARE(got->info.value(QStringLiteral("description")).toString(), QStringLiteral("fallback metadata"));
    QCOMPARE(got->info.value(QStringLiteral("metadataSource")).toString(), QStringLiteral("test"));
}


void TestManticoreQueries::testVerifiedSourceReplacesStaleInfo()
{
    Torrent original = makeTorrent(770010, "Багровый");
    original.info = QJsonObject {
        { QStringLiteral("description"), QStringLiteral("old guessed description") },
        { QStringLiteral("rutrackerThreadId"), 999999 },
        { QStringLiteral("metadataSource"), QStringLiteral("legacy fallback") },
    };

    const auto first = indexing_->insert(original);
    QVERIFY2(first.success, qPrintable(first.error));
    QVERIFY(waitForTorrent(original.hash));

    Torrent verified = original;
    verified.name = QStringLiteral(
        "Багровый прилив / Crimson Tide (1995) BDRip 1080p");
    verified.info = QJsonObject {
        { QStringLiteral("sourceProvider"), QStringLiteral("rutracker-ru") },
        { QStringLiteral("sourceTopicId"), 471557 },
        { QStringLiteral("sourceUrl"), QStringLiteral("https://rutracker.org/forum/viewtopic.php?t=471557") },
        { QStringLiteral("sourceVerified"), true },
        { QStringLiteral("strictComplete"), true },
        { QStringLiteral("description"), QStringLiteral("exact Rutor release description") },
        { QStringLiteral("quality"), QStringLiteral("BDRip-AVC 1080p") },
        { QStringLiteral("video"), QStringLiteral("AVC / H.264 1920x1080") },
        { QStringLiteral("audioTracks"), QJsonArray {
            QStringLiteral("Аудио #1: Russian AC3 5.1")
        } },
    };

    const auto merged = indexing_->insert(verified);
    QVERIFY2(merged.success, qPrintable(merged.error));
    QVERIFY(merged.alreadyExists);

    const auto stored = repo_->get(original.hash);
    QVERIFY(stored.has_value());
    QVERIFY(stored->info.value(QStringLiteral("sourceVerified")).toBool());
    QCOMPARE(stored->info.value(QStringLiteral("sourceUrl")).toString(),
        QStringLiteral("https://rutracker.org/forum/viewtopic.php?t=471557"));
    QCOMPARE(stored->name,
        QStringLiteral("Багровый прилив / Crimson Tide (1995) BDRip 1080p"));
    QCOMPARE(stored->info.value(QStringLiteral("description")).toString(),
        QStringLiteral("exact Rutor release description"));
    QVERIFY2(!stored->info.contains(QStringLiteral("rutrackerThreadId")),
        "stale post-hoc source link must not survive an exact verified source");
}

void TestManticoreQueries::testStatistics()
{
    const auto stats = repo_->statistics();
    // At least the three torrents added above.
    QVERIFY2(stats.torrents >= 3, qPrintable(QString("torrents=%1").arg(stats.torrents)));
    QVERIFY(stats.totalSize > 0);
}

void TestManticoreQueries::testRecent()
{
    const auto recent = repo_->recent(10);
    QVERIFY(!recent.isEmpty());
}

void TestManticoreQueries::testTop()
{
    // Ubuntu torrent now has 111 seeders; it should appear in the top list.
    const auto top = repo_->top(QString(), QString(), 0, 10);
    QVERIFY(!top.isEmpty());
    // Ordered by seeders descending.
    for (int i = 1; i < top.size(); ++i)
        QVERIFY(top[i - 1].seeders >= top[i].seeders);
}

void TestManticoreQueries::testRemove()
{
    const Torrent t = makeTorrent(3, "x"); // Debian torrent
    QVERIFY(repo_->exists(t.hash));
    QVERIFY(repo_->remove(t.hash));
    // Removal should eventually be visible.
    bool gone = false;
    for (int i = 0; i < 50; ++i) {
        if (!repo_->exists(t.hash)) {
            gone = true;
            break;
        }
        QThread::msleep(20);
    }
    QVERIFY2(gone, "Torrent should be removed");
}

void TestManticoreQueries::testSelectQueryAgainstLiveIndex()
{
    // Exercise the SelectQuery builder end-to-end against the live index.
    const QString sql = SelectQuery("torrents")
                            .columns("id, hash, name")
                            .whereRaw("seeders > 0")
                            .orderBy("seeders", true)
                            .limit(0, 5)
                            .build();
    bool ok = false;
    const auto rows = db_->query(sql, {}, &ok);
    QVERIFY(ok);
    // The Ubuntu torrent (111 seeders) guarantees at least one row.
    QVERIFY(!rows.isEmpty());
}

// A full-index sweep (the `torrent.cleanup` maintenance path) must be able to
// walk past Manticore's max_matches, which rejects any OFFSET >= 1000 with
// "offset out of bounds" — the reason the filter cleanup used to stop after the
// first 1000 torrents.
void TestManticoreQueries::testPageAfterIdWalksPastMaxMatches()
{
    constexpr int kExtra = 1200;
    const int base = 10000;
    for (int i = 0; i < kExtra; ++i)
        QVERIFY(repo_->add(makeTorrent(base + i, QString("Sweep Sample %1").arg(i))));
    QVERIFY(waitForTorrent(makeTorrent(base + kExtra - 1, "x").hash));

    // Plain OFFSET paging is what breaks: prove the limit is real, so this test
    // fails loudly if the sweep ever goes back to offsets.
    bool ok = false;
    db_->query(SelectQuery("torrents").orderBy("id", false).limit(1000, 500).build(), {}, &ok);
    QVERIFY2(!ok, "Manticore should reject an offset of 1000 (max_matches)");

    int seen = 0;
    qint64 afterId = 0;
    qint64 previousId = 0;
    for (;;) {
        const QVector<Torrent> batch = repo_->pageAfterId(afterId, 500);
        if (batch.isEmpty())
            break;
        for (const Torrent& t : batch) {
            QVERIFY(t.id > previousId); // strictly ascending, no repeats
            previousId = t.id;
            ++seen;
        }
        afterId = batch.last().id;
    }
    QVERIFY2(seen >= kExtra, qPrintable(QString("swept %1 of >= %2 torrents").arg(seen).arg(kExtra)));
    QCOMPARE(static_cast<qint64>(seen), repo_->statistics().torrents);
}

// ---------------------------------------------------------------------------
// Row ids derived from the infohash
// ---------------------------------------------------------------------------

void TestManticoreQueries::testRowIdIsDerivedFromHash()
{
    const Torrent t = makeTorrent(770001, "Derived Id Sample");
    QVERIFY(repo_->add(t));
    QVERIFY(waitForTorrent(t.hash));

    const auto stored = repo_->get(t.hash);
    QVERIFY(stored.has_value());
    // The row id is not a counter value: it is the hash, and a peer computing it
    // from the same hash must arrive at the same number.
    QCOMPARE(stored->id, rats::data::rowIdFromHash(t.hash));
}

void TestManticoreQueries::testFilesRowSharesTheTorrentId()
{
    Torrent t = makeTorrent(770002, "Shared Id Sample");
    t.fileList = { File { "shared/one.mkv", 111 }, File { "shared/two.mkv", 222 } };
    t.files = t.fileList.size();
    QVERIFY(repo_->add(t));
    QVERIFY(waitForTorrent(t.hash));
    QVERIFY(waitForFiles(t.hash, 2));

    // The files row carrying the torrent's own id is what turns the export join
    // into a docid lookup, so assert the id itself rather than just the contents.
    const qint64 expected = rats::data::rowIdFromHash(t.hash);
    const auto rows = db_->query(QStringLiteral("SELECT id FROM files WHERE id = %1").arg(expected));
    QCOMPARE(rows.size(), 1);

    const QVector<File> files = repo_->filesOf(t.hash);
    QCOMPARE(files.size(), 2);
    QCOMPARE(files.at(0).path, QString("shared/one.mkv"));
}

void TestManticoreQueries::testReAddDoesNotDuplicateRow()
{
    // With a derived id, adding the same torrent twice targets one row, and the
    // second add leaves the stored copy alone. Under the old counter the same
    // sequence produced two rows carrying one hash.
    Torrent t = makeTorrent(770003, "Duplicate Guard");
    QVERIFY(repo_->add(t));
    QVERIFY(waitForTorrent(t.hash));

    t.name = "Duplicate Guard Reindexed";
    QVERIFY(repo_->add(t));

    const auto rows
        = db_->query(QStringLiteral("SELECT id FROM torrents WHERE id = %1").arg(rats::data::rowIdFromHash(t.hash)));
    QCOMPARE(rows.size(), 1);
    QCOMPARE(repo_->get(t.hash)->name, QString("Duplicate Guard"));
}

void TestManticoreQueries::testRowIdCollisionIsRefused()
{
    // Two hashes that agree on their first 64 bits and differ afterwards map to
    // one row. The second one must be refused, never stored over the first.
    const QString shared = QStringLiteral("beefbeefbeefbeef");
    Torrent first = makeTorrent(1, "Collision First");
    first.hash = shared + QString(24, QLatin1Char('1'));
    Torrent second = makeTorrent(2, "Collision Second");
    second.hash = shared + QString(24, QLatin1Char('2'));
    QCOMPARE(rats::data::rowIdFromHash(first.hash), rats::data::rowIdFromHash(second.hash));

    QVERIFY(repo_->add(first));
    QVERIFY(waitForTorrent(first.hash));

    QVERIFY2(!repo_->add(second), "a torrent whose row id belongs to another must be refused");

    // The original survives untouched, and the impostor is not findable.
    const auto stored = repo_->get(first.hash);
    QVERIFY(stored.has_value());
    QCOMPARE(stored->name, QString("Collision First"));
    QVERIFY(!repo_->get(second.hash).has_value());
    QVERIFY(!repo_->exists(second.hash));

    // Batch lookups must reach the same verdict, and report which hash lost.
    QSet<QString> collided;
    const auto found = repo_->getMany({ first.hash, second.hash }, &collided);
    QVERIFY(found.contains(first.hash));
    QVERIFY(!found.contains(second.hash));
    QVERIFY(collided.contains(second.hash));
}

void TestManticoreQueries::testBatchRowIdCollisionIsRefused()
{
    // The collision add() catches needs a stored row to compare against. Two
    // torrents that are both *new* to the index have none, so a batch write is the
    // one path where nothing upstream can see the clash: getMany() finds neither,
    // reports neither as collided, and both arrive here as "known absent".
    const QString shared = QStringLiteral("cafecafecafecafe");
    Torrent first = makeTorrent(1, "Batch Collision First");
    first.hash = shared + QString(24, QLatin1Char('a'));
    Torrent second = makeTorrent(2, "Batch Collision Second");
    second.hash = shared + QString(24, QLatin1Char('b'));
    QCOMPARE(rats::data::rowIdFromHash(first.hash), rats::data::rowIdFromHash(second.hash));
    QVERIFY(!repo_->exists(first.hash));
    QVERIFY(!repo_->exists(second.hash));

    const auto before = repo_->statistics();
    // One row written, not two: the count is the contract the statistics ride on.
    QCOMPARE(repo_->addMany({ first, second }), 1);
    QVERIFY(waitForTorrent(first.hash));

    // The first one owns the row, whole and unmixed, and the loser stayed out.
    const auto stored = repo_->get(first.hash);
    QVERIFY(stored.has_value());
    QCOMPARE(stored->name, QString("Batch Collision First"));
    QVERIFY(!repo_->exists(second.hash));

    // The statistics must describe the row that exists, not the rows handed in --
    // an over-count here never heals, since nothing recomputes it until a restart.
    QCOMPARE(repo_->statistics().torrents, before.torrents + 1);
    QCOMPARE(repo_->statistics().files, before.files + first.files);
    QCOMPARE(repo_->statistics().totalSize, before.totalSize + first.size);

    // Re-priming from the index must agree with the counters kept in memory.
    const auto inMemory = repo_->statistics();
    repo_->primeFromDatabase();
    QCOMPARE(repo_->statistics().torrents, inMemory.torrents);
    QCOMPARE(repo_->statistics().totalSize, inMemory.totalSize);
}

// The same hash twice is not a collision: it is one torrent, and collapsing it is
// ordinary dedupe rather than a refusal. Dump batches repeat hashes routinely.
void TestManticoreQueries::testBatchDuplicateHashCollapsesToOneRow()
{
    Torrent t = makeTorrent(1, "Batch Duplicate");
    const auto before = repo_->statistics();

    QCOMPARE(repo_->addMany({ t, t }), 1);
    QVERIFY(waitForTorrent(t.hash));

    QCOMPARE(repo_->statistics().torrents, before.torrents + 1);
    const auto stored = repo_->get(t.hash);
    QVERIFY(stored.has_value());
    QCOMPARE(stored->name, QString("Batch Duplicate"));
}

void TestManticoreQueries::testMigrationReKeysLegacyRows()
{
    // Write rows the way the pre-migration code did: a small sequential id that
    // has nothing to do with the hash. The repository can no longer produce these,
    // so they go in through the raw database layer.
    struct Legacy {
        qint64 id;
        QString hash;
        QString name;
    };
    const QVector<Legacy> legacy {
        { 41, QStringLiteral("11110000") + QString(32, QLatin1Char('d')), QStringLiteral("Legacy One") },
        { 42, QStringLiteral("22220000") + QString(32, QLatin1Char('d')), QStringLiteral("Legacy Two") },
    };

    for (const Legacy& row : legacy) {
        QVERIFY(db_->insert("torrents",
            QVariantMap { { "id", row.id }, { "hash", row.hash }, { "name", row.name }, { "nameIndex", row.name },
                { "size", 4096 }, { "files", 1 }, { "piecelength", 262144 },
                { "added", QDateTime::currentSecsSinceEpoch() }, { "ipv4", "10.0.0.1" }, { "port", 6881 },
                { "contentType", 1 }, { "contentCategory", 0 }, { "seeders", 1 }, { "leechers", 1 }, { "completed", 1 },
                { "trackersChecked", 0 }, { "good", 0 }, { "bad", 0 } }));
        QVERIFY(db_->insert("files",
            QVariantMap { { "id", row.id }, { "hash", row.hash }, { "path", "legacy/file.bin" }, { "size", "4096" } }));
    }

    // A legacy row is invisible to a lookup precisely because its id is wrong —
    // that is what the migration exists to fix.
    QVERIFY(!repo_->exists(legacy.first().hash));

    for (const QString& table : { QStringLiteral("torrents"), QStringLiteral("files") }) {
        qint64 cursor = 0;
        for (;;) {
            const auto page = repo_->migrateRowIdPage(table, cursor, 500);
            // A failed page leaves the cursor where it was, so looping on
            // `finished` alone would spin forever.
            QVERIFY2(!page.failed, qPrintable("migration page failed on " + table));
            if (page.finished)
                break;
        }
    }

    for (const Legacy& row : legacy) {
        QVERIFY2(waitForTorrent(row.hash), qPrintable("not reachable after migration: " + row.name));
        QVERIFY2(waitForFiles(row.hash, 1), qPrintable("files not reachable after migration: " + row.name));
        const auto stored = repo_->get(row.hash, /*includeFiles*/ true);
        QVERIFY(stored.has_value());
        QCOMPARE(stored->name, row.name);
        // Re-keyed, and the full-text column survived the rewrite.
        QCOMPARE(stored->id, rats::data::rowIdFromHash(row.hash));
        QCOMPARE(stored->fileList.size(), 1);
        QCOMPARE(stored->fileList.at(0).path, QString("legacy/file.bin"));
        // The old row is gone rather than left behind as a duplicate.
        QVERIFY(db_->query(QStringLiteral("SELECT id FROM torrents WHERE id = %1").arg(row.id)).isEmpty());
    }

    TorrentRepository::SearchQuery q;
    q.text = "Legacy One";
    q.limit = 10;
    QVERIFY2(!repo_->searchTorrents(q).isEmpty(), "re-keyed row must still be findable by name");

    // Running it again must be a no-op, not a second rewrite.
    qint64 cursor = 0;
    int rewritten = 0;
    for (;;) {
        const auto page = repo_->migrateRowIdPage(QStringLiteral("torrents"), cursor, 500);
        QVERIFY(!page.failed);
        if (page.finished)
            break;
        rewritten += page.rewritten;
    }
    QCOMPARE(rewritten, 0);
}

void TestManticoreQueries::testUpdateAppliesAndKeepsSearchable()
{
    // update() used to be a partial UPDATE naming nameIndex, which Manticore
    // rejects outright as a full-text field -- so nothing was written and vote
    // merges vanished. Pin that it now actually applies.
    Torrent t = makeTorrent(770004, "Updatable Sample zephyr");
    t.good = 1;
    QVERIFY(repo_->add(t));
    QVERIFY(waitForTorrent(t.hash));

    Torrent changed = *repo_->get(t.hash);
    changed.good = 42;
    changed.bad = 7;
    changed.seeders = 999;
    QVERIFY(repo_->update(changed));

    const auto stored = repo_->get(t.hash);
    QVERIFY(stored.has_value());
    QCOMPARE(stored->good, 42);
    QCOMPARE(stored->bad, 7);
    QCOMPARE(stored->seeders, 999);
    // Rewriting the row must not cost it its place in the full-text index, and
    // must not leave a second row behind.
    QCOMPARE(stored->id, rats::data::rowIdFromHash(t.hash));
    TorrentRepository::SearchQuery q;
    q.text = "zephyr";
    q.limit = 10;
    QCOMPARE(repo_->searchTorrents(q).size(), 1);
}

// ============================================================================
// Whole-database export / import
// ============================================================================
//
// These drive the real DatabaseExporter and DatabaseImporter against a live
// index, which is the only place the two halves meet: the exporter's keyset
// sweep, its file-list join and its background serialiser on one side, the
// importer's batched merge through IndexingService on the other.

void TestManticoreQueries::testExportImportRoundTrip()
{
    // A distinct hash space, so the rows the other tests left behind cannot make
    // this one pass by accident.
    const int kCount = 1200; // several frames, and past max_matches
    QSet<QString> written;
    QVector<Torrent> batch;
    for (int i = 0; i < kCount; ++i) {
        Torrent t = makeTorrent(500000 + i, QString("roundtrip subject %1").arg(i));
        t.fileList = { File { QString("dir/file-%1.mkv").arg(i), 4096 + i } };
        t.files = 1;
        written.insert(t.hash);
        batch.append(t);
    }
    QCOMPARE(repo_->addMany(batch), kCount);
    QVERIFY(waitForTorrent(batch.last().hash));
    QVERIFY(waitForFiles(batch.first().hash, 1));

    const QString path = QDir(tempDir_->path()).absoluteFilePath("roundtrip.ratsdb");
    rats::service::DatabaseExporter exporter(repo_);
    rats::service::CancelToken noCancel;
    qint64 lastReported = 0;
    const auto result = exporter.run(
        path, rats::service::dump::Header {}, noCancel, [&](qint64 torrents, qint64) { lastReported = torrents; });

    QVERIFY2(result.ok, qPrintable(result.error));
    QVERIFY(result.torrents >= kCount); // plus whatever earlier tests left in the index
    QCOMPARE(lastReported, result.torrents);
    QVERIFY(result.bytes > 0);
    QCOMPARE(QFileInfo(path).size(), result.bytes);

    // Read it straight back and confirm every hash we wrote came out again, with
    // its file list intact — the join is a separate query from the page sweep, and
    // losing it would be silent.
    rats::service::DumpReader reader;
    QString error;
    QVERIFY2(reader.open(path, &error), qPrintable(error));
    QSet<QString> seen;
    int withFiles = 0;
    QVector<Torrent> frame;
    while (reader.readBatch(frame, &error)) {
        for (const Torrent& t : frame) {
            if (!written.contains(t.hash))
                continue;
            seen.insert(t.hash);
            if (t.fileList.size() == 1)
                ++withFiles;
        }
    }
    QVERIFY(reader.atEnd());
    QVERIFY2(reader.complete(), "a finished export must carry its end marker and footer");
    QCOMPARE(seen.size(), kCount);
    QCOMPARE(withFiles, kCount);
}

void TestManticoreQueries::testImportIsAMergeNotADuplicate()
{
    const int kCount = 40;
    QVector<Torrent> batch;
    for (int i = 0; i < kCount; ++i) {
        Torrent t = makeTorrent(600000 + i, QString("merge subject %1").arg(i));
        t.fileList = { File { QString("m/%1.bin").arg(i), 100 + i } };
        t.files = 1;
        batch.append(t);
    }
    QCOMPARE(repo_->addMany(batch), kCount);
    QVERIFY(waitForTorrent(batch.last().hash));

    const QString path = QDir(tempDir_->path()).absoluteFilePath("merge.ratsdb");
    rats::service::DatabaseExporter exporter(repo_);
    rats::service::CancelToken noCancel;
    QVERIFY(exporter.run(path, rats::service::dump::Header {}, noCancel, {}).ok);

    const qint64 before = repo_->statistics().torrents;

    // Importing our own dump must add nothing: every row is already ours. This is
    // the invariant that makes a dump merge idempotent, and it only holds because
    // a row id is derived from the infohash rather than handed out by a counter.
    rats::service::DatabaseImporter importer(indexing_);
    rats::service::DatabaseImporter::Options options;
    options.applyFilters = false;
    const auto result = importer.run(path, options, noCancel, {});

    QVERIFY2(result.ok, qPrintable(result.error));
    QVERIFY(!result.truncated);
    QCOMPARE(result.inserted, 0LL);
    QVERIFY(result.merged >= kCount);
    QCOMPARE(repo_->statistics().torrents, before);
}

void TestManticoreQueries::testCancelledExportLeavesNoFile()
{
    const QString path = QDir(tempDir_->path()).absoluteFilePath("cancelled.ratsdb");
    QFile::remove(path);

    rats::service::CancelToken cancel;
    cancel.cancel(); // already raised: the sweep must stop before writing anything

    rats::service::DatabaseExporter exporter(repo_);
    const auto result = exporter.run(path, rats::service::dump::Header {}, cancel, {});

    QVERIFY(!result.ok);
    // Cancellation is not a failure, so it reports no error — and it must not
    // leave a file that looks like a dump but has no footer.
    QVERIFY(result.error.isEmpty());
    QVERIFY(!QFile::exists(path));
}

void TestManticoreQueries::testInterruptedImportResumesWhereItStopped()
{
    const int kCount = 1500; // at least three frames
    QVector<Torrent> batch;
    for (int i = 0; i < kCount; ++i)
        batch.append(makeTorrent(700000 + i, QString("resume subject %1").arg(i)));
    QCOMPARE(repo_->addMany(batch), kCount);
    QVERIFY(waitForTorrent(batch.last().hash));

    const QString path = QDir(tempDir_->path()).absoluteFilePath("resume.ratsdb");
    rats::service::DatabaseExporter exporter(repo_);
    rats::service::CancelToken noCancel;
    const auto exported = exporter.run(path, rats::service::dump::Header {}, noCancel, {});
    QVERIFY(exported.ok);

    // Stop after the first committed frame and keep the offset it reported.
    rats::service::CancelToken cancel;
    qint64 resumeOffset = 0;
    qint64 firstPass = 0;
    rats::service::DatabaseImporter importer(indexing_);
    rats::service::DatabaseImporter::Options options;
    options.applyFilters = false;
    const auto partial
        = importer.run(path, options, cancel, [&](const rats::service::DatabaseImporter::Result& progress) {
              resumeOffset = progress.offset;
              firstPass = progress.processed;
              cancel.cancel();
          });

    QVERIFY(partial.cancelled);
    QVERIFY(!partial.ok);
    QVERIFY(firstPass > 0);
    QVERIFY(resumeOffset > 0);
    QVERIFY(resumeOffset < QFileInfo(path).size());

    // Resuming reads only what is left. The offset is recorded after the batch it
    // follows is committed, so a resume may repeat work but must never skip it —
    // the two passes together have to cover the whole dump.
    options.startOffset = resumeOffset;
    const auto rest = importer.run(path, options, noCancel, {});
    QVERIFY2(rest.ok, qPrintable(rest.error));
    QVERIFY(rest.processed > 0);
    QVERIFY(rest.processed < exported.torrents);
    QVERIFY(firstPass + rest.processed >= exported.torrents);
}

QTEST_MAIN(TestManticoreQueries)
#include "test_manticore_queries.moc"
