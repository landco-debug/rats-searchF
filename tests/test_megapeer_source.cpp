#include <QtTest>
#include <QJsonArray>

#include "domain/content.h"
#include "net/megapeer_source.h"

using rats::domain::Torrent;
using rats::net::MegaPeerSource;

class TestMegaPeerSource : public QObject {
    Q_OBJECT
private slots:
    void buildsSearchUrl();
    void parsesExactSearchRow();
    void parsesIdOnlyUrls();
    void verifiesExactDownloadAndRichAudio();
    void rejectsWrongDownloadId();
};

void TestMegaPeerSource::buildsSearchUrl()
{
    const QUrl url = MegaPeerSource::searchUrl(
        QStringLiteral("тест"), QStringLiteral("size_asc"));
    QVERIFY(url.toEncoded().contains("sort=2"));
    QVERIFY(url.toEncoded().contains("ascdesc=1"));
    QVERIFY(url.toEncoded().contains("search="));
}

void TestMegaPeerSource::parsesExactSearchRow()
{
    const QByteArray html = R"(
      <table>
       <tr class="table_fon">
        <td>28 Мая 24</td>
        <td><a href="/torrent/77934/sicario"><b>Sicario (2015) BDRip-AVC</b></a></td>
        <td><a href="/download/77934/sicario.torrent">download</a></td>
        <td>1.50 GB</td>
        <td><font color="green">17</font> <font color="red">3</font></td>
       </tr>
      </table>)";
    const QVector<Torrent> rows = MegaPeerSource::parseSearchPage(
        html, QUrl(QStringLiteral("https://megapeer.vip/browse.php")));
    QCOMPARE(rows.size(), 1);
    const Torrent& t = rows.first();
    QCOMPARE(t.name, QStringLiteral("Sicario (2015) BDRip-AVC"));
    QCOMPARE(t.seeders, 17);
    QCOMPARE(t.leechers, 3);
    QCOMPARE(t.info.value(QStringLiteral("sourceTopicId")).toInt(), 77934);
    QCOMPARE(t.info.value(QStringLiteral("sourceDownloadId")).toInt(), 77934);
    QVERIFY(t.info.value(QStringLiteral("sourceUrl")).toString()
        .contains(QStringLiteral("/torrent/77934/")));
    QVERIFY(t.info.value(QStringLiteral("sourceTorrentUrl")).toString()
        .contains(QStringLiteral("/download/77934/")));
}

void TestMegaPeerSource::parsesIdOnlyUrls()
{
    const QByteArray html = R"(
      <table>
       <tr class="table_fon">
        <td>28 Мая 24</td>
        <td><a href="/torrent/123"><b>Compact URL release</b></a></td>
        <td><a href="/download/123">download</a></td>
        <td>700 MB</td>
        <td><font>5</font> <font>1</font></td>
       </tr>
      </table>)";
    const QVector<Torrent> rows = MegaPeerSource::parseSearchPage(
        html, QUrl(QStringLiteral("https://megapeer.vip/browse.php")));
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows.first().info.value(QStringLiteral("sourceTopicId")).toInt(), 123);
    QCOMPARE(rows.first().info.value(QStringLiteral("sourceDownloadId")).toInt(), 123);
}

void TestMegaPeerSource::verifiesExactDownloadAndRichAudio()
{
    Torrent t;
    t.hash = QStringLiteral("0123456789abcdef0123456789abcdef01234567");
    t.name = QStringLiteral("Artist - Album (2005) MP3");
    t.contentType = rats::domain::ContentType::Audio;
    t.info[QStringLiteral("sourceProvider")] = QStringLiteral("megapeer");
    t.info[QStringLiteral("sourceTopicId")] = 101;
    t.info[QStringLiteral("sourceDownloadId")] = 101;
    t.info[QStringLiteral("sourceUrl")]
        = QStringLiteral("https://megapeer.vip/torrent/101/artist-album");
    t.info[QStringLiteral("sourceTorrentUrl")]
        = QStringLiteral("https://megapeer.vip/download/101/artist-album.torrent");
    t.info[QStringLiteral("contentTypeEvidence")]
        = QStringLiteral("torrent-files");

    const QByteArray html = R"(
      <html><body>
       <a href="/download/101/artist-album.torrent">torrent</a>
       <div>
        Информация о раздаче<br>
        Категория: Музыка<br>
        Название: Album<br>
        Исполнитель: Artist<br>
        Формат/Кодек: MP3<br>
        Битрейт аудио: 320 kbps<br>
        Тип рипа: tracks<br>
        Описание: Exact public release page with a complete track listing,
        source notes, edition information and technical audio parameters. This
        text is deliberately substantial so the strict exact-source contract
        proves useful release information rather than a title-only result.
       </div>
      </body></html>)";

    QVERIFY(MegaPeerSource::applyDetailPage(
        t, html, QUrl(QStringLiteral(
            "https://megapeer.vip/torrent/101/artist-album"))));
    QVERIFY(t.info.value(QStringLiteral("sourceVerified")).toBool());
    QCOMPARE(rats::domain::toId(t.contentType),
        rats::domain::toId(rats::domain::ContentType::Audio));
    QVERIFY(t.info.value(QStringLiteral("audioTracks")).toArray().size() >= 2);
    QVERIFY(MegaPeerSource::isStrictComplete(t));
}

void TestMegaPeerSource::rejectsWrongDownloadId()
{
    Torrent t;
    t.hash = QStringLiteral("0123456789abcdef0123456789abcdef01234567");
    t.name = QStringLiteral("Release");
    t.contentType = rats::domain::ContentType::Audio;
    t.info[QStringLiteral("sourceProvider")] = QStringLiteral("megapeer");
    t.info[QStringLiteral("sourceTopicId")] = 101;
    t.info[QStringLiteral("sourceDownloadId")] = 101;
    t.info[QStringLiteral("sourceUrl")]
        = QStringLiteral("https://megapeer.vip/torrent/101/release");

    const QByteArray html = R"(
      <a href="/download/999/wrong.torrent">wrong</a>
      <div>Описание: long enough exact looking description that must still
      fail because the concrete page does not point to the torrent download
      paired with the candidate search row.</div>)";
    QVERIFY(!MegaPeerSource::applyDetailPage(
        t, html, QUrl(QStringLiteral(
            "https://megapeer.vip/torrent/101/release"))));
}

QTEST_MAIN(TestMegaPeerSource)
#include "test_megapeer_source.moc"
