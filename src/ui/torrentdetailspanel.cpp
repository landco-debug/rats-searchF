#include "torrentdetailspanel.h"
#include "format.h"

#include "app/application.h"
#include "app/favorites_store.h"
#include "data/torrent_repository.h"
#include "domain/content.h"
#include "domain/torrent_codec.h"
#include "net/torrent_engine.h"
#include "net/media_metadata_utils.h"
#include "net/rich_metadata_resolver.h"
#include "peer/peer_api.h"
#include "services/download_service.h"
#include "services/tracker_service.h"
#include "services/voting_service.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDebug>
#include <QDesktopServices>
#include <QFont>
#include <QFrame>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPixmap>
#include <QPointer>
#include <QScrollArea>
#include <QSet>
#include <QStyle>
#include <QTimer>
#include <QUrl>

using rats::domain::ContentCategory;
using rats::domain::ContentType;

TorrentDetailsPanel::TorrentDetailsPanel(QWidget* parent) : QWidget(parent)
{
    setupUi();

    richMetadataResolver_ = new rats::net::RichMetadataResolver(this);
    connect(richMetadataResolver_, &rats::net::RichMetadataResolver::metadataFound, this,
        &TorrentDetailsPanel::onRichMetadataFound);

    clear();
}

TorrentDetailsPanel::~TorrentDetailsPanel() { }

