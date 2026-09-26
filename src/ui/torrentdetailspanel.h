#ifndef TORRENTDETAILSPANEL_H
#define TORRENTDETAILSPANEL_H

#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>

#include "domain/torrent.h"

namespace rats::app {
class Application;
}

namespace rats::net {
class RichMetadataResolver;
}

/**
 * @brief Panel for displaying detailed torrent information
 * Similar to TorrentPage in legacy React app
 *
 * Features migrated from legacy:
 * - Voting (Good/Bad buttons)
 * - Download progress display
 *
 * Note: Files tree has been moved to TorrentFilesWidget (bottom panel)
 */
class TorrentDetailsPanel : public QWidget {
    Q_OBJECT

public:
    explicit TorrentDetailsPanel(QWidget* parent = nullptr);
    ~TorrentDetailsPanel();

    void setApplication(rats::app::Application* app);
    void setTorrent(const rats::domain::Torrent& torrent);
    void clear();
    QString currentHash() const { return currentHash_; }

    // Download progress
    void setDownloadProgress(double progress, qint64 downloaded, qint64 total, int speed);
    void setDownloadCompleted();
    void resetDownloadState();

signals:
    void downloadRequested(const QString& hash);
    void downloadCancelRequested(const QString& hash);
    void closeRequested();
    void goToDownloadsRequested();

public slots:
    void onVotesUpdated(const QString& hash, int good, int bad);

private slots:
    void onMagnetClicked();
    void onDownloadClicked();
    void onCopyHashClicked();
    void onGoodVoteClicked();
    void onBadVoteClicked();
    void onCancelDownloadClicked();
    void onFavoriteClicked();
    // Repository signalled that this torrent's row changed (tracker counts/info).
    void onTorrentUpdated(const QString& hash);
    void onTrackerInfoAvailable(const QString& hash, const QJsonObject& info);
    void onTrackerInfoCheckFinished(const QString& hash, bool found);
    void onRemoteTorrentReceived(const QString& hash, const QJsonObject& data);
    void onRichMetadataFound(const QString& hash, const QJsonObject& patch);

private:
    void setupUi();
    // Repaint the seeders/leechers/completed row after a tracker count scrape.
    void updateTrackerStats(int seeders, int leechers, int completed);
    void updateRatingDisplay();
    void updateVotingButtons();
    QString makeBreakable(const QString& text) const;

    void updateFavoriteButton();

    rats::app::Application* app_ = nullptr;

    // Header section
    QLabel* titleLabel_;
    QLabel* contentTypeLabel_;
    QWidget* contentTypeIcon_;

    // Info section
    QLabel* sizeLabel_;
    QLabel* filesLabel_;
    QLabel* dateLabel_;
    QLabel* hashLabel_;
    QLabel* categoryLabel_;

    // Stats section
    QLabel* seedersLabel_;
    QLabel* leechersLabel_;
    QLabel* completedLabel_;

    // Rating/Voting section
    QProgressBar* ratingBar_;
    QLabel* ratingLabel_;
    QPushButton* goodVoteButton_;
    QPushButton* badVoteButton_;
    QLabel* votesLabel_;

    // Download progress section
    QWidget* downloadProgressWidget_;
    QProgressBar* downloadProgressBar_;
    QLabel* downloadStatusLabel_;
    QLabel* downloadSpeedLabel_;
    QPushButton* cancelDownloadButton_;
    QPushButton* goToDownloadsButton_;

    // Actions
    QPushButton* magnetButton_;
    QPushButton* downloadButton_;
    QPushButton* favoriteButton_;
    QPushButton* copyHashButton_;
    QPushButton* closeButton_;

    // Multi-source torrent information resolver. Tracker websites are tried
    // first, then connected Rats Search peers, an exact public info-hash index,
    // and finally raw BitTorrent DHT/BEP 9 metadata. Every path terminates with
    // either data or a visible error.
    void requestTrackerRefresh();
    void requestRichMetadataEnrichment(const QString& hash);
    void requestPeerInfoFallback(const QString& hash);
    void requestPublicIndexFallback(const QString& hash);
    void requestDhtMetadataFallback(const QString& hash);
    void showInfoUnavailable(const QString& hash, const QString& reason = QString());
    bool hasUsefulTrackerInfo(const QJsonObject& info) const;
    bool hasUserFacingInfo(const QJsonObject& info) const;
    bool hasReleaseSpecificInfo(const QJsonObject& info) const;
    void mergeInfoPatch(const QJsonObject& patch, bool persist = true);
    void enrichFromTorrentIdentity();
    void updateTrackerInfoDisplay(const QJsonObject& info);
    void updateTechnicalInfoDisplay(const QJsonObject& info);
    void rebuildMetadataLinks(const QJsonObject& info);
    void loadPosterImage(const QString& url);

    // Tracker info UI elements
    QWidget* trackerInfoWidget_; // Container for all tracker info
    QLabel* trackerInfoLoadingLabel_; // Current resolution stage / terminal error
    QLabel* trackerInfoSourceLabel_; // Human-facing metadata sources
    QLabel* trackerUrlsLabel_; // Kept for backwards compatibility; hidden in the rich UI
    QLabel* technicalInfoLabel_; // Resolution/video/audio/language details
    QPushButton* retryInfoButton_; // Retry all resolution paths after failure
    QLabel* posterLabel_; // Poster/cover image
    QLabel* descriptionLabel_; // Description text (expandable)
    QPushButton* descriptionToggle_; // "Show more / Show less" button
    QWidget* trackerLinksWidget_; // Container for tracker link buttons
    QGridLayout* trackerLinksLayout_; // Two-column wrapping layout for source links
    bool descriptionExpanded_ = false;
    QString fullDescription_; // Full description text
    QNetworkAccessManager* posterNetworkManager_;
    rats::net::RichMetadataResolver* richMetadataResolver_ = nullptr;

    // Current torrent data
    QString currentHash_;
    rats::domain::Torrent currentTorrent_;
    bool isDownloading_ = false;
    bool hasVoted_ = false;

    // State for the current hash's staged information lookup.
    bool infoResolved_ = false; // enough human-facing data to stop the spinner
    bool trackerLookupFinished_ = false;
    bool richMetadataRequested_ = false;
    bool peerFallbackRequested_ = false;
    bool publicIndexFallbackRequested_ = false;
    bool dhtFallbackRequested_ = false;
    QString lastInfoError_;
};

#endif // TORRENTDETAILSPANEL_H
