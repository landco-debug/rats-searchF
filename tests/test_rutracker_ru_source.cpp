#include <QJsonArray>
#include <QtTest/QtTest>
#include <QUrlQuery>

#include "net/rutracker_ru_source.h"

using rats::domain::Torrent;
using rats::net::RuTrackerRuSource;

class TestRuTrackerRuSource : public QObject {
    Q_OBJECT

private slots:
    void buildsAuthenticatedForumSearchUrl();
    void parsesCurrentExactSearchRowWithoutPretendingHashIsPublic();
    void forumLabelClassifiesAudio();
    void detailPageCompletesIdentityFromExactMagnet();
    void audioDetailPageIsStrictWithoutVideo();
    void wrongTopicPageIsRejected();
};

static const QString kHash
    = QStringLiteral("89abcdef0123456789abcdef0123456789abcdef");

void TestRuTrackerRuSource::buildsAuthenticatedForumSearchUrl()
{
    const QUrl url = RuTrackerRuSource::searchUrl(
        QStringLiteral("Under Siege"), QStringLiteral("seeders_desc"));
    QCOMPARE(url.scheme(), QStringLiteral("https"));
    QCOMPARE(url.host(), QStringLiteral("rutracker.org"));
    QCOMPARE(url.path(), QStringLiteral("/forum/tracker.php"));

    QUrlQuery query(url);
    QCOMPARE(query.queryItemValue(QStringLiteral("nm")), QStringLiteral("Under Siege"));
    QCOMPARE(query.queryItemValue(QStringLiteral("f[]")), QStringLiteral("-1"));
}

void TestRuTrackerRuSource::parsesCurrentExactSearchRowWithoutPretendingHashIsPublic()
{
    const QByteArray html = R"(
      <table id="tor-tbl"><tbody>
        <tr id="trs-tr-777">
          <td class="f-name-col"><div class="f-name"><a href="tracker.php?f=2198">HD Video</a></div></td>
          <td class="t-title-col"><div class="t-title">
            <a class="tLink" data-topic_id="777" href="viewtopic.php?t=777"><b>Under Siege (1992) BDRip 1080p</b></a>
          </div></td>
          <td class="tor-size" data-ts_text="20401094656"><a class="tr-dl" href="dl.php?t=777">18.99 GB</a></td>
          <td>author</td><td>misc</td><td>misc</td>
          <td class="seedmed" data-ts_text="84"><b>84</b></td>
          <td class="leechmed"><b>7</b></td>
          <td>1000</td>
          <td data-ts_text="1790500000">date</td>
        </tr>
      </tbody></table>)";

    const QVector<Torrent> torrents = RuTrackerRuSource::parseSearchPage(
        html, QUrl(QStringLiteral("https://rutracker.org/forum/tracker.php")));
    QCOMPARE(torrents.size(), 1);

    const Torrent& t = torrents.first();
    QVERIFY(t.hash.isEmpty());
    QCOMPARE(t.seeders, 84);
    QCOMPARE(t.leechers, 7);
    QCOMPARE(t.size, 20401094656LL);
    QCOMPARE(t.info.value(QStringLiteral("sourceProvider")).toString(),
        QStringLiteral("rutracker-ru"));
    QCOMPARE(t.info.value(QStringLiteral("sourceTopicId")).toInt(), 777);
    QCOMPARE(t.info.value(QStringLiteral("sourceForumId")).toInt(), 2198);
    QCOMPARE(rats::domain::toId(t.contentType),
        rats::domain::toId(rats::domain::ContentType::Video));
    QCOMPARE(t.info.value(QStringLiteral("sourceUrl")).toString(),
        QStringLiteral("https://rutracker.org/forum/viewtopic.php?t=777"));
    QCOMPARE(t.info.value(QStringLiteral("sourceTorrentUrl")).toString(),
        QStringLiteral("https://rutracker.org/forum/dl.php?t=777"));
    QVERIFY(!t.info.value(QStringLiteral("sourceVerified")).toBool());
}

void TestRuTrackerRuSource::forumLabelClassifiesAudio()
{
    const QByteArray html = R"(
      <tr id="trs-tr-778">
        <td class="f-name-col"><div class="f-name"><a href="tracker.php?f=409">Зарубежная музыка</a></div></td>
        <td class="t-title-col"><div class="t-title">
          <a data-topic_id="778" class="tLink" href="viewtopic.php?t=778"><b>Sade - Diamond Life [FLAC]</b></a>
        </div></td>
        <td class="tor-size" data-ts_text="1048576000"><a class="tr-dl" href="dl.php?t=778">1 GB</a></td>
        <td></td><td></td><td></td>
        <td class="seedmed" data-ts_text="30"><b>30</b></td>
        <td class="leechmed"><b>2</b></td><td></td><td data-ts_text="1790500000"></td>
      </tr>)";
    const QVector<Torrent> torrents = RuTrackerRuSource::parseSearchPage(
        html, QUrl(QStringLiteral("https://rutracker.org/forum/tracker.php")));
    QCOMPARE(torrents.size(), 1);
    QCOMPARE(rats::domain::toId(torrents.first().contentType),
        rats::domain::toId(rats::domain::ContentType::Audio));
    QCOMPARE(torrents.first().info.value(QStringLiteral("contentTypeEvidence")).toString(),
        QStringLiteral("source-category"));
}