void TorrentDetailsPanel::setupUi()
{
    setObjectName("detailsPanel");

    // Main layout for the panel (no margins - scroll area fills entire panel)
    QVBoxLayout* panelLayout = new QVBoxLayout(this);
    panelLayout->setContentsMargins(0, 0, 0, 0);
    panelLayout->setSpacing(0);

    // Create scroll area for vertical scrolling
    QScrollArea* scrollArea = new QScrollArea();
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea->setObjectName("detailsScrollArea");

    // Content widget inside scroll area
    QWidget* contentWidget = new QWidget();
    QVBoxLayout* mainLayout = new QVBoxLayout(contentWidget);
    mainLayout->setContentsMargins(16, 12, 16, 16);
    mainLayout->setSpacing(12);

    // Header with close button
    QHBoxLayout* headerLayout = new QHBoxLayout();
    headerLayout->setSpacing(8);

    // Content type icon
    contentTypeIcon_ = new QWidget();
    contentTypeIcon_->setFixedSize(32, 32);
    contentTypeIcon_->setObjectName("contentTypeIcon");
    headerLayout->addWidget(contentTypeIcon_);

    // Title
    titleLabel_ = new QLabel(tr("Select a torrent"));
    titleLabel_->setWordWrap(true);
    titleLabel_->setObjectName("detailsTitleLabel");
    headerLayout->addWidget(titleLabel_, 1);

    // Close button
    closeButton_ = new QPushButton("×");
    closeButton_->setObjectName("closeButton");
    closeButton_->setFixedSize(28, 28);
    closeButton_->setCursor(Qt::PointingHandCursor);
    connect(closeButton_, &QPushButton::clicked, this, &TorrentDetailsPanel::closeRequested);
    headerLayout->addWidget(closeButton_);

    mainLayout->addLayout(headerLayout);

    // Content type label
    contentTypeLabel_ = new QLabel();
    contentTypeLabel_->setObjectName("contentTypeLabel");
    mainLayout->addWidget(contentTypeLabel_);

    // Separator
    QFrame* sep1 = new QFrame();
    sep1->setFrameShape(QFrame::HLine);
    sep1->setObjectName("detailsSeparator");
    sep1->setFixedHeight(1);
    mainLayout->addWidget(sep1);

    // Stats section (seeders, leechers, completed)
    QLabel* statsTitle = new QLabel(tr("Statistics"));
    statsTitle->setObjectName("sectionTitle");
    mainLayout->addWidget(statsTitle);

    QHBoxLayout* statsLayout = new QHBoxLayout();
    statsLayout->setSpacing(16);

    // Seeders
    QVBoxLayout* seedersLayout = new QVBoxLayout();
    seedersLabel_ = new QLabel("0");
    seedersLabel_->setObjectName("seedersLabel");
    seedersLabel_->setAlignment(Qt::AlignCenter);
    QLabel* seedersText = new QLabel(tr("Seeders"));
    seedersText->setObjectName("statsSubLabel");
    seedersText->setAlignment(Qt::AlignCenter);
    seedersLayout->addWidget(seedersLabel_);
    seedersLayout->addWidget(seedersText);
    statsLayout->addLayout(seedersLayout);

    // Leechers
    QVBoxLayout* leechersLayout = new QVBoxLayout();
    leechersLabel_ = new QLabel("0");
    leechersLabel_->setObjectName("leechersLabel");
    leechersLabel_->setAlignment(Qt::AlignCenter);
    QLabel* leechersText = new QLabel(tr("Leechers"));
    leechersText->setObjectName("statsSubLabel");
    leechersText->setAlignment(Qt::AlignCenter);
    leechersLayout->addWidget(leechersLabel_);
    leechersLayout->addWidget(leechersText);
    statsLayout->addLayout(leechersLayout);

    // Completed
    QVBoxLayout* completedLayout = new QVBoxLayout();
    completedLabel_ = new QLabel("0");
    completedLabel_->setObjectName("completedLabel");
    completedLabel_->setAlignment(Qt::AlignCenter);
    QLabel* completedText = new QLabel(tr("Completed"));
    completedText->setObjectName("statsSubLabel");
    completedText->setAlignment(Qt::AlignCenter);
    completedLayout->addWidget(completedLabel_);
    completedLayout->addWidget(completedText);
    statsLayout->addLayout(completedLayout);

    mainLayout->addLayout(statsLayout);

    // Rating bar
    QHBoxLayout* ratingLayout = new QHBoxLayout();
    ratingBar_ = new QProgressBar();
    ratingBar_->setObjectName("ratingBar");
    ratingBar_->setRange(0, 100);
    ratingBar_->setValue(0);
    ratingBar_->setTextVisible(false);
    ratingBar_->setFixedHeight(6);
    ratingLayout->addWidget(ratingBar_, 1);
    ratingLabel_ = new QLabel("N/A");
    ratingLabel_->setObjectName("ratingLabel");
    ratingLayout->addWidget(ratingLabel_);
    mainLayout->addLayout(ratingLayout);

    // Voting buttons (migrated from legacy/app/torrent-page.js)
    QHBoxLayout* votingLayout = new QHBoxLayout();
    votingLayout->setSpacing(8);

    goodVoteButton_ = new QPushButton(tr("👍 Good"));
    goodVoteButton_->setObjectName("goodVoteButton");
    goodVoteButton_->setCursor(Qt::PointingHandCursor);
    connect(goodVoteButton_, &QPushButton::clicked, this, &TorrentDetailsPanel::onGoodVoteClicked);
    votingLayout->addWidget(goodVoteButton_);

    badVoteButton_ = new QPushButton(tr("👎 Bad"));
    badVoteButton_->setObjectName("badVoteButton");
    badVoteButton_->setCursor(Qt::PointingHandCursor);
    connect(badVoteButton_, &QPushButton::clicked, this, &TorrentDetailsPanel::onBadVoteClicked);
    votingLayout->addWidget(badVoteButton_);

    votingLayout->addStretch();

    votesLabel_ = new QLabel();
    votesLabel_->setObjectName("votesLabel");
    votingLayout->addWidget(votesLabel_);

    mainLayout->addLayout(votingLayout);

    // Tracker info section (poster, description, links from tracker websites)
    trackerInfoWidget_ = new QWidget();
    trackerInfoWidget_->setObjectName("trackerInfoWidget");
    QVBoxLayout* trackerInfoLayout = new QVBoxLayout(trackerInfoWidget_);
    trackerInfoLayout->setContentsMargins(0, 0, 0, 0);
    trackerInfoLayout->setSpacing(8);

    QFrame* sepTracker = new QFrame();
    sepTracker->setFrameShape(QFrame::HLine);
    sepTracker->setObjectName("detailsSeparator");
    sepTracker->setFixedHeight(1);
    trackerInfoLayout->addWidget(sepTracker);

    QLabel* trackerInfoTitle = new QLabel(tr("Media / Release Info"));
    trackerInfoTitle->setObjectName("sectionTitle");
    trackerInfoLayout->addWidget(trackerInfoTitle);

    // Loading indicator
    trackerInfoLoadingLabel_ = new QLabel(tr("🔍 Loading tracker info..."));
    trackerInfoLoadingLabel_->setObjectName("trackerLoadingLabel");
    trackerInfoLoadingLabel_->setWordWrap(true);
    trackerInfoLoadingLabel_->hide();
    trackerInfoLayout->addWidget(trackerInfoLoadingLabel_);

    trackerInfoSourceLabel_ = new QLabel();
    trackerInfoSourceLabel_->setObjectName("hintLabel");
    trackerInfoSourceLabel_->setWordWrap(true);
    trackerInfoSourceLabel_->hide();
    trackerInfoLayout->addWidget(trackerInfoSourceLabel_);

    technicalInfoLabel_ = new QLabel();
    technicalInfoLabel_->setObjectName("descriptionLabel");
    technicalInfoLabel_->setWordWrap(true);
    technicalInfoLabel_->setTextFormat(Qt::PlainText);
    technicalInfoLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    technicalInfoLabel_->hide();
    trackerInfoLayout->addWidget(technicalInfoLabel_);

    trackerUrlsLabel_ = new QLabel();
    trackerUrlsLabel_->setObjectName("hintLabel");
    trackerUrlsLabel_->setWordWrap(true);
    trackerUrlsLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    trackerUrlsLabel_->hide();
    trackerInfoLayout->addWidget(trackerUrlsLabel_);

    retryInfoButton_ = new QPushButton(tr("Retry information lookup"));
    retryInfoButton_->setObjectName("secondaryButton");
    retryInfoButton_->setCursor(Qt::PointingHandCursor);
    retryInfoButton_->hide();
    connect(retryInfoButton_, &QPushButton::clicked, this, [this]() {
        trackerLookupFinished_ = false;
        richMetadataRequested_ = false;
        peerFallbackRequested_ = false;
        publicIndexFallbackRequested_ = false;
        dhtFallbackRequested_ = false;
        lastInfoError_.clear();
        requestTrackerRefresh();
    });
    trackerInfoLayout->addWidget(retryInfoButton_);

    // Poster image
    posterLabel_ = new QLabel();
    posterLabel_->setObjectName("posterLabel");
    posterLabel_->setAlignment(Qt::AlignCenter);
    posterLabel_->setMaximumHeight(300);
    posterLabel_->setScaledContents(false);
    posterLabel_->hide();
    trackerInfoLayout->addWidget(posterLabel_);

    // Description
    descriptionLabel_ = new QLabel();
    descriptionLabel_->setObjectName("descriptionLabel");
    descriptionLabel_->setWordWrap(true);
    descriptionLabel_->setTextFormat(Qt::PlainText);
    descriptionLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    descriptionLabel_->hide();
    trackerInfoLayout->addWidget(descriptionLabel_);

    // Show more/less toggle
    descriptionToggle_ = new QPushButton(tr("Show more ▼"));
    descriptionToggle_->setObjectName("descriptionToggle");
    descriptionToggle_->setCursor(Qt::PointingHandCursor);
    descriptionToggle_->setFlat(true);
    descriptionToggle_->hide();
    connect(descriptionToggle_, &QPushButton::clicked, this, [this]() {
        descriptionExpanded_ = !descriptionExpanded_;
        if (descriptionExpanded_) {
            descriptionLabel_->setText(fullDescription_);
            descriptionToggle_->setText(tr("Show less ▲"));
        } else {
            // Show first ~300 chars
            QString preview = fullDescription_.left(300);
            if (fullDescription_.length() > 300)
                preview += "...";
            descriptionLabel_->setText(preview);
            descriptionToggle_->setText(tr("Show more ▼"));
        }
    });
    trackerInfoLayout->addWidget(descriptionToggle_);

    // Tracker links
    trackerLinksWidget_ = new QWidget();
    trackerLinksLayout_ = new QHBoxLayout(trackerLinksWidget_);
    trackerLinksLayout_->setContentsMargins(0, 4, 0, 0);
    trackerLinksLayout_->setSpacing(8);
    trackerLinksLayout_->addStretch();
    trackerLinksWidget_->hide();
    trackerInfoLayout->addWidget(trackerLinksWidget_);

    trackerInfoWidget_->hide();
    mainLayout->addWidget(trackerInfoWidget_);

    posterNetworkManager_ = new QNetworkAccessManager(this);

    // Download progress section (hidden by default)
    downloadProgressWidget_ = new QWidget();
    downloadProgressWidget_->setObjectName("downloadProgressWidget");
    QVBoxLayout* downloadLayout = new QVBoxLayout(downloadProgressWidget_);
    downloadLayout->setContentsMargins(12, 8, 12, 8);
    downloadLayout->setSpacing(6);

    QHBoxLayout* downloadHeaderLayout = new QHBoxLayout();
    QLabel* downloadTitle = new QLabel(tr("📥 Downloading..."));
    downloadTitle->setObjectName("downloadTitleLabel");
    downloadHeaderLayout->addWidget(downloadTitle);
    downloadHeaderLayout->addStretch();
    downloadSpeedLabel_ = new QLabel();
    downloadSpeedLabel_->setObjectName("downloadSpeedLabel");
    downloadHeaderLayout->addWidget(downloadSpeedLabel_);
    downloadLayout->addLayout(downloadHeaderLayout);

    downloadProgressBar_ = new QProgressBar();
    downloadProgressBar_->setObjectName("downloadProgressBarDetails");
    downloadProgressBar_->setRange(0, 100);
    downloadProgressBar_->setValue(0);
    downloadProgressBar_->setTextVisible(true);
    downloadProgressBar_->setFixedHeight(20);
    downloadLayout->addWidget(downloadProgressBar_);

    QHBoxLayout* downloadStatusLayout = new QHBoxLayout();
    downloadStatusLabel_ = new QLabel();
    downloadStatusLabel_->setObjectName("downloadStatusLabel");
    downloadStatusLayout->addWidget(downloadStatusLabel_);
    downloadStatusLayout->addStretch();
    cancelDownloadButton_ = new QPushButton(tr("Cancel"));
    cancelDownloadButton_->setObjectName("cancelDownloadButton");
    cancelDownloadButton_->setCursor(Qt::PointingHandCursor);
    connect(cancelDownloadButton_, &QPushButton::clicked, this, &TorrentDetailsPanel::onCancelDownloadClicked);
    downloadStatusLayout->addWidget(cancelDownloadButton_);
    downloadLayout->addLayout(downloadStatusLayout);

    // Go to Downloads button
    goToDownloadsButton_ = new QPushButton(tr("📥 Go to Downloads"));
    goToDownloadsButton_->setObjectName("goToDownloadsButton");
    goToDownloadsButton_->setCursor(Qt::PointingHandCursor);
    connect(goToDownloadsButton_, &QPushButton::clicked, this, &TorrentDetailsPanel::goToDownloadsRequested);
    downloadLayout->addWidget(goToDownloadsButton_);

    downloadProgressWidget_->hide();
    mainLayout->addWidget(downloadProgressWidget_);

    // Info section
    QLabel* infoTitle = new QLabel(tr("Information"));
    infoTitle->setObjectName("sectionTitle");
    mainLayout->addWidget(infoTitle);

    // Size
    QHBoxLayout* sizeRow = new QHBoxLayout();
    QLabel* sizeTitle = new QLabel(tr("Size:"));
    sizeTitle->setObjectName("infoLabel");
    sizeTitle->setFixedWidth(80);
    sizeLabel_ = new QLabel("-");
    sizeLabel_->setObjectName("infoValue");
    sizeRow->addWidget(sizeTitle);
    sizeRow->addWidget(sizeLabel_, 1);
    mainLayout->addLayout(sizeRow);

    // Files
    QHBoxLayout* filesRow = new QHBoxLayout();
    QLabel* filesTitle = new QLabel(tr("Files:"));
    filesTitle->setObjectName("infoLabel");
    filesTitle->setFixedWidth(80);
    filesLabel_ = new QLabel("-");
    filesLabel_->setObjectName("infoValue");
    filesRow->addWidget(filesTitle);
    filesRow->addWidget(filesLabel_, 1);
    mainLayout->addLayout(filesRow);

    // Date
    QHBoxLayout* dateRow = new QHBoxLayout();
    QLabel* dateTitle = new QLabel(tr("Added:"));
    dateTitle->setObjectName("infoLabel");
    dateTitle->setFixedWidth(80);
    dateLabel_ = new QLabel("-");
    dateLabel_->setObjectName("infoValue");
    dateRow->addWidget(dateTitle);
    dateRow->addWidget(dateLabel_, 1);
    mainLayout->addLayout(dateRow);

    // Category
    QHBoxLayout* categoryRow = new QHBoxLayout();
    QLabel* categoryTitle = new QLabel(tr("Category:"));
    categoryTitle->setObjectName("infoLabel");
    categoryTitle->setFixedWidth(80);
    categoryLabel_ = new QLabel("-");
    categoryLabel_->setObjectName("infoValue");
    categoryRow->addWidget(categoryTitle);
    categoryRow->addWidget(categoryLabel_, 1);
    mainLayout->addLayout(categoryRow);

    // Hash section
    QLabel* hashTitle = new QLabel(tr("Info Hash"));
    hashTitle->setObjectName("sectionTitle");
    mainLayout->addWidget(hashTitle);

    hashLabel_ = new QLabel("-");
    hashLabel_->setObjectName("hashLabel");
    hashLabel_->setWordWrap(true);
    hashLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    mainLayout->addWidget(hashLabel_);

    // Spacer
    mainLayout->addStretch();

    // Action buttons
    QLabel* actionsTitle = new QLabel(tr("Actions"));
    actionsTitle->setObjectName("sectionTitle");
    mainLayout->addWidget(actionsTitle);

    // Magnet button
    magnetButton_ = new QPushButton(tr("Open Magnet Link"));
    magnetButton_->setObjectName("magnetButton");
    magnetButton_->setCursor(Qt::PointingHandCursor);
    connect(magnetButton_, &QPushButton::clicked, this, &TorrentDetailsPanel::onMagnetClicked);
    mainLayout->addWidget(magnetButton_);

    // Download button
    downloadButton_ = new QPushButton(tr("Download"));
    downloadButton_->setObjectName("successButton");
    downloadButton_->setCursor(Qt::PointingHandCursor);
    connect(downloadButton_, &QPushButton::clicked, this, &TorrentDetailsPanel::onDownloadClicked);
    mainLayout->addWidget(downloadButton_);

    // Favorite button
    favoriteButton_ = new QPushButton(tr("⭐ Add to Favorites"));
    favoriteButton_->setObjectName("secondaryButton");
    favoriteButton_->setCursor(Qt::PointingHandCursor);
    connect(favoriteButton_, &QPushButton::clicked, this, &TorrentDetailsPanel::onFavoriteClicked);
    mainLayout->addWidget(favoriteButton_);

    // Copy hash button
    copyHashButton_ = new QPushButton(tr("Copy Info Hash"));
    copyHashButton_->setObjectName("secondaryButton");
    copyHashButton_->setCursor(Qt::PointingHandCursor);
    connect(copyHashButton_, &QPushButton::clicked, this, &TorrentDetailsPanel::onCopyHashClicked);
    mainLayout->addWidget(copyHashButton_);

    // Set up scroll area with content
    scrollArea->setWidget(contentWidget);
    panelLayout->addWidget(scrollArea);
}

