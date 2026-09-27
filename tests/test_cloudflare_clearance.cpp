#include <QtTest>

#include "net/cloudflare_clearance.h"

using rats::net::looksLikeCloudflareChallenge;

class TestCloudflareClearance : public QObject {
    Q_OBJECT

private slots:
    void detectsManagedChallenge();
    void detectsForbiddenCloudflarePage();
    void ignoresNormalTrackerHtml();
};

void TestCloudflareClearance::detectsManagedChallenge()
{
    const QByteArray body
        = "<html><title>Just a moment...</title>"
          "<script src='/cdn-cgi/challenge-platform/h/g/orchestrate/chl_page/v1'></script>"
          "</html>";
    QVERIFY(looksLikeCloudflareChallenge(200, body));
}

void TestCloudflareClearance::detectsForbiddenCloudflarePage()
{
    const QByteArray body
        = "<html><h1>403 Forbidden</h1><p>cloudflare challenge</p></html>";
    QVERIFY(looksLikeCloudflareChallenge(403, body));
}

void TestCloudflareClearance::ignoresNormalTrackerHtml()
{
    const QByteArray body
        = "<html><table class='table_fon'><tr><td>normal torrent row</td></tr></table></html>";
    QVERIFY(!looksLikeCloudflareChallenge(200, body));
}

QTEST_MAIN(TestCloudflareClearance)
#include "test_cloudflare_clearance.moc"