void TestRuTrackerRuSource::detailPageCompletesIdentityFromExactMagnet()
{
    Torrent t;
    t.name = QStringLiteral("Under Siege BDRip 1080p");
    t.contentType = rats::domain::ContentType::Video;
    t.info[QStringLiteral("sourceProvider")] = QStringLiteral("rutracker-ru");
    t.info[QStringLiteral("sourceTopicId")] = 777;
    t.info[QStringLiteral("sourceForumId")] = 2198;
    t.info[QStringLiteral("contentTypeEvidence")] = QStringLiteral("source-category");
    t.info[QStringLiteral("sourceUrl")]
        = QStringLiteral("https://rutracker.org/forum/viewtopic.php?t=777");

    const QByteArray html = R"(
      <html><body>
      <h1 id="topic-title">Under Siege / В осаде (1992) BDRip 1080p</h1>
      <table class="attach"><tr><td>
      <a class="magnet-link" href="magnet:?xt=urn:btih:89abcdef0123456789abcdef0123456789abcdef">magnet</a>
      </td></tr></table>
      <div class="post_body">
        Качество: BDRip 1080p<span class="post-br"></span>
        Видео: AVC / H.264, 1920x1080, 23.976 fps, 12.5 Mbps<span class="post-br"></span>
        Аудио #1: Russian DTS 5.1, 1509 kbps<span class="post-br"></span>
        Аудио #2: English AC3 5.1, 640 kbps<span class="post-br"></span>
        Субтитры: Russian, English<span class="post-br"></span>
        Подробное описание конкретного релиза с параметрами исходника, кодирования,
        дорожек и субтитров. Этот текст намеренно достаточно длинный, чтобы strict
        completeness не принимал короткую общую аннотацию фильма за техническое
        описание раздачи.
      </div><!--/post_body-->
      </body></html>)";

    QVERIFY(RuTrackerRuSource::applyDetailPage(
        t, html, QUrl(QStringLiteral(
            "https://rutracker.org/forum/viewtopic.php?t=777"))));
    QCOMPARE(t.hash, kHash);
    QVERIFY(t.info.value(QStringLiteral("sourceVerified")).toBool());
    QVERIFY(!t.info.value(QStringLiteral("quality")).toString().isEmpty());
    QVERIFY(!t.info.value(QStringLiteral("video")).toString().isEmpty());
    QCOMPARE(t.info.value(QStringLiteral("audioTracks")).toArray().size(), 2);
    QVERIFY(RuTrackerRuSource::isStrictComplete(t));
}

void TestRuTrackerRuSource::audioDetailPageIsStrictWithoutVideo()
{
    Torrent t;
    t.name = QStringLiteral("Sade - Diamond Life [FLAC]");
    t.contentType = rats::domain::ContentType::Audio;
    t.info[QStringLiteral("sourceProvider")] = QStringLiteral("rutracker-ru");
    t.info[QStringLiteral("sourceTopicId")] = 778;
    t.info[QStringLiteral("contentTypeEvidence")] = QStringLiteral("source-category");
    t.info[QStringLiteral("sourceUrl")]
        = QStringLiteral("https://rutracker.org/forum/viewtopic.php?t=778");

    const QByteArray html = R"(
      <h1 id="topic-title">Sade - Diamond Life [FLAC]</h1>
      <a class="magnet-link" href="magnet:?xt=urn:btih:89abcdef0123456789abcdef0123456789abcdef">magnet</a>
      <div class="post_body">
      Исполнитель: Sade<span class="post-br"></span>
      Альбом: Diamond Life<span class="post-br"></span>
      Формат/Кодек: FLAC<span class="post-br"></span>
      Битрейт аудио: lossless<span class="post-br"></span>
      Тип рипа: tracks + .cue<span class="post-br"></span>
      Подробное описание конкретной музыкальной раздачи, включая издание, источник рипа,
      треклист и технические параметры lossless-аудио. Этот текст намеренно достаточно длинный
      для строгой проверки без несуществующих полей Видео и Качество.
      </div><!--/post_body-->)";

    QVERIFY(RuTrackerRuSource::applyDetailPage(
        t, html, QUrl(QStringLiteral(
            "https://rutracker.org/forum/viewtopic.php?t=778"))));
    QCOMPARE(t.hash, kHash);
    QVERIFY(t.info.value(QStringLiteral("video")).toString().isEmpty());
    QVERIFY(t.info.value(QStringLiteral("audioTracks")).toArray().size() >= 2);
    QVERIFY(RuTrackerRuSource::isStrictComplete(t));
}

void TestRuTrackerRuSource::wrongTopicPageIsRejected()
{
    Torrent t;
    t.name = QStringLiteral("Expected release");
    t.info[QStringLiteral("sourceProvider")] = QStringLiteral("rutracker-ru");
    t.info[QStringLiteral("sourceTopicId")] = 777;
    t.info[QStringLiteral("sourceUrl")]
        = QStringLiteral("https://rutracker.org/forum/viewtopic.php?t=777");

    const QByteArray html = R"(
      <div class="post_body">
      A sufficiently long exact-looking release description that must still be
      rejected because the returned concrete topic URL does not match the topic
      selected in the search row. It also includes a perfectly valid magnet.
      </div><!--/post_body-->
      <a href="magnet:?xt=urn:btih:89abcdef0123456789abcdef0123456789abcdef">magnet</a>)";

    QVERIFY(!RuTrackerRuSource::applyDetailPage(
        t, html, QUrl(QStringLiteral(
            "https://rutracker.org/forum/viewtopic.php?t=999"))));
    QVERIFY(t.hash.isEmpty());
}

QTEST_MAIN(TestRuTrackerRuSource)
#include "test_rutracker_ru_source.moc"