void TorrentDetailsPanel::setApplication(rats::app::Application* app)
{
    app_ = app;
    if (!app_)
        return;

    if (auto* voting = app_->voting()) {
        connect(voting, &rats::service::VotingService::votesUpdated, this, &TorrentDetailsPanel::onVotesUpdated);
    }
    if (auto* repo = app_->torrents()) {
        connect(repo, &rats::data::TorrentRepository::torrentUpdated, this, &TorrentDetailsPanel::onTorrentUpdated);
    }
    if (auto* fav = app_->favorites()) {
        connect(fav, &rats::app::FavoritesStore::favoritesChanged, this, &TorrentDetailsPanel::updateFavoriteButton);
    }
    if (auto* trackers = app_->trackers()) {
        connect(trackers, &rats::service::TrackerService::infoAvailable, this,
            &TorrentDetailsPanel::onTrackerInfoAvailable);
        connect(trackers, &rats::service::TrackerService::infoCheckFinished, this,
            &TorrentDetailsPanel::onTrackerInfoCheckFinished);
    }
    if (auto* peers = app_->peerApi()) {
        connect(peers, &rats::peer::PeerApi::remoteTorrentReceived, this,
            &TorrentDetailsPanel::onRemoteTorrentReceived);
    }
}

void TorrentDetailsPanel::setTorrent(const rats::domain::Torrent& torrent)
{
    currentTorrent_ = torrent;
    currentHash_ = torrent.hash;

    // Search rows intentionally stay small and may omit file paths. Pull the
    // selected torrent's local full record when available: release filenames
    // often encode more useful exact information than any generic movie API
    // (x265, 10bit, Rus/Eng, AAC5.1, REMUX, etc.).
    if (app_ && app_->torrents()) {
        if (auto stored = app_->torrents()->get(currentHash_, true)) {
            if (currentTorrent_.fileList.isEmpty())
                currentTorrent_.fileList = stored->fileList;
            for (auto it = stored->info.constBegin(); it != stored->info.constEnd(); ++it) {
                if (!currentTorrent_.info.contains(it.key()) || currentTorrent_.info.value(it.key()).isNull()
                    || currentTorrent_.info.value(it.key()).isUndefined()) {
                    currentTorrent_.info.insert(it.key(), it.value());
                }
            }
        }
    }

    infoResolved_ = hasUserFacingInfo(currentTorrent_.info);
    trackerLookupFinished_ = false;
    richMetadataRequested_ = false;
    peerFallbackRequested_ = false;
    publicIndexFallbackRequested_ = false;
    dhtFallbackRequested_ = false;
    lastInfoError_.clear();

    // Check voted status from the voting service (distributed store).
    hasVoted_ = (app_ && app_->voting()) ? app_->voting()->hasVoted(torrent.hash) : false;

    // Update UI - use makeBreakable to allow wrapping of long names without
    // spaces
    titleLabel_->setText(makeBreakable(torrent.name));

    // Content type — emoji glyph + human name from the domain type.
    const QString typeName = (torrent.contentType == ContentType::Unknown)
        ? tr("Unknown")
        : rats::ui::capitalizeFirst(rats::domain::toString(torrent.contentType));
    contentTypeLabel_->setText(rats::ui::contentTypeIcon(torrent.contentType) + " " + typeName);

    // Stats
    seedersLabel_->setText(QString::number(torrent.seeders));
    leechersLabel_->setText(QString::number(torrent.leechers));
    completedLabel_->setText(QString::number(torrent.completed));

    // Info
    sizeLabel_->setText(rats::ui::formatSize(torrent.size));
    filesLabel_->setText(tr("%n file(s)", nullptr, torrent.files));
    dateLabel_->setText(torrent.added.isValid() ? torrent.added.toString("MMMM d, yyyy") : "-");

    // Category — display the human content type + optional finer category.
    const bool typeUnknown = (torrent.contentType == ContentType::Unknown);
    const bool catUnknown = (torrent.contentCategory == ContentCategory::Unknown);
    categoryLabel_->setText(typeUnknown ? tr("Unknown")
                                        : rats::ui::capitalizeFirst(rats::domain::toString(torrent.contentType))
                + (catUnknown
                        ? QString()
                        : " (" + rats::ui::capitalizeFirst(rats::domain::toString(torrent.contentCategory)) + ")"));

    // Hash - use makeBreakable for long hash strings
    hashLabel_->setText(makeBreakable(torrent.hash));

    // Rating
    updateRatingDisplay();
    updateVotingButtons();
    updateFavoriteButton();

    // Check if this torrent is currently downloading
    if (app_ && app_->downloads() && app_->downloads()->isDownloading(torrent.hash)) {
        rats::service::Download d = app_->downloads()->getDownload(torrent.hash);
        if (d.completed) {
            setDownloadCompleted();
        } else {
            setDownloadProgress(d.progress, d.downloadedBytes, d.totalSize, static_cast<int>(d.downloadSpeed));
        }
    } else {
        // Reset to normal download button state
        resetDownloadState();
    }

    // Derive exact-but-limited facts from the selected torrent name and file
    // paths before any network request. This makes obvious details visible even
    // when all tracker sites are unavailable.
    enrichFromTorrentIdentity();

    // Show cached/derived human-facing metadata immediately. Low-level tracker
    // URLs or DHT bookkeeping alone are intentionally not treated as useful
    // movie/release information.
    if (hasUserFacingInfo(currentTorrent_.info)) {
        updateTrackerInfoDisplay(currentTorrent_.info);
    } else {
        trackerInfoWidget_->hide();
        trackerInfoLoadingLabel_->hide();
        trackerInfoSourceLabel_->hide();
        technicalInfoLabel_->hide();
        trackerUrlsLabel_->hide();
        retryInfoButton_->hide();
        posterLabel_->hide();
        descriptionLabel_->hide();
        descriptionToggle_->hide();
        trackerLinksWidget_->hide();
        fullDescription_.clear();
        descriptionExpanded_ = false;
    }

    setVisible(true);

    // Kick off background tracker (counts + website info) refresh via the
    // service.
    requestTrackerRefresh();
}

void TorrentDetailsPanel::clear()
{
    currentHash_.clear();
    currentTorrent_ = rats::domain::Torrent();
    hasVoted_ = false;

    titleLabel_->setText(tr("Select a torrent"));
    contentTypeLabel_->clear();

    seedersLabel_->setText("0");
    leechersLabel_->setText("0");
    completedLabel_->setText("0");

    sizeLabel_->setText("-");
    filesLabel_->setText("-");
    dateLabel_->setText("-");
    categoryLabel_->setText("-");
    hashLabel_->setText("-");

    ratingBar_->setValue(0);
    ratingLabel_->setText("N/A");
    votesLabel_->setText(tr("No votes yet"));

    // Clear tracker info
    trackerInfoWidget_->hide();
    posterLabel_->hide();
    posterLabel_->clear();
    descriptionLabel_->hide();
    descriptionLabel_->clear();
    descriptionToggle_->hide();
    trackerLinksWidget_->hide();
    trackerInfoLoadingLabel_->hide();
    trackerInfoSourceLabel_->hide();
    trackerInfoSourceLabel_->clear();
    technicalInfoLabel_->hide();
    technicalInfoLabel_->clear();
    trackerUrlsLabel_->hide();
    trackerUrlsLabel_->clear();
    retryInfoButton_->hide();
    fullDescription_.clear();
    descriptionExpanded_ = false;
    infoResolved_ = false;
    trackerLookupFinished_ = false;
    richMetadataRequested_ = false;
    peerFallbackRequested_ = false;
    publicIndexFallbackRequested_ = false;
    dhtFallbackRequested_ = false;
    lastInfoError_.clear();

    // Remove old tracker link buttons
    while (trackerLinksLayout_->count() > 1) { // keep the stretch
        QLayoutItem* item = trackerLinksLayout_->takeAt(0);
        if (item->widget()) {
            delete item->widget();
        }
        delete item;
    }
}

