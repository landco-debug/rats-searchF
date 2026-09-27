#include <QJsonArray>
#include <QtTest/QtTest>

#include "net/rutor_source.h"

using rats::domain::Torrent;
using rats::net::RutorSource;

class TestRutorSource : public QObject {
    Q_OBJECT

private slots:
    void buildsRutorSearchUrl();
    void searchRowCarriesExactProvenance();
    void detailPageVerifiesSameHashAndRichReleaseInfo();
    void mismatchedDetailHashIsRejected();
    void incompleteDetailPageIsNotStrict();
};

static const QString kHash
    = QStringLiteral("0123456789abcdef0123456789abcdef01234567");

void TestRutorSource::buildsRutorSearchUrl()
{
    const QUrl url = RutorSource::searchUrl(
        QStringLiteral("Police Academy 60 fps"), QStringLiteral("seeders_desc"));
    QCOMPARE(url.host(), QStringLiteral("rutor.info"));
    QCOMPARE(url.path(), QStringLiteral("/search/0/0/100/2/Police Academy 60 fps/"));
}

void TestRutorSource::searchRowCarriesExactProvenance()
{
    const QByteArray html = R"(
      <table><tr>
        <td>26 Сен 26</td>
        <td>
          <a class="downgif" href="/download/471557"></a>
          <a href="magnet:?xt=urn:btih:0123456789ABCDEF0123456789ABCDEF01234567">magnet</a>
          <a href="/torrent/471557/police-academy">Police Academy BDRip-AVC 60 fps</a>
        </td>
        <td>3.63 GB</td>
        <td><span class="green"><b>42</b></span></td>
        <td><span class=red><a href="/peers">3</a></span></td>
      </tr></table>)";

    const QVector<Torrent> torrents = RutorSource::parseSearchPage(
        html, QUrl(QStringLiteral("https://rutor.info/search/test")));
    QCOMPARE(torrents.size(), 1);

    const Torrent& t = torrents.first();
    QCOMPARE(t.hash, kHash);
    QCOMPARE(t.info.value(QStringLiteral("sourceProvider")).toString(),
        QStringLiteral("rutor"));
    QCOMPARE(t.info.value(QStringLiteral("sourceTopicId")).toInt(), 471557);
    QCOMPARE(t.info.value(QStringLiteral("sourceUrl")).toString(),
        QStringLiteral("https://rutor.info/torrent/471557/police-academy"));
    QCOMPARE(t.info.value(QStringLiteral("sourceTorrentUrl")).toString(),
        QStringLiteral("https://rutor.info/download/471557"));
    QVERIFY(!t.info.value(QStringLiteral("sourceVerified")).toBool());
    QCOMPARE(t.seeders, 42);
    QCOMPARE(t.leechers, 3);
    QVERIFY(t.size > 3LL * 1024 * 1024 * 1024);
}

void TestRutorSource::detailPageVerifiesSameHashAndRichReleaseInfo()
{
    Torrent t;
    t.hash = kHash;
    t.name = QStringLiteral("Police Academy BDRip-AVC 60 fps");
    t.info[QStringLiteral("sourceProvider")] = QStringLiteral("rutor");
    t.info[QStringLiteral("sourceTopicId")] = 471557;
    t.info[QStringLiteral("sourceUrl")]
        = QStringLiteral("https://rutor.info/torrent/471557/police-academy");

    const QByteArray html = R"(
      <html><body>
      <table id="details"><tbody>
        <tr><td>Качество</td><td>BDRip-AVC 1080p 60 fps</td></tr>
        <tr><td>Видео</td><td>AVC / H.264, 1920x1080, 59.940 fps, 14 Mbps</td></tr>
        <tr><td>Аудио #1</td><td>Russian AC3 5.1, 640 kbps</td></tr>
        <tr><td>Аудио #2</td><td>English DTS 5.1, 1509 kbps</td></tr>
        <tr><td>Субтитры</td><td>Russian, English</td></tr>
        <tr><td>Описание</td><td>
          Detailed information about this exact release, its source master,
          encoding parameters, frame rate, audio tracks and subtitles. This is
          deliberately long enough that a tiny generic synopsis cannot satisfy
          the strict completeness policy by accident.
        </td></tr>
      </tbody></table>
      <a href="magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567">magnet</a>
      </body></html>)";

    QVERIFY(RutorSource::applyDetailPage(
        t, html, QUrl(QStringLiteral("https://rutor.info/torrent/471557/police-academy"))));
    QVERIFY(t.info.value(QStringLiteral("sourceVerified")).toBool());
    QVERIFY(!t.info.value(QStringLiteral("quality")).toString().isEmpty());
    QVERIFY(!t.info.value(QStringLiteral("video")).toString().isEmpty());
    QCOMPARE(t.info.value(QStringLiteral("audioTracks")).toArray().size(), 2);
    QVERIFY(RutorSource::isStrictComplete(t));
}

void TestRutorSource::mismatchedDetailHashIsRejected()
{
    Torrent t;
    t.hash = kHash;
    t.name = QStringLiteral("Expected release");
    t.info[QStringLiteral("sourceProvider")] = QStringLiteral("rutor");
    t.info[QStringLiteral("sourceUrl")]
        = QStringLiteral("https://rutor.info/torrent/1/wrong");

    const QByteArray html
        = R"(<table id="details"><tr><td>Качество</td><td>1080p</td></tr></table>
             <a href="magnet:?xt=urn:btih:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa">magnet</a>)";

    QVERIFY(!RutorSource::applyDetailPage(
        t, html, QUrl(QStringLiteral("https://rutor.info/torrent/1/wrong"))));
    QVERIFY(!t.info.value(QStringLiteral("sourceVerified")).toBool());
}

void TestRutorSource::incompleteDetailPageIsNotStrict()
{
    Torrent t;
    t.hash = kHash;
    t.name = QStringLiteral("Some Movie 1080p");
    t.info[QStringLiteral("sourceProvider")] = QStringLiteral("rutor");
    t.info[QStringLiteral("sourceUrl")]
        = QStringLiteral("https://rutor.info/torrent/2/some-movie");

    const QByteArray html = R"(
      <table id="details"><tr><td>Описание</td><td>
        This page has an exact magnet but deliberately omits concrete video and
        audio track information, so it must never enter the strict result set.
        The description is long enough to prove that length alone is not enough.
      </td></tr></table>
      <a href="magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567">magnet</a>)";

    QVERIFY(RutorSource::applyDetailPage(
        t, html, QUrl(QStringLiteral("https://rutor.info/torrent/2/some-movie"))));
    QVERIFY(!RutorSource::isStrictComplete(t));
}

QTEST_MAIN(TestRutorSource)
#include "test_rutor_source.moc"
