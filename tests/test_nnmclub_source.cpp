#include <QtTest>
#include <QJsonArray>

#include "domain/content.h"
#include "net/nnmclub_source.h"
#include "net/source_parse_utils.h"

using rats::domain::Torrent;
using rats::net::NnmClubSource;

class TestNnmClubSource : public QObject {
    Q_OBJECT
private slots:
    void buildsPublicSearchBody();
    void parsesExactSearchRow();
    void verifiesExactPublicTopicAndRichVideo();
    void rejectsWrongDownloadId();
    void typedSearchHintsPrioritizeBooksSafely();
};

void TestNnmClubSource::buildsPublicSearchBody()
{
    const QByteArray body = NnmClubSource::searchBody(
        QStringLiteral("Тест"), QStringLiteral("seeders_desc"));
    QVERIFY(body.contains("f%5B%5D=-1"));
    QVERIFY(body.contains("o=10"));
    QVERIFY(body.contains("s=2"));
    QVERIFY(body.contains("sds=4"));
    // Windows-1251 bytes: Т=e2? uppercase Т is D2, е E5, с F1, т F2.
    QVERIFY(body.contains("nm=%D2%E5%F1%F2"));
}

void TestNnmClubSource::parsesExactSearchRow()
{
    const QByteArray html = R"(
      <table class="forumline tablesorter"><tbody>
       <tr>
        <td><a href="tracker.php?f=92">Архив Музыки</a></td>
        <td><a href="viewtopic.php?t=850460"><b>Artist - Album [FLAC]</b></a></td>
        <td>x</td><td>x</td><td>x</td>
        <td><u>284164096</u> 271 MB</td>
        <td class="seedmed"><b>23</b></td>
        <td class="leechmed"><b>2</b></td>
        <td>100</td><td><u>1790500000</u></td>
        <td><a href="download.php?id=123456">torrent</a></td>
       </tr>
      </tbody></table>)";

    const QVector<Torrent> rows = NnmClubSource::parseSearchPage(
        html, QUrl(QStringLiteral("https://nnmclub.to/forum/tracker.php")));
    QCOMPARE(rows.size(), 1);
    const Torrent& t = rows.first();
    QCOMPARE(t.name, QStringLiteral("Artist - Album [FLAC]"));
    QCOMPARE(t.seeders, 23);
    QCOMPARE(t.leechers, 2);
    QCOMPARE(t.size, 284164096LL);
    QCOMPARE(t.info.value(QStringLiteral("sourceTopicId")).toInt(), 850460);
    QCOMPARE(t.info.value(QStringLiteral("sourceDownloadId")).toInt(), 123456);
    QCOMPARE(rats::domain::toId(t.contentType),
        rats::domain::toId(rats::domain::ContentType::Audio));
    QCOMPARE(t.info.value(QStringLiteral("contentTypeEvidence")).toString(),
        QStringLiteral("source-category"));
}

void TestNnmClubSource::verifiesExactPublicTopicAndRichVideo()
{
    Torrent t;
    t.hash = QStringLiteral("0123456789abcdef0123456789abcdef01234567");
    t.name = QStringLiteral("Example Film (2025) BDRip 1080p");
    t.contentType = rats::domain::ContentType::Video;
    t.info[QStringLiteral("sourceProvider")] = QStringLiteral("nnmclub");
    t.info[QStringLiteral("sourceTopicId")] = 777;
    t.info[QStringLiteral("sourceDownloadId")] = 888;
    t.info[QStringLiteral("sourceUrl")]
        = QStringLiteral("https://nnmclub.to/forum/viewtopic.php?t=777");
    t.info[QStringLiteral("sourceTorrentUrl")]
        = QStringLiteral("https://nnmclub.to/forum/download.php?id=888");
    t.info[QStringLiteral("contentTypeEvidence")]
        = QStringLiteral("source-category");

    const QByteArray html = R"(
      <html><body>
       <a href="download.php?id=888">torrent</a>
       <div class="postbody">
        Название: Example Film<br>
        Год: 2025<br>
        Качество: BDRip 1080p<br>
        Видео: AVC, 1920x1080, 12.0 Mbps<br>
        Аудио #1: Russian, AC3, 5.1, 640 kbps<br>
        Аудио #2: English, DTS, 5.1, 1509 kbps<br>
        Субтитры: Russian, English<br>
        Описание: This exact NNM-Club release contains detailed edition,
        translation, source and technical information. The description is long
        enough to ensure the strict result is a concrete release card rather
        than only a title and magnet.
       </div><!--/postbody-->
      </body></html>)";

    QVERIFY(NnmClubSource::applyDetailPage(
        t, html, QUrl(QStringLiteral(
            "https://nnmclub.to/forum/viewtopic.php?t=777"))));
    QVERIFY(t.info.value(QStringLiteral("sourceVerified")).toBool());
    QCOMPARE(t.info.value(QStringLiteral("quality")).toString(),
        QStringLiteral("BDRip 1080p"));
    QCOMPARE(t.info.value(QStringLiteral("audioTracks")).toArray().size(), 2);
    QVERIFY(NnmClubSource::isStrictComplete(t));
}

void TestNnmClubSource::rejectsWrongDownloadId()
{
    Torrent t;
    t.hash = QStringLiteral("0123456789abcdef0123456789abcdef01234567");
    t.name = QStringLiteral("Release");
    t.contentType = rats::domain::ContentType::Audio;
    t.info[QStringLiteral("sourceProvider")] = QStringLiteral("nnmclub");
    t.info[QStringLiteral("sourceTopicId")] = 777;
    t.info[QStringLiteral("sourceDownloadId")] = 888;
    t.info[QStringLiteral("sourceUrl")]
        = QStringLiteral("https://nnmclub.to/forum/viewtopic.php?t=777");

    const QByteArray html = R"(
      <a href="download.php?id=999">wrong</a>
      <div class="postbody">Описание: wrong exact-looking page</div>)";
    QVERIFY(!NnmClubSource::applyDetailPage(
        t, html, QUrl(QStringLiteral(
            "https://nnmclub.to/forum/viewtopic.php?t=777"))));
}


void TestNnmClubSource::typedSearchHintsPrioritizeBooksSafely()
{
    Torrent book;
    book.name = QStringLiteral(
        "History of Fortifications [PDF, FB2, EPUB]");
    QCOMPARE(rats::net::sourceparse::contentTypeHintScore(
        book, QStringLiteral("books")), 500);

    Torrent explicitVideo;
    explicitVideo.name = QStringLiteral("Manual PDF");
    explicitVideo.contentType = rats::domain::ContentType::Video;
    explicitVideo.info[QStringLiteral("contentTypeEvidence")]
        = QStringLiteral("source-category");
    QVERIFY(rats::net::sourceparse::contentTypeHintScore(
        explicitVideo, QStringLiteral("books")) < 0);
}

QTEST_MAIN(TestNnmClubSource)
#include "test_nnmclub_source.moc"