void TorrentDetailsPanel::updateRatingDisplay()
{
    int good = currentTorrent_.good;
    int bad = currentTorrent_.bad;

    if (good == 0 && bad == 0) {
        ratingBar_->setValue(0);
        ratingBar_->setProperty("ratingType", "neutral");
        ratingLabel_->setText(tr("No ratings"));
        ratingLabel_->setProperty("ratingType", "neutral");
    } else {
        int rating = static_cast<int>((static_cast<double>(good) / (good + bad)) * 100);
        ratingBar_->setValue(rating);

        QString ratingType = rating >= 50 ? "good" : "bad";
        ratingBar_->setProperty("ratingType", ratingType);
        ratingLabel_->setText(QString("%1%").arg(rating));
        ratingLabel_->setProperty("ratingType", ratingType);
    }

    ratingBar_->style()->unpolish(ratingBar_);
    ratingBar_->style()->polish(ratingBar_);
    ratingLabel_->style()->unpolish(ratingLabel_);
    ratingLabel_->style()->polish(ratingLabel_);
}

void TorrentDetailsPanel::onMagnetClicked()
{
    if (currentHash_.isEmpty())
        return;

    QDesktopServices::openUrl(QUrl(currentTorrent_.magnetLink()));
}

void TorrentDetailsPanel::onDownloadClicked()
{
    if (currentHash_.isEmpty())
        return;
    emit downloadRequested(currentHash_);
}

void TorrentDetailsPanel::onCopyHashClicked()
{
    if (currentHash_.isEmpty())
        return;

    QClipboard* clipboard = QApplication::clipboard();
    clipboard->setText(currentHash_);

    // Visual feedback
    copyHashButton_->setText(tr("Copied!"));
    QTimer::singleShot(2000, this, [this]() { copyHashButton_->setText(tr("Copy Info Hash")); });
}

void TorrentDetailsPanel::onFavoriteClicked()
{
    if (currentHash_.isEmpty() || !app_ || !app_->favorites())
        return;

    auto* fav = app_->favorites();
    if (fav->isFavorite(currentHash_)) {
        fav->remove(currentHash_);
    } else {
        fav->add(currentTorrent_);
    }

    updateFavoriteButton();
}

void TorrentDetailsPanel::updateFavoriteButton()
{
    if (!app_ || !app_->favorites() || currentHash_.isEmpty()) {
        favoriteButton_->setText(tr("⭐ Add to Favorites"));
        return;
    }

    if (app_->favorites()->isFavorite(currentHash_)) {
        favoriteButton_->setText(tr("★ In Favorites (Remove)"));
        favoriteButton_->setObjectName("warningButton");
    } else {
        favoriteButton_->setText(tr("⭐ Add to Favorites"));
        favoriteButton_->setObjectName("secondaryButton");
    }
    favoriteButton_->style()->unpolish(favoriteButton_);
    favoriteButton_->style()->polish(favoriteButton_);
}

void TorrentDetailsPanel::onGoodVoteClicked()
{
    if (currentHash_.isEmpty())
        return;

    hasVoted_ = true;
    updateVotingButtons();

    if (app_ && app_->voting()) {
        app_->voting()->vote(currentHash_, true, {});
    }
}

void TorrentDetailsPanel::onBadVoteClicked()
{
    if (currentHash_.isEmpty())
        return;

    hasVoted_ = true;
    updateVotingButtons();

    if (app_ && app_->voting()) {
        app_->voting()->vote(currentHash_, false, {});
    }
}

void TorrentDetailsPanel::onVotesUpdated(const QString& hash, int good, int bad)
{
    if (hash != currentHash_)
        return;

    currentTorrent_.good = good;
    currentTorrent_.bad = bad;
    updateRatingDisplay();
    updateVotingButtons();
}

void TorrentDetailsPanel::updateVotingButtons()
{
    int total = currentTorrent_.good + currentTorrent_.bad;
    if (total > 0) {
        votesLabel_->setText(tr("%n vote(s)", nullptr, total));
    } else {
        votesLabel_->setText(tr("No votes yet"));
    }

    goodVoteButton_->setEnabled(!hasVoted_);
    badVoteButton_->setEnabled(!hasVoted_);

    if (hasVoted_) {
        goodVoteButton_->setText(tr("👍 Voted"));
        badVoteButton_->setText(tr("👎 Voted"));
    } else {
        goodVoteButton_->setText(tr("👍 Good"));
        badVoteButton_->setText(tr("👎 Bad"));
    }
}

void TorrentDetailsPanel::setDownloadProgress(double progress, qint64 downloaded, qint64 total, int speed)
{
    isDownloading_ = true;
    downloadProgressWidget_->show();
    downloadButton_->hide();

    int percent = static_cast<int>(progress * 100);
    downloadProgressBar_->setValue(percent);

    downloadStatusLabel_->setText(
        QString("%1 / %2").arg(rats::ui::formatSize(downloaded), rats::ui::formatSize(total)));
    downloadSpeedLabel_->setText(rats::ui::formatSpeed(speed));
}

void TorrentDetailsPanel::setDownloadCompleted()
{
    isDownloading_ = false;
    downloadProgressWidget_->hide();
    downloadButton_->show();
    downloadButton_->setText(tr("✓ Completed"));
    downloadButton_->setEnabled(false);
    downloadButton_->setObjectName("completedButton");
    downloadButton_->style()->unpolish(downloadButton_);
    downloadButton_->style()->polish(downloadButton_);
}

void TorrentDetailsPanel::resetDownloadState()
{
    isDownloading_ = false;
    downloadProgressWidget_->hide();
    downloadButton_->show();
    downloadButton_->setText(tr("Download"));
    downloadButton_->setEnabled(true);
    downloadButton_->setObjectName("successButton");
    downloadButton_->style()->unpolish(downloadButton_);
    downloadButton_->style()->polish(downloadButton_);
}

void TorrentDetailsPanel::onCancelDownloadClicked()
{
    if (!currentHash_.isEmpty()) {
        emit downloadCancelRequested(currentHash_);
    }
    resetDownloadState();
}

QString TorrentDetailsPanel::makeBreakable(const QString& text) const
{
    // Insert zero-width space after common separators to allow line breaking
    // This helps with long filenames without spaces
    const QChar zwsp(0x200B); // Zero-width space
    QString result;
    result.reserve(text.size() * 2);

    int consecutiveChars = 0;
    const int maxConsecutive = 20; // Force break after this many chars without a break opportunity

    for (int i = 0; i < text.size(); ++i) {
        QChar c = text[i];
        result += c;

        if (c.isSpace()) {
            consecutiveChars = 0;
        } else {
            consecutiveChars++;

            // Insert break opportunity after common separators
            if (c == '.' || c == '_' || c == '-' || c == '~' || c == '+' || c == '[' || c == ']' || c == '('
                || c == ')') {
                result += zwsp;
                consecutiveChars = 0;
            }
            // Force break after many consecutive non-space characters
            else if (consecutiveChars >= maxConsecutive) {
                result += zwsp;
                consecutiveChars = 0;
            }
        }
    }

    return result;
}

void TorrentDetailsPanel::updateTrackerStats(int seeders, int leechers, int completed)
{
    currentTorrent_.seeders = seeders;
    currentTorrent_.leechers = leechers;
    currentTorrent_.completed = completed;

    seedersLabel_->setText(QString::number(seeders));
    leechersLabel_->setText(QString::number(leechers));
    completedLabel_->setText(QString::number(completed));
}

// ============================================================================
// Tracker refresh — delegated to TrackerService. Results are persisted to the
// repository, which emits torrentUpdated(hash); onTorrentUpdated() then reloads
// the row and refreshes stats + scraped info. Rate limiting lives in the
// service.
// ============================================================================

bool TorrentDetailsPanel::hasUserFacingInfo(const QJsonObject& info) const
{
    return !info.value(QStringLiteral("description")).toString().trimmed().isEmpty()
        || !info.value(QStringLiteral("synopsis")).toString().trimmed().isEmpty()
        || !info.value(QStringLiteral("releaseDetails")).toString().trimmed().isEmpty()
        || !info.value(QStringLiteral("technicalInfo")).toObject().isEmpty();
}

