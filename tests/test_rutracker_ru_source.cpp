#include <QJsonArray>
#include <QtTest/QtTest>
#include <QUrlQuery>

#include "net/rutracker_ru_source.h"

using rats::domain::Torrent;
using rats::net::RuTrackerRuSource;

class TestRuTrackerRuSource : public QObject {
    Q_OBJECT

private slots:
    void buildsPublicSearchUrl();
    void audioSearchUsesAudioForums();
    void searchRowCarriesExactProvenanceAndSwarmCounts();
    void audioForumClassifiesAudio();
    void detailPageVerifiesExactHashAndRichInfo();
    void audioDetailPageIsStrictWithoutVideo();
    void mismatchedHashIsRejected();
};

static const QString kHash
    = QStringLiteral("89abcdef0123456789abcdef0123456789abcdef");

void TestRuTrackerRuSource::buildsPublicSearchUrl()
{
    const QUrl url = RuTrackerRuSource::searchUrl(
        QStringLiteral("Under Siege"), QStringLiteral("seeders_desc"));
    QCOMPARE(url.scheme(), QStringLiteral("http"));
    QCOMPARE(url.host(), QStringLiteral("rutracker.ru"));

    QUrlQuery query(url);
    QCOMPARE(query.queryItemValue(QStringLiteral("nm")), QStringLiteral("Under Siege"));
    QCOMPARE(query.queryItemValue(QStringLiteral("o")), QStringLiteral("10"));
    QCOMPARE(query.queryItemValue(QStringLiteral("s")), QStringLiteral("2"));
}

void TestRuTrackerRuSource::audioSearchUsesAudioForums()
{
    const QUrl url = RuTrackerRuSource::searchUrl(
        QStringLiteral("Sade"), QStringLiteral("seeders_desc"), QStringLiteral("audio"));
    QUrlQuery query(url);
    QStringList forums;
    for (const auto& item : query.queryItems())
        if (item.first == QStringLiteral("f[]"))
            forums.append(item.second);
    QVERIFY(forums.contains(QStringLiteral("1666")));
    QVERIFY(!forums.contains(QStringLiteral("-1")));
}

void TestRuTrackerRuSource::searchRowCarriesExactProvenanceAndSwarmCounts()
{
    const QByteArray html = R"(
      <table>
        <tr id="tor_777">
          <td><a href="tracker.php?f=1757">HD Video</a></td>
          <td><a href="./viewtopic.php?t=777"><b>Under Siege (1992) BDRip 1080p</b></a></td>
          <td>author</td><td>downloads</td><td>misc</td>
          <td><u>20401094656</u> 18.99 GB</td>
          <td class="seedmed"><b>84</b></td>
          <td class='leechmed'><b>7</b></td>
          <td>1000</td>
          <td><u>1790500000</u></td>
          <td><a href="magnet:?xt=urn:btih:89ABCDEF0123456789ABCDEF0123456789ABCDEF">magnet</a></td>
        </tr>
      </table>)";

    const QVector<Torrent> torrents = RuTrackerRuSource::parseSearchPage(
        html, QUrl(QStringLiteral("http://rutracker.ru/tracker.php")));
    QCOMPARE(torrents.size(), 1);

    const Torrent& t = torrents.first();
    QCOMPARE(t.hash, kHash);
    QCOMPARE(t.seeders, 84);
    QCOMPARE(t.leechers, 7);
    QCOMPARE(t.size, 20401094656LL);
    QCOMPARE(t.info.value(QStringLiteral("sourceProvider")).toString(),
        QStringLiteral("rutracker-ru"));
    QCOMPARE(t.info.value(QStringLiteral("sourceTopicId")).toInt(), 777);
    QCOMPARE(t.info.value(QStringLiteral("sourceForumId")).toInt(), 1757);
    QCOMPARE(rats::domain::toId(t.contentType), rats::domain::toId(rats::domain::ContentType::Video));
    QCOMPARE(t.info.value(QStringLiteral("sourceUrl")).toString(),
        QStringLiteral("http://rutracker.ru/viewtopic.php?t=777"));
    QVERIFY(!t.info.value(QStringLiteral("sourceVerified")).toBool());
}

void TestRuTrackerRuSource::audioForumClassifiesAudio()
{
    const QByteArray html = R"(
      <tr id="tor_778">
        <td><a href="tracker.php?f=1666">Зарубежная рок-музыка</a></td>
        <td><a href="./viewtopic.php?t=778"><b>Sade - Diamond Life [FLAC]</b></a></td>
        <td>author</td><td>downloads</td><td>misc</td><td><u>1048576000</u></td>
        <td class="seedmed"><b>30</b></td><td class="leechmed"><b>2</b></td>
        <td>100</td><td><u>1790500000</u></td>
        <td><a href="magnet:?xt=urn:btih:89ABCDEF0123456789ABCDEF0123456789ABCDEF">magnet</a></td>
      </tr>)";
    const QVector<Torrent> torrents=RuTrackerRuSource::parseSearchPage(
        html,QUrl(QStringLiteral("http://rutracker.ru/tracker.php")));
    QCOMPARE(torrents.size(),1);
    QCOMPARE(torrents.first().info.value(QStringLiteral("sourceForumId")).toInt(),1666);
    QCOMPARE(rats::domain::toId(torrents.first().contentType),rats::domain::toId(rats::domain::ContentType::Audio));
}