bool TorrentDetailsPanel::hasReleaseSpecificInfo(const QJsonObject& info) const
{
    const QString description = info.value(QStringLiteral("description")).toString().trimmed();
    const QString synopsis = info.value(QStringLiteral("synopsis")).toString().trimmed();
    const QString releaseDetails = info.value(QStringLiteral("releaseDetails")).toString().trimmed();

    // A full tracker post is already enough: it is exactly the kind of
    // user-facing release card the legacy application tried to display.
    if (rats::net::metadata::descriptionRichness(description) >= 1400)
        return true;
    if (releaseDetails.size() >= 220)
        return true;

    const QJsonObject tech = info.value(QStringLiteral("technicalInfo")).toObject();
    auto present = [&tech](const QString& key) {
        const QJsonValue value = tech.value(key);
        return (value.isString() && !value.toString().trimmed().isEmpty())
            || (value.isArray() && !value.toArray().isEmpty());
    };

    int technicalGroups = 0;
    for (const QString& key : { QStringLiteral("resolution"), QStringLiteral("source"), QStringLiteral("videoCodec"),
             QStringLiteral("bitDepth"), QStringLiteral("hdr"), QStringLiteral("audioCodecs"),
             QStringLiteral("audioChannels"), QStringLiteral("audioDetails"), QStringLiteral("languages"),
             QStringLiteral("subtitles") }) {
        if (present(key))
            ++technicalGroups;
    }

    const bool hasAudioOrLanguage = present(QStringLiteral("audioCodecs")) || present(QStringLiteral("audioChannels"))
        || present(QStringLiteral("audioDetails")) || present(QStringLiteral("languages"));
    const bool hasNarrative = description.size() >= 120 || synopsis.size() >= 120;

    // "1080p" or "Movie · 2160p" by itself is *not* a successful result.
    // Consider the card complete only when generic narrative plus several exact
    // release facts are available, or when an exact audio-track description was
    // obtained.
    return present(QStringLiteral("audioDetails")) || (hasNarrative && technicalGroups >= 3 && hasAudioOrLanguage);
}

bool TorrentDetailsPanel::hasUsefulTrackerInfo(const QJsonObject& info) const
{
    if (info.isEmpty())
        return false;
    if (hasUserFacingInfo(info) || !info.value(QStringLiteral("poster")).toString().isEmpty())
        return true;
    if (info.value(QStringLiteral("rutrackerThreadId")).toInt() > 0
        || info.value(QStringLiteral("nyaaThreadId")).toInt() > 0
        || info.value(QStringLiteral("rutorThreadId")).toInt() > 0
        || info.value(QStringLiteral("x1337ThreadId")).toInt() > 0)
        return true;
    return !info.value(QStringLiteral("metadataSources")).toArray().isEmpty()
        || !info.value(QStringLiteral("metadataSource")).toString().isEmpty();
}

void TorrentDetailsPanel::mergeInfoPatch(const QJsonObject& patch, bool persist)
{
    if (patch.isEmpty() || currentHash_.isEmpty())
        return;

    QJsonObject merged = currentTorrent_.info;

    auto mergeStringArray = [](const QJsonArray& a, const QJsonArray& b) {
        QJsonArray out;
        QStringList seen;
        auto append = [&out, &seen](const QJsonValue& value) {
            const QString text = value.toString().trimmed();
            if (text.isEmpty())
                return;
            for (const QString& existing : seen) {
                if (existing.compare(text, Qt::CaseInsensitive) == 0)
                    return;
            }
            seen.append(text);
            out.append(text);
        };
        for (const QJsonValue& value : a)
            append(value);
        for (const QJsonValue& value : b)
            append(value);
        return out;
    };

    // Sources and tracker identities accumulate; a later enrichment must not
    // erase evidence from an earlier source.
    merged[QStringLiteral("metadataSources")] = mergeStringArray(
        merged.value(QStringLiteral("metadataSources")).toArray(),
        patch.value(QStringLiteral("metadataSources")).toArray());
    merged[QStringLiteral("trackers")]
        = mergeStringArray(merged.value(QStringLiteral("trackers")).toArray(), patch.value(QStringLiteral("trackers")).toArray());

    const QJsonObject incomingTech = patch.value(QStringLiteral("technicalInfo")).toObject();
    if (!incomingTech.isEmpty()) {
        merged[QStringLiteral("technicalInfo")] = rats::net::metadata::mergeTechnicalInfo(
            merged.value(QStringLiteral("technicalInfo")).toObject(), incomingTech);
    }

    if (patch.value(QStringLiteral("sourceDescriptions")).isObject()) {
        QJsonObject descriptions = merged.value(QStringLiteral("sourceDescriptions")).toObject();
        const QJsonObject incoming = patch.value(QStringLiteral("sourceDescriptions")).toObject();
        for (auto it = incoming.constBegin(); it != incoming.constEnd(); ++it)
            descriptions[it.key()] = it.value();
        merged[QStringLiteral("sourceDescriptions")] = descriptions;
    }

    // For release descriptions, richest wins. Generic synopsis is stored
    // separately so it never overwrites tracker-specific audio/video details.
    const QString incomingDescription = patch.value(QStringLiteral("description")).toString().trimmed();
    if (!incomingDescription.isEmpty()) {
        const QString old = merged.value(QStringLiteral("description")).toString();
        if (rats::net::metadata::descriptionRichness(incomingDescription)
            > rats::net::metadata::descriptionRichness(old)) {
            merged[QStringLiteral("description")] = incomingDescription;
        }
    }

    const QString incomingSynopsis = patch.value(QStringLiteral("synopsis")).toString().trimmed();
    if (!incomingSynopsis.isEmpty()
        && incomingSynopsis.size() > merged.value(QStringLiteral("synopsis")).toString().trimmed().size()) {
        merged[QStringLiteral("synopsis")] = incomingSynopsis;
    }

    const QString incomingRelease = patch.value(QStringLiteral("releaseDetails")).toString().trimmed();
    if (!incomingRelease.isEmpty()
        && incomingRelease.size() > merged.value(QStringLiteral("releaseDetails")).toString().trimmed().size()) {
        merged[QStringLiteral("releaseDetails")] = incomingRelease;
    }

    // A tracker/YTS poster already attached to this exact result is preferable
    // to later generic artwork, so only fill an empty slot.
    const QString incomingPoster = patch.value(QStringLiteral("poster")).toString().trimmed();
    if (!incomingPoster.isEmpty() && merged.value(QStringLiteral("poster")).toString().trimmed().isEmpty())
        merged[QStringLiteral("poster")] = incomingPoster;

    // Copy the remaining scalar/source-specific fields. The specially handled
    // aggregate fields above are skipped.
    for (auto it = patch.constBegin(); it != patch.constEnd(); ++it) {
        if (it.key() == QStringLiteral("metadataSources") || it.key() == QStringLiteral("trackers")
            || it.key() == QStringLiteral("technicalInfo") || it.key() == QStringLiteral("sourceDescriptions")
            || it.key() == QStringLiteral("description") || it.key() == QStringLiteral("synopsis")
            || it.key() == QStringLiteral("releaseDetails") || it.key() == QStringLiteral("poster")) {
            continue;
        }
        if (!it.value().isNull() && !it.value().isUndefined())
            merged[it.key()] = it.value();
    }

    currentTorrent_.info = merged;
    infoResolved_ = hasUserFacingInfo(merged);

    if (persist && app_ && app_->torrents())
        app_->torrents()->mergeInfo(currentHash_, merged);

    if (hasUsefulTrackerInfo(merged))
        updateTrackerInfoDisplay(merged);
}

void TorrentDetailsPanel::enrichFromTorrentIdentity()
{
    if (currentHash_.isEmpty())
        return;

    QString corpus = currentTorrent_.name;
    for (const rats::domain::File& file : currentTorrent_.fileList) {
        if (!file.path.trimmed().isEmpty())
            corpus += QLatin1Char('\n') + file.path;
    }

    const QJsonObject tech = rats::net::metadata::extractTechnicalInfo(corpus);
    if (tech.isEmpty())
        return;

    QJsonObject patch;
    QJsonArray sources;
    sources.append(QStringLiteral("Torrent name/files"));
    patch[QStringLiteral("metadataSources")] = sources;
    patch[QStringLiteral("technicalInfo")] = tech;

    // This is exact to the selected torrent, but it is only inference from its
    // own names. Never manufacture an audio track that the filename did not
    // actually expose.
    mergeInfoPatch(patch, false);
}

void TorrentDetailsPanel::requestRichMetadataEnrichment(const QString& hash)
{
    if (hash != currentHash_ || richMetadataRequested_ || !richMetadataResolver_)
        return;

    richMetadataRequested_ = true;
    richMetadataResolver_->resolve(hash, currentTorrent_.name, currentTorrent_.contentCategory);
}

void TorrentDetailsPanel::requestTrackerRefresh()
{
    if (!app_ || currentHash_.isEmpty())
        return;

    const QString hash = currentHash_;
    auto* trackers = app_->trackers();

    if (trackers)
        trackers->checkCounts(hash);

    infoResolved_ = hasUserFacingInfo(currentTorrent_.info);
    if (infoResolved_)
        updateTrackerInfoDisplay(currentTorrent_.info);
    else {
        trackerInfoWidget_->show();
        retryInfoButton_->hide();
        trackerInfoSourceLabel_->hide();
        technicalInfoLabel_->hide();
        trackerUrlsLabel_->hide();
        trackerInfoLoadingLabel_->setText(tr("🔍 Searching release descriptions and media details…"));
        trackerInfoLoadingLabel_->show();
        // The user never has to wait for every source to fail before getting a
        // way forward: reference searches are available immediately.
        rebuildMetadataLinks(currentTorrent_.info);
    }

    // Rich paths run in parallel. Tracker pages provide the release author's
    // full text; YTS/Cinemeta/Torrentio cover exact-hash technical fields and
    // movie/series metadata when tracker sites are unavailable.
    if (trackers)
        trackers->checkInfo(hash, currentTorrent_.name);
    requestRichMetadataEnrichment(hash);

    // Peers may already have a rich tracker description cached.
    QTimer::singleShot(1200, this, [this, hash]() {
        if (hash == currentHash_ && !hasReleaseSpecificInfo(currentTorrent_.info))
            requestPeerInfoFallback(hash);
    });

    // Exact hash index and DHT are lower-level fallbacks. They are useful for
    // release/file metadata, but they are deliberately not presented as the
    // primary "torrent information" when richer human-readable sources exist.
    QTimer::singleShot(3500, this, [this, hash]() {
        if (hash == currentHash_ && !hasReleaseSpecificInfo(currentTorrent_.info))
            requestPublicIndexFallback(hash);
    });

    QTimer::singleShot(6000, this, [this, hash]() {
        if (hash == currentHash_ && !hasReleaseSpecificInfo(currentTorrent_.info))
            requestDhtMetadataFallback(hash);
    });

    // Nothing can leave a permanent spinner. If we found a synopsis but no
    // exact-release audio/video facts, keep the useful data visible and explain
    // that the release-specific layer could not be confirmed.
    QTimer::singleShot(35000, this, [this, hash]() {
        if (hash != currentHash_)
            return;
        if (!hasUserFacingInfo(currentTorrent_.info)) {
            showInfoUnavailable(hash, lastInfoError_);
        } else if (!hasReleaseSpecificInfo(currentTorrent_.info)) {
            trackerInfoLoadingLabel_->setText(
                tr("⚠️ Movie/series information was found, but exact release audio/video details could not be confirmed."));
            trackerInfoLoadingLabel_->show();
            retryInfoButton_->show();
        }
    });
}

void TorrentDetailsPanel::requestPeerInfoFallback(const QString& hash)
{
    if (!app_ || hash != currentHash_ || peerFallbackRequested_ || hasReleaseSpecificInfo(currentTorrent_.info))
        return;
    peerFallbackRequested_ = true;

    if (!hasUserFacingInfo(currentTorrent_.info)) {
        trackerInfoLoadingLabel_->setText(tr("🔎 Asking connected Rats Search peers for cached release details…"));
        trackerInfoLoadingLabel_->show();
    }

    const int sent = app_->peerApi() ? app_->peerApi()->requestTorrentFromPeers(hash, true) : 0;
    if (sent == 0)
        lastInfoError_ = tr("No connected peer could be queried.");
}

void TorrentDetailsPanel::requestPublicIndexFallback(const QString& hash)
{
    if (!app_ || hash != currentHash_ || publicIndexFallbackRequested_)
        return;
    publicIndexFallbackRequested_ = true;

    if (!hasUserFacingInfo(currentTorrent_.info)) {
        trackerInfoLoadingLabel_->setText(tr("🌐 Checking an exact info-hash index…"));
        trackerInfoLoadingLabel_->show();
    }

    QUrl url(QStringLiteral("https://magnetz.eu/api/magnets/infohash/%1").arg(hash));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("RatsSearch/2"));
    request.setTransferTimeout(8000);

    QNetworkReply* reply = posterNetworkManager_->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, hash]() {
        reply->deleteLater();
        if (hash != currentHash_)
            return;

        if (reply->error() != QNetworkReply::NoError) {
            lastInfoError_ = tr("Public index lookup failed: %1").arg(reply->errorString());
            requestDhtMetadataFallback(hash);
            return;
        }

        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll(), &parseError);
        const QJsonObject data = document.object().value(QStringLiteral("data")).toObject();
        const QString returnedHash = data.value(QStringLiteral("info_hash")).toString();

        if (parseError.error != QJsonParseError::NoError || data.isEmpty()
            || returnedHash.compare(hash, Qt::CaseInsensitive) != 0) {
            lastInfoError_ = tr("Public index returned no matching torrent.");
            requestDhtMetadataFallback(hash);
            return;
        }

        QJsonObject info;
        QJsonArray sources;
        sources.append(QStringLiteral("Magnetz"));
        info[QStringLiteral("metadataSources")] = sources;
        info[QStringLiteral("metadataSource")] = QStringLiteral("Magnetz public index");

        const QString webUrl = data.value(QStringLiteral("web_url")).toString();
        if (!webUrl.isEmpty())
            info[QStringLiteral("magnetzUrl")] = webUrl;

        const QString creator = data.value(QStringLiteral("creator")).toString();
        if (!creator.isEmpty())
            info[QStringLiteral("createdBy")] = creator;

        const QString created = data.value(QStringLiteral("creation_date")).toString();
        const QDateTime createdAt = QDateTime::fromString(created, Qt::ISODate);
        if (createdAt.isValid())
            info[QStringLiteral("creationDate")] = createdAt.toSecsSinceEpoch();

        if (data.contains(QStringLiteral("is_private")))
            info[QStringLiteral("private")] = data.value(QStringLiteral("is_private")).toBool();

        const QJsonObject release = data.value(QStringLiteral("release")).toObject();
        QStringList releaseBits;
        for (const char* key : { "type", "resolution", "format" }) {
            const QString value = release.value(QLatin1String(key)).toString().trimmed();
            if (!value.isEmpty())
                releaseBits.append(value);
        }
        if (!releaseBits.isEmpty()) {
            const QString releaseText = releaseBits.join(QStringLiteral(" · "));
            info[QStringLiteral("releaseDetails")] = tr("Release: %1").arg(releaseText);
            info[QStringLiteral("technicalInfo")] = rats::net::metadata::extractTechnicalInfo(releaseText);
        }

        QVector<rats::domain::File> files;
        const QJsonArray fileArray = data.value(QStringLiteral("files")).toArray();
        files.reserve(fileArray.size());
        for (const QJsonValue& value : fileArray) {
            const QJsonObject file = value.toObject();
            const QString path = file.value(QStringLiteral("path")).toString();
            const qint64 size = file.value(QStringLiteral("size")).toVariant().toLongLong();
            if (!path.isEmpty())
                files.append(rats::domain::File { path, size });
        }
        if (!files.isEmpty())
            currentTorrent_.fileList = files;
        if (app_ && app_->torrents() && !files.isEmpty())
            app_->torrents()->updateFiles(hash, files);

        mergeInfoPatch(info);
        enrichFromTorrentIdentity();
        if (!hasUserFacingInfo(currentTorrent_.info))
            requestDhtMetadataFallback(hash);
    });
}

void TorrentDetailsPanel::requestDhtMetadataFallback(const QString& hash)
{
    if (!app_ || hash != currentHash_ || dhtFallbackRequested_)
        return;
    dhtFallbackRequested_ = true;

    auto* engine = app_->engine();
    if (!engine) {
        lastInfoError_ = tr("BitTorrent metadata engine is unavailable.");
        return;
    }

    if (!hasUserFacingInfo(currentTorrent_.info)) {
        trackerInfoLoadingLabel_->setText(tr("🧲 Fetching BitTorrent metadata from DHT / BEP 9…"));
        trackerInfoLoadingLabel_->show();
    }

    QPointer<TorrentDetailsPanel> self(this);
    const bool started = engine->fetchMetadata(
        hash,
        [self, hash](const rats::net::TorrentMetadata& meta, const QString& error) {
            if (!self)
                return;

            QMetaObject::invokeMethod(
                self,
                [self, hash, meta, error]() {
                    if (!self || hash != self->currentHash_)
                        return;

                    if (!meta.valid) {
                        self->lastInfoError_
                            = error.isEmpty() ? self->tr("BitTorrent metadata was not found.") : error;
                        return;
                    }

                    QJsonObject info;
                    QJsonArray sources;
                    sources.append(QStringLiteral("BitTorrent DHT / BEP 9"));
                    info[QStringLiteral("metadataSources")] = sources;
                    info[QStringLiteral("metadataSource")] = QStringLiteral("BitTorrent DHT / BEP 9");

                    if (!meta.comment.isEmpty())
                        info[QStringLiteral("description")] = meta.comment;
                    if (!meta.createdBy.isEmpty())
                        info[QStringLiteral("createdBy")] = meta.createdBy;
                    if (meta.creationDate > 0)
                        info[QStringLiteral("creationDate")] = meta.creationDate;
                    info[QStringLiteral("private")] = meta.isPrivate;

                    const QJsonObject tech
                        = rats::net::metadata::extractTechnicalInfo(meta.name + QLatin1Char('\n') + meta.comment);
                    if (!tech.isEmpty())
                        info[QStringLiteral("technicalInfo")] = tech;

                    QVector<rats::domain::File> files;
                    files.reserve(meta.files.size());
                    for (const auto& file : meta.files)
                        files.append(rats::domain::File { file.path, file.size });
                    if (!files.isEmpty())
                        self->currentTorrent_.fileList = files;
                    if (self->app_ && self->app_->torrents() && !files.isEmpty())
                        self->app_->torrents()->updateFiles(hash, files);

                    self->mergeInfoPatch(info);
                    self->enrichFromTorrentIdentity();
                },
                Qt::QueuedConnection);
        },
        25000);

    if (!started)
        lastInfoError_ = tr("BitTorrent metadata engine is unavailable.");
}