void TestRuTrackerRuSource::detailPageVerifiesExactHashAndRichInfo()
{
    Torrent t;
    t.hash = kHash;
    t.name = QStringLiteral("Under Siege BDRip 1080p");
    t.info[QStringLiteral("sourceProvider")] = QStringLiteral("rutracker-ru");
    t.info[QStringLiteral("sourceTopicId")] = 777;
    t.info[QStringLiteral("sourceUrl")]
        = QStringLiteral("http://rutracker.ru/viewtopic.php?t=777");

    const QByteArray html = R"(
      <html><body>
      <h1 id="topic-title">Under Siege / В осаде (1992) BDRip 1080p</h1>
      <a class="magnet-link" href="magnet:?xt=urn:btih:89abcdef0123456789abcdef0123456789abcdef">magnet</a>
      <div class="post_body">
        <img class="postImgAligned" title="http://img.example/poster.jpg"/>
        Качество: BDRip 1080p<span class="post-br"></span>
        Видео: AVC / H.264, 1920x1080, 23.976 fps, 12.5 Mbps<span class="post-br"></span>
        Аудио #1: Russian DTS 5.1, 1509 kbps<span class="post-br"></span>
        Аудио #2: English AC3 5.1, 640 kbps<span class="post-br"></span>
        Субтитры: Russian, English<span class="post-br"></span>
        Подробное описание конкретного релиза с параметрами исходника, кодирования,
        дорожек и субтитров. Этот текст намеренно достаточно длинный, чтобы strict
        completeness не принимал короткую общую аннотацию фильма за техническое
        описание раздачи.
      </div><div class="post-footer">footer</div>
      </body></html>)";

    QVERIFY(RuTrackerRuSource::applyDetailPage(
        t, html, QUrl(QStringLiteral("http://rutracker.ru/viewtopic.php?t=777"))));
    QVERIFY(t.info.value(QStringLiteral("sourceVerified")).toBool());
    QVERIFY(!t.info.value(QStringLiteral("quality")).toString().isEmpty());
    QVERIFY(!t.info.value(QStringLiteral("video")).toString().isEmpty());
    QCOMPARE(t.info.value(QStringLiteral("audioTracks")).toArray().size(), 2);
    QVERIFY(RuTrackerRuSource::isStrictComplete(t));
}

void TestRuTrackerRuSource::audioDetailPageIsStrictWithoutVideo()
{
    Torrent t;
    t.hash=kHash;
    t.name=QStringLiteral("Sade - Diamond Life [FLAC]");
    t.contentType=rats::domain::ContentType::Audio;
    t.info[QStringLiteral("sourceProvider")]=QStringLiteral("rutracker-ru");
    t.info[QStringLiteral("sourceTopicId")]=778;
    t.info[QStringLiteral("sourceForumId")]=1666;
    t.info[QStringLiteral("sourceUrl")]=QStringLiteral("http://rutracker.ru/viewtopic.php?t=778");
    const QByteArray html=R"(
      <h1 id="topic-title">Sade - Diamond Life [FLAC]</h1>
      <a href="magnet:?xt=urn:btih:89abcdef0123456789abcdef0123456789abcdef">magnet</a>
      <div class="post_body">
      Исполнитель: Sade<span class="post-br"></span>
      Альбом: Diamond Life<span class="post-br"></span>
      Формат: FLAC<span class="post-br"></span>
      Битрейт: Lossless<span class="post-br"></span>
      Тип рипа: tracks + .cue<span class="post-br"></span>
      Подробное описание конкретной музыкальной раздачи, включая издание, источник рипа,
      треклист и технические параметры lossless-аудио. Этот текст намеренно достаточно длинный
      для строгой проверки без несуществующих полей Видео и Качество.
      </div><!--/post_body-->)";
    QVERIFY(RuTrackerRuSource::applyDetailPage(t,html,QUrl(QStringLiteral("http://rutracker.ru/viewtopic.php?t=778"))));
    QVERIFY(t.info.value(QStringLiteral("video")).toString().isEmpty());
    QVERIFY(t.info.value(QStringLiteral("audioTracks")).toArray().size()>=2);
    QVERIFY(RuTrackerRuSource::isStrictComplete(t));
}

void TestRuTrackerRuSource::mismatchedHashIsRejected()
{
    Torrent t;
    t.hash = kHash;
    t.name = QStringLiteral("Expected release");
    t.info[QStringLiteral("sourceProvider")] = QStringLiteral("rutracker-ru");
    t.info[QStringLiteral("sourceUrl")]
        = QStringLiteral("http://rutracker.ru/viewtopic.php?t=1");

    const QByteArray html
        = R"(<div class="post_body">Качество: 1080p</div>
             <a href="magnet:?xt=urn:btih:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa">magnet</a>)";

    QVERIFY(!RuTrackerRuSource::applyDetailPage(
        t, html, QUrl(QStringLiteral("http://rutracker.ru/viewtopic.php?t=1"))));
    QVERIFY(!t.info.value(QStringLiteral("sourceVerified")).toBool());
}

QTEST_MAIN(TestRuTrackerRuSource)
#include "test_rutracker_ru_source.moc"