void TorrentDetailsPanel::showInfoUnavailable(const QString& hash, const QString& reason)
{
    if (hash != currentHash_)
        return;

    trackerInfoWidget_->show();
    trackerUrlsLabel_->hide();

    if (hasUserFacingInfo(currentTorrent_.info)) {
        updateTrackerInfoDisplay(currentTorrent_.info);
        QString text = tr("⚠️ Exact release details are incomplete after checking multiple metadata sources.");
        if (!reason.isEmpty())
            text += QStringLiteral("\n") + reason;
        trackerInfoLoadingLabel_->setText(text);
        trackerInfoLoadingLabel_->show();
        retryInfoButton_->show();
        return;
    }

    trackerInfoSourceLabel_->hide();
    technicalInfoLabel_->hide();
    descriptionLabel_->hide();
    descriptionToggle_->hide();
    posterLabel_->hide();

    // Requirement: even when no source can return a machine-readable release
    // card, never leave the user at a dead end. Offer direct exact-hash/title
    // searches on sites that expose human release descriptions.
    rebuildMetadataLinks(currentTorrent_.info);

    QString text = tr("⚠️ Detailed torrent information is unavailable from automatic sources. "
                      "Use one of the links below to open a site with a release description.");
    if (!reason.isEmpty())
        text += QStringLiteral("\n") + reason;
    trackerInfoLoadingLabel_->setText(text);
    trackerInfoLoadingLabel_->show();
    retryInfoButton_->show();
}

void TorrentDetailsPanel::onTrackerInfoAvailable(const QString& hash, const QJsonObject& info)
{
    if (hash != currentHash_ || info.isEmpty())
        return;

    // TrackerService persists the same patch immediately after this signal. Merge
    // only into the live panel here so remote-only hits can update without
    // generating a second database write/signal cycle.
    mergeInfoPatch(info, false);
}

void TorrentDetailsPanel::onTrackerInfoCheckFinished(const QString& hash, bool found)
{
    if (hash != currentHash_)
        return;

    trackerLookupFinished_ = true;

    // A tracker hit may still be a sparse Nyaa description, so "found" is not
    // equivalent to "we have the exact release's audio/video details".
    if (!hasReleaseSpecificInfo(currentTorrent_.info))
        requestPeerInfoFallback(hash);

    if (!hasReleaseSpecificInfo(currentTorrent_.info))
        requestPublicIndexFallback(hash);

    Q_UNUSED(found);
}

void TorrentDetailsPanel::onRemoteTorrentReceived(const QString& hash, const QJsonObject& data)
{
    if (hash != currentHash_)
        return;

    const rats::domain::Torrent peerTorrent = rats::domain::codec::torrentFromJson(data);
    if (!peerTorrent.fileList.isEmpty())
        currentTorrent_.fileList = peerTorrent.fileList;

    // PeerApi/indexing owns persistence. The panel only merges the cached rich
    // description immediately, then mines exact filename/path tags.
    if (!peerTorrent.info.isEmpty())
        mergeInfoPatch(peerTorrent.info, false);
    enrichFromTorrentIdentity();
}

void TorrentDetailsPanel::onRichMetadataFound(const QString& hash, const QJsonObject& patch)
{
    if (hash != currentHash_ || patch.isEmpty())
        return;

    mergeInfoPatch(patch, true);

    if (hasReleaseSpecificInfo(currentTorrent_.info)) {
        trackerInfoLoadingLabel_->hide();
        retryInfoButton_->hide();
    }
}

void TorrentDetailsPanel::onTorrentUpdated(const QString& hash)
{
    if (hash != currentHash_ || !app_ || !app_->torrents())
        return;

    auto updated = app_->torrents()->get(hash, false);
    if (!updated)
        return;

    updateTrackerStats(updated->seeders, updated->leechers, updated->completed);

    // Preserve richer in-memory fields while folding in whatever was just
    // persisted. This also avoids recursively persisting from the repository's
    // own torrentUpdated signal.
    mergeInfoPatch(updated->info, false);
}

void TorrentDetailsPanel::updateTechnicalInfoDisplay(const QJsonObject& info)
{
    const QJsonObject tech = info.value(QStringLiteral("technicalInfo")).toObject();

    auto arrayStrings = [](const QJsonValue& value) {
        QStringList values;
        if (value.isArray()) {
            for (const QJsonValue& item : value.toArray()) {
                const QString text = item.toString().trimmed();
                if (!text.isEmpty() && !values.contains(text, Qt::CaseInsensitive))
                    values.append(text);
            }
        } else if (value.isString() && !value.toString().trimmed().isEmpty()) {
            values.append(value.toString().trimmed());
        }
        return values;
    };

    QStringList lines;

    QStringList quality;
    const QString resolution = tech.value(QStringLiteral("resolution")).toString().trimmed();
    const QString source = tech.value(QStringLiteral("source")).toString().trimmed();
    if (!resolution.isEmpty())
        quality << resolution;
    if (!source.isEmpty() && !quality.contains(source, Qt::CaseInsensitive))
        quality << source;
    if (!quality.isEmpty())
        lines << tr("Quality: %1").arg(quality.join(QStringLiteral(" · ")));

    QStringList video;
    for (const QString& value : { tech.value(QStringLiteral("videoCodec")).toString().trimmed(),
             tech.value(QStringLiteral("bitDepth")).toString().trimmed() }) {
        if (!value.isEmpty() && !video.contains(value, Qt::CaseInsensitive))
            video << value;
    }
    const QStringList hdr = arrayStrings(tech.value(QStringLiteral("hdr")));
    for (const QString& value : hdr) {
        if (!video.contains(value, Qt::CaseInsensitive))
            video << value;
    }
    if (!video.isEmpty())
        lines << tr("Video: %1").arg(video.join(QStringLiteral(" · ")));

    const QStringList audioDetails = arrayStrings(tech.value(QStringLiteral("audioDetails")));
    if (!audioDetails.isEmpty()) {
        lines << tr("Audio tracks:");
        for (const QString& detail : audioDetails)
            lines << QStringLiteral("  ") + detail;
    } else {
        QStringList audio = arrayStrings(tech.value(QStringLiteral("audioCodecs")));
        const QStringList channels = arrayStrings(tech.value(QStringLiteral("audioChannels")));
        for (const QString& value : channels) {
            if (!audio.contains(value, Qt::CaseInsensitive))
                audio << value;
        }
        if (!audio.isEmpty())
            lines << tr("Audio: %1").arg(audio.join(QStringLiteral(" · ")));
    }

    const QStringList languages = arrayStrings(tech.value(QStringLiteral("languages")));
    if (!languages.isEmpty())
        lines << tr("Languages: %1").arg(languages.join(QStringLiteral(", ")));

    const QStringList subtitles = arrayStrings(tech.value(QStringLiteral("subtitles")));
    if (!subtitles.isEmpty()) {
        lines << tr("Subtitles:");
        for (const QString& detail : subtitles)
            lines << QStringLiteral("  ") + detail;
    }

    QStringList genres = arrayStrings(info.value(QStringLiteral("genres")));
    if (!genres.isEmpty())
        lines << tr("Genres: %1").arg(genres.join(QStringLiteral(", ")));

    QString runtime = info.value(QStringLiteral("runtime")).toString().trimmed();
    if (runtime.isEmpty() && info.value(QStringLiteral("runtimeMinutes")).toInt() > 0)
        runtime = tr("%1 min").arg(info.value(QStringLiteral("runtimeMinutes")).toInt());
    if (!runtime.isEmpty())
        lines << tr("Runtime: %1").arg(runtime);

    const QString imdbRating = info.value(QStringLiteral("imdbRating")).toString().trimmed();
    if (!imdbRating.isEmpty())
        lines << tr("IMDb: %1").arg(imdbRating);

    const QStringList directors = arrayStrings(info.value(QStringLiteral("director")));
    if (!directors.isEmpty())
        lines << tr("Director: %1").arg(directors.join(QStringLiteral(", ")));

    const QStringList cast = arrayStrings(info.value(QStringLiteral("cast")));
    if (!cast.isEmpty())
        lines << tr("Cast: %1").arg(cast.mid(0, 6).join(QStringLiteral(", ")));

    if (lines.isEmpty()) {
        technicalInfoLabel_->clear();
        technicalInfoLabel_->hide();
        return;
    }

    technicalInfoLabel_->setText(lines.join(QLatin1Char('\n')));
    technicalInfoLabel_->show();
}

void TorrentDetailsPanel::updateTrackerInfoDisplay(const QJsonObject& info)
{
    trackerInfoWidget_->show();

    // Low-level announce URLs are transport plumbing, not useful media
    // information. Keep the widget permanently hidden even when older cached
    // records still contain trackerUrls.
    trackerUrlsLabel_->hide();
    trackerUrlsLabel_->clear();

    QStringList sources;
    auto appendSource = [&sources](QString source) {
        source = source.trimmed();
        if (source.isEmpty())
            return;

        const QString lower = source.toLower();
        if (lower == QStringLiteral("rutracker"))
            source = QStringLiteral("RuTracker");
        else if (lower == QStringLiteral("rutor"))
            source = QStringLiteral("Rutor");
        else if (lower == QStringLiteral("nyaa"))
            source = QStringLiteral("Nyaa");
        else if (lower == QStringLiteral("1337x"))
            source = QStringLiteral("1337x");

        for (const QString& existing : sources) {
            if (existing.compare(source, Qt::CaseInsensitive) == 0)
                return;
        }
        sources.append(source);
    };

    for (const QJsonValue& value : info.value(QStringLiteral("metadataSources")).toArray())
        appendSource(value.toString());
    for (const QJsonValue& value : info.value(QStringLiteral("trackers")).toArray())
        appendSource(value.toString());

    if (sources.isEmpty())
        appendSource(info.value(QStringLiteral("metadataSource")).toString());

    if (!sources.isEmpty()) {
        trackerInfoSourceLabel_->setText(tr("Sources: %1").arg(sources.join(QStringLiteral(" · "))));
        trackerInfoSourceLabel_->show();
    } else {
        trackerInfoSourceLabel_->hide();
    }

    updateTechnicalInfoDisplay(info);

    const QString posterUrl = info.value(QStringLiteral("poster")).toString().trimmed();
    if (!posterUrl.isEmpty())
        loadPosterImage(posterUrl);
    else
        posterLabel_->hide();

    // Keep exact-release text and generic movie synopsis separate. The former is
    // where tracker posts and Torrentio carry audio tracks/encode information;
    // the latter is useful context but must never make a sparse release look
    // fully resolved.
    QStringList sections;
    const QString releaseDetails = info.value(QStringLiteral("releaseDetails")).toString().trimmed();
    const QString trackerDescription = info.value(QStringLiteral("description")).toString().trimmed();
    const QString synopsis = info.value(QStringLiteral("synopsis")).toString().trimmed();

    if (!releaseDetails.isEmpty())
        sections << tr("RELEASE DETAILS\n%1").arg(releaseDetails);

    if (!trackerDescription.isEmpty() && trackerDescription != releaseDetails)
        sections << tr("RELEASE DESCRIPTION\n%1").arg(trackerDescription);

    if (!synopsis.isEmpty() && synopsis != trackerDescription && synopsis != releaseDetails)
        sections << tr("SYNOPSIS\n%1").arg(synopsis);

    if (sections.isEmpty()) {
        const QString note = info.value(QStringLiteral("metadataNote")).toString().trimmed();
        if (!note.isEmpty())
            sections << note;
    }

    const QString description = sections.join(QStringLiteral("\n\n"));
    if (!description.isEmpty()) {
        fullDescription_ = description;
        descriptionExpanded_ = false;

        if (description.length() > 420) {
            descriptionLabel_->setText(description.left(420) + QStringLiteral("..."));
            descriptionToggle_->setText(tr("Show more ▼"));
            descriptionToggle_->show();
        } else {
            descriptionLabel_->setText(description);
            descriptionToggle_->hide();
        }
        descriptionLabel_->show();
    } else {
        fullDescription_.clear();
        descriptionLabel_->clear();
        descriptionLabel_->hide();
        descriptionToggle_->hide();
    }

    // A synopsis is useful immediately, but if no exact-release technical data
    // has arrived yet, show that enrichment is still running rather than
    // claiming the lookup is complete.
    if (hasReleaseSpecificInfo(info)) {
        trackerInfoLoadingLabel_->hide();
        retryInfoButton_->hide();
    } else if (hasUserFacingInfo(info)) {
        trackerInfoLoadingLabel_->setText(tr("🔍 Looking for exact release audio/video details…"));
        trackerInfoLoadingLabel_->show();
        retryInfoButton_->hide();
    }

    rebuildMetadataLinks(info);

    const QString trackerCategory = info.value(QStringLiteral("contentCategory")).toString().trimmed();
    if (!trackerCategory.isEmpty())
        categoryLabel_->setText(trackerCategory);
}

void TorrentDetailsPanel::rebuildMetadataLinks(const QJsonObject& info)
{
    while (trackerLinksLayout_->count() > 1) {
        QLayoutItem* item = trackerLinksLayout_->takeAt(0);
        if (item->widget())
            delete item->widget();
        delete item;
    }

    bool hasLinks = false;
    QSet<QString> seenUrls;
    auto addLink = [this, &hasLinks, &seenUrls](const QString& label, const QString& url) {
        const QString trimmed = url.trimmed();
        if (trimmed.isEmpty() || seenUrls.contains(trimmed))
            return;
        seenUrls.insert(trimmed);

        QPushButton* button = new QPushButton(QStringLiteral("🔗 ") + label);
        button->setObjectName("trackerLinkButton");
        button->setCursor(Qt::PointingHandCursor);
        button->setToolTip(trimmed);
        connect(button, &QPushButton::clicked, this, [trimmed]() { QDesktopServices::openUrl(QUrl(trimmed)); });
        trackerLinksLayout_->insertWidget(trackerLinksLayout_->count() - 1, button);
        hasLinks = true;
    };

    const int rutrackerId = info.value(QStringLiteral("rutrackerThreadId")).toInt();
    if (rutrackerId > 0)
        addLink(QStringLiteral("RuTracker"),
            QStringLiteral("https://rutracker.org/forum/viewtopic.php?t=%1").arg(rutrackerId));

    const int nyaaId = info.value(QStringLiteral("nyaaThreadId")).toInt();
    if (nyaaId > 0)
        addLink(QStringLiteral("Nyaa"), QStringLiteral("https://nyaa.si/view/%1").arg(nyaaId));

    addLink(QStringLiteral("Rutor"), info.value(QStringLiteral("rutorUrl")).toString());
    addLink(QStringLiteral("1337x"), info.value(QStringLiteral("x1337Url")).toString());
    addLink(QStringLiteral("YTS"), info.value(QStringLiteral("ytsUrl")).toString());
    addLink(QStringLiteral("Wikipedia"), info.value(QStringLiteral("wikipediaUrl")).toString());

    const QString imdbId = info.value(QStringLiteral("imdbId")).toString().trimmed();
    if (imdbId.startsWith(QStringLiteral("tt")))
        addLink(QStringLiteral("IMDb"), QStringLiteral("https://www.imdb.com/title/%1/").arg(imdbId));

    addLink(QStringLiteral("Magnetz"), info.value(QStringLiteral("magnetzUrl")).toString());

    // Guarantee a useful escape hatch for every selected torrent. If the exact
    // release could not be enriched, offer searches on sites that normally have
    // full release cards (description, video/audio, translation, subtitles).
    if (!hasReleaseSpecificInfo(info)) {
        QString query = rats::net::metadata::cleanMediaTitle(currentTorrent_.name);
        const int year = rats::net::metadata::extractYear(currentTorrent_.name);
        if (year > 0)
            query += QStringLiteral(" ") + QString::number(year);
        if (query.trimmed().isEmpty())
            query = currentTorrent_.name.trimmed();

        if (!currentHash_.isEmpty()) {
            addLink(tr("RuTracker exact hash"),
                QStringLiteral("https://rutracker.org/forum/viewtopic.php?h=%1").arg(currentHash_));
            addLink(tr("1337x exact hash"),
                QStringLiteral("https://1337x.to/srch?search=%1")
                    .arg(QString::fromLatin1(QUrl::toPercentEncoding(currentHash_))));
        }

        if (!query.isEmpty()) {
            const QString encoded = QString::fromLatin1(QUrl::toPercentEncoding(query));
            addLink(tr("Find description on Rutor"), QStringLiteral("https://rutor.info/search/%1").arg(encoded));
            addLink(tr("Find description on RuTracker"),
                QStringLiteral("https://rutracker.org/forum/tracker.php?nm=%1").arg(encoded));

            const bool cyrillic = query.contains(QRegularExpression(QStringLiteral("[\\x{0400}-\\x{04FF}]")));
            const QString wikiHost = cyrillic ? QStringLiteral("ru.wikipedia.org") : QStringLiteral("en.wikipedia.org");
            addLink(tr("Find movie info"),
                QStringLiteral("https://%1/w/index.php?search=%2").arg(wikiHost, encoded));
        }
    }

    trackerLinksWidget_->setVisible(hasLinks);
}

void TorrentDetailsPanel::loadPosterImage(const QString& url)
{
    if (url.isEmpty()) {
        posterLabel_->hide();
        return;
    }

    posterLabel_->setText(tr("Loading image..."));
    posterLabel_->show();

    QUrl imageUrl(url);
    QNetworkRequest request { imageUrl };
    request.setHeader(QNetworkRequest::UserAgentHeader,
        QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"));
    request.setTransferTimeout(15000);

    QNetworkReply* reply = posterNetworkManager_->get(request);
    QString hash = currentHash_;

    connect(reply, &QNetworkReply::finished, this, [this, reply, hash]() {
        reply->deleteLater();

        // Only update if still showing this torrent
        if (currentHash_ != hash)
            return;

        if (reply->error() != QNetworkReply::NoError) {
            posterLabel_->hide();
            return;
        }

        QByteArray imageData = reply->readAll();
        QPixmap pixmap;
        if (pixmap.loadFromData(imageData)) {
            // Scale to fit panel width, max 300px height
            int maxWidth = this->width() - 40;
            if (maxWidth < 100)
                maxWidth = 250;

            if (pixmap.width() > maxWidth || pixmap.height() > 300) {
                pixmap = pixmap.scaled(maxWidth, 300, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            }
            posterLabel_->setPixmap(pixmap);
            posterLabel_->show();
        } else {
            posterLabel_->hide();
        }
    });
}
