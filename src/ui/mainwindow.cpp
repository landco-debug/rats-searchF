#include "mainwindow.h"
#include "searchresultmodel.h"
#include "theme.h"
#include "torrentdetailspanel.h"
#include "torrentitemdelegate.h"
#include "torrentmenu.h"
#include "version.h"

// Tab widgets
#include "activitywidget.h"
#include "downloadswidget.h"
#include "favoriteswidget.h"
#include "feedwidget.h"
#include "toptorrentswidget.h"
#include "torrentfileswidget.h"

// Settings dialog
#include "settingsdialog.h"

// Layered application + services
#include "app/application.h"
#include "app/config_store.h"
#include "app/favorites_store.h"
#include "app/search_history_store.h"
#include "app/translation_manager.h"
#include "common/result.h"
#include "data/torrent_repository.h"
#include "domain/content.h"
#include "domain/torrent_codec.h"
#include "format.h"
#include "net/crawler.h"
#include "net/p2p_transport.h"
#include "net/rutor_search_client.h"
#include "net/megapeer_search_client.h"
#include "net/rutracker_ru_search_client.h"
#include "net/torrent_engine.h"
#include "peer/peer_api.h"
#include "rest/api_router.h"
#include "services/database_sync_service.h"
#include "services/download_service.h"
#include "services/indexing_service.h"
#include "services/migration_service.h"
#include "services/peer_registry.h"
#include "services/search_service.h"
#include "services/torrent_creator.h"
#include "services/torrent_exporter.h"
#include "services/update_service.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QCompleter>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFocusEvent>
#include <QFontMetrics>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QSettings>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStringListModel>
#include <QStyle>
#include <QSysInfo>
#include <QSystemTrayIcon>
#include <QTabWidget>
#include <QTabBar>
#include <QTableView>
#include <QTemporaryFile>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <QWidgetAction>

using rats::Result;
using rats::domain::SearchHit;
using rats::domain::Torrent;
using rats::service::SearchService;
using rats::service::UpdateService;
namespace codec = rats::domain::codec;

namespace {
class SearchHeaderView final : public QHeaderView {
public:
    explicit SearchHeaderView(QWidget* parent) : QHeaderView(Qt::Horizontal, parent) {}
protected:
    void paintSection(QPainter* painter, const QRect& rect, int logicalIndex) const override
    {
        QHeaderView::paintSection(painter, rect, logicalIndex);
        if (logicalIndex != SearchResultModel::SeedersColumn
            && logicalIndex != SearchResultModel::LeechersColumn)
            return;
        const bool seeders = logicalIndex == SearchResultModel::SeedersColumn;
        const QColor color = rats::ui::Theme::instance().color(
            seeders ? QLatin1String("success") : QLatin1String("danger"));
        QRect textRect = rect.adjusted(3, 0, -3, 0);
        if (sortIndicatorSection() == logicalIndex)
            textRect.adjust(0, 0, -12, 0);
        painter->save();
        painter->setPen(color);
        QFont font = painter->font();
        font.setBold(true);
        painter->setFont(font);
        painter->drawText(textRect, Qt::AlignCenter,
            seeders ? QStringLiteral("S") : QStringLiteral("L"));
        painter->restore();
    }
};
} // namespace

MainWindow::MainWindow(rats::app::Application* app, QWidget* parent)
    : QMainWindow(parent), app_(app), trayIcon(nullptr), trayMenu(nullptr)
{
    // The Application is already started by main(); MainWindow only wires UI.
    rats::app::ConfigStore* config = app_->config();

    // Check if user has accepted the agreement before showing the main UI.
    if (config && !config->agreementAccepted()) {
        if (!showAgreementDialog()) {
            // User declined - schedule application exit (main() owns app_->stop()).
            QTimer::singleShot(0, qApp, &QApplication::quit);
            return;
        }
    }

    setWindowTitle(tr("Rats Search %1 - BitTorrent P2P Search Engine").arg(RATSSEARCH_VERSION_STRING));
    // Default only. Persisted geometry is restored after the widgets exist, so
    // it is not immediately overwritten by this resize.
    resize(1400, 900);
    setWindowIcon(QIcon(":/images/icon.png"));
    setAcceptDrops(true); // drag & drop .torrent files

    applyTheme(config ? config->darkMode() : false);
    setupUi();
    setupMenuBar();
    setupStatusBar();
    setupSystemTray();
    fileMetadataNetwork_ = new QNetworkAccessManager(this);

    // On macOS, clicking the Dock icon activates the application but does not
    // necessarily route through QSystemTrayIcon. If this window was hidden by
    // our minimize/close-to-tray path, restore it explicitly on app activation.
    connect(qApp, &QGuiApplication::applicationStateChanged, this,
        [this](Qt::ApplicationState state) {
            if (state == Qt::ApplicationActive && hiddenToTray_)
                QTimer::singleShot(0, this, &MainWindow::bringToFront);
        });

    // Restore UI state only after every referenced widget/header/splitter exists.
    // The old code restored before setupUi() and then immediately called
    // resize(1400, 900), which discarded most of the persisted state.
    loadSettings();

    // Hand the running application to every tab/panel, then wire the signals.
    wireWidgets();
    connectSignals();

    // Seed the status bar from live state. The transport starts before the window
    // exists (and the agreement dialog above spins a nested event loop), so peers
    // may already be connected: peerCountChanged only fires on a change, and every
    // emit before connectSignals() is lost.
    if (app_->torrents()) {
        cachedTorrentCount_ = app_->torrents()->statistics().torrents;
    }
    if (app_->peers()) {
        cachedRemoteTorrentCount_ = app_->peers()->remoteTorrentsCount();
    }
    if (app_->transport()) {
        onPeerCountChanged(app_->transport()->peerCount());
    }
    updateStatusBar();
    refreshP2PStatus();
    updateNetworkStatus();

}

MainWindow::~MainWindow()
{
    saveSettings();
    // Service lifecycle is owned by main()/Application — nothing to stop here.
}

void MainWindow::applyTheme(bool darkMode)
{
    rats::ui::Theme::instance().setDark(darkMode);
    setStyleSheet(rats::ui::Theme::instance().styleSheet());
}

void MainWindow::setupUi()
{
    QWidget* centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);

    QVBoxLayout* mainLayout = new QVBoxLayout(centralWidget);
    mainLayout->setContentsMargins(16, 16, 16, 16);
    mainLayout->setSpacing(12);

    // Search bar section
    QWidget* searchSection = new QWidget();
    searchSection->setObjectName("searchSection");
    QHBoxLayout* searchLayout = new QHBoxLayout(searchSection);
    searchLayout->setContentsMargins(12, 8, 12, 8);
    searchLayout->setSpacing(12);

    QLabel* logoLabel = new QLabel("🐀");
    logoLabel->setObjectName("logoLabel");
    searchLayout->addWidget(logoLabel);

    QLabel* titleLabel = new QLabel("Rats Search");
    titleLabel->setObjectName("titleLabel");
    searchLayout->addWidget(titleLabel);

    searchLayout->addSpacing(20);

    searchLineEdit = new QLineEdit(this);
    searchLineEdit->setPlaceholderText(tr("Search for torrents..."));
    searchLineEdit->setMinimumHeight(44);
    searchLineEdit->setMinimumWidth(400);
    QFont searchFont = searchLineEdit->font();
    searchFont.setPointSize(12);
    searchLineEdit->setFont(searchFont);

    searchButton = new QPushButton(tr("Search"), this);
    searchButton->setMinimumSize(120, 44);
    searchButton->setDefault(true);
    searchButton->setCursor(Qt::PointingHandCursor);

    // Content-type filter. The data strings are the canonical wire names from
    // domain::ContentType, passed straight through to the repository filter.
    typeComboBox = new QComboBox(this);
    typeComboBox->addItem(tr("All types"), "");
    typeComboBox->addItem(tr("Video"), "video");
    typeComboBox->addItem(tr("Audio"), "audio");
    typeComboBox->addItem(tr("Games"), "games");
    typeComboBox->addItem(tr("Software"), "software");
    typeComboBox->addItem(tr("Books"), "books");
    typeComboBox->addItem(tr("Pictures"), "pictures");
    typeComboBox->addItem(tr("Archives"), "archive");
    typeComboBox->setMinimumHeight(44);
    typeComboBox->setToolTip(tr("Filter results by content type"));

    safeSearchCheckBox = new QCheckBox(tr("Safe"), this);
    safeSearchCheckBox->setToolTip(tr("Safe search: hide adult content"));
    safeSearchCheckBox->setCursor(Qt::PointingHandCursor);
    if (app_ && app_->config())
        safeSearchCheckBox->setChecked(app_->config()->safeSearch());

    sortComboBox = new QComboBox(this);
    sortComboBox->addItem(tr("Sort: Seeders ↓"), "seeders_desc");
    sortComboBox->addItem(tr("Sort: Seeders ↑"), "seeders_asc");
    sortComboBox->addItem(tr("Sort: Size ↓"), "size_desc");
    sortComboBox->addItem(tr("Sort: Size ↑"), "size_asc");
    sortComboBox->addItem(tr("Sort: Date ↓"), "added_desc");
    sortComboBox->addItem(tr("Sort: Date ↑"), "added_asc");
    sortComboBox->addItem(tr("Sort: Name A-Z"), "name_asc");
    sortComboBox->addItem(tr("Sort: Name Z-A"), "name_desc");
    sortComboBox->setMinimumHeight(44);

    setupSearchHistory();

    setupSearchFilters();

    searchLayout->addWidget(searchLineEdit, 1);
    searchLayout->addWidget(typeComboBox);
    searchLayout->addWidget(filtersButton);
    searchLayout->addWidget(safeSearchCheckBox);
    searchLayout->addWidget(sortComboBox);
    searchLayout->addWidget(searchButton);

    mainLayout->addWidget(searchSection);

    // Vertical splitter: content area on top, files panel at bottom
    verticalSplitter = new QSplitter(Qt::Vertical, this);
    verticalSplitter->setHandleWidth(3);

    // Horizontal splitter for tabs + details panel
    mainSplitter = new QSplitter(Qt::Horizontal, this);
    mainSplitter->setHandleWidth(2);

    tabWidget = new QTabWidget(this);
    tabWidget->setDocumentMode(false);
    // Never turn useful tab names into "Search Res..." / "Downlo...".
    // Let every tab use its natural text width; if the details panel leaves too
    // little room, Qt's native scroll buttons are preferable to ambiguous labels.
    tabWidget->tabBar()->setElideMode(Qt::ElideNone);
    tabWidget->tabBar()->setUsesScrollButtons(true);
    tabWidget->tabBar()->setExpanding(false);

    // Search results tab
    QWidget* searchTab = new QWidget();
    QVBoxLayout* searchTabLayout = new QVBoxLayout(searchTab);
    searchTabLayout->setContentsMargins(0, 8, 0, 0);

    resultsTableView = new QTableView(this);
    searchResultModel = new SearchResultModel(this);
    torrentDelegate = new TorrentItemDelegate(this);

    resultsTableView->setModel(searchResultModel);
    resultsTableView->setItemDelegate(torrentDelegate);
    resultsTableView->setHorizontalHeader(new SearchHeaderView(resultsTableView));
    resultsTableView->setSelectionBehavior(QAbstractItemView::SelectRows);
    resultsTableView->setSelectionMode(QAbstractItemView::SingleSelection);
    resultsTableView->setAlternatingRowColors(true);
    resultsTableView->setSortingEnabled(true);
    resultsTableView->horizontalHeader()->setStretchLastSection(false);
    resultsTableView->verticalHeader()->setVisible(false);
    resultsTableView->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    resultsTableView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    resultsTableView->setShowGrid(false);
    resultsTableView->setContextMenuPolicy(Qt::CustomContextMenu);
    resultsTableView->setMouseTracking(true);

    // Keep the release name wide. S/L are compact Latin torrent shorthand,
    // drawn in conventional green/red; Date contains no time component.
    resultsTableView->horizontalHeader()->setSectionResizeMode(
        SearchResultModel::NameColumn, QHeaderView::Stretch);
    resultsTableView->horizontalHeader()->setSectionResizeMode(
        SearchResultModel::SizeColumn, QHeaderView::Fixed);
    resultsTableView->horizontalHeader()->setSectionResizeMode(
        SearchResultModel::SeedersColumn, QHeaderView::Fixed);
    resultsTableView->horizontalHeader()->setSectionResizeMode(
        SearchResultModel::LeechersColumn, QHeaderView::Fixed);
    resultsTableView->horizontalHeader()->setSectionResizeMode(
        SearchResultModel::DateColumn, QHeaderView::Fixed);
    resultsTableView->setColumnWidth(SearchResultModel::SizeColumn, 92);
    resultsTableView->setColumnWidth(SearchResultModel::SeedersColumn, 42);
    resultsTableView->setColumnWidth(SearchResultModel::LeechersColumn, 42);
    resultsTableView->setColumnWidth(SearchResultModel::DateColumn, 96);

    searchTabLayout->addWidget(resultsTableView);
    tabWidget->addTab(searchTab, tr("Search Results"));

    topTorrentsWidget = new TopTorrentsWidget(this);
    tabWidget->addTab(topTorrentsWidget, tr("🔥 Top"));

    feedWidget = new FeedWidget(this);
    tabWidget->addTab(feedWidget, tr("📰 Feed"));

    downloadsWidget = new DownloadsWidget(this);
    tabWidget->addTab(downloadsWidget, tr("📥 Downloads"));

    activityWidget = new ActivityWidget(this);
    tabWidget->addTab(activityWidget, tr("⚡ Activity"));

    favoritesWidget = new FavoritesWidget(this);
    tabWidget->addTab(favoritesWidget, tr("⭐ Favorites"));

    mainSplitter->addWidget(tabWidget);

    // Right side - Details panel
    detailsPanel = new TorrentDetailsPanel(this);
    detailsPanel->setMinimumWidth(280);
    detailsPanel->setMinimumHeight(150);
    detailsPanel->hide();

    mainSplitter->addWidget(detailsPanel);
    mainSplitter->setSizes({ 900, 350 });

    verticalSplitter->addWidget(mainSplitter);

    // Bottom panel - Files widget
    filesWidget = new TorrentFilesWidget(this);
    filesWidget->setMinimumHeight(120);
    filesWidget->hide();
    verticalSplitter->addWidget(filesWidget);

    mainLayout->addWidget(verticalSplitter, 1);
}

void MainWindow::setupMenuBar()
{
    QMenu* fileMenu = menuBar()->addMenu(tr("&File"));

    QAction* addTorrentAction = fileMenu->addAction(tr("📥 &Add Torrent..."));
    addTorrentAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_O));
    addTorrentAction->setToolTip(tr("Add a .torrent file to the search index"));
    connect(addTorrentAction, &QAction::triggered, this, &MainWindow::addTorrentFile);

    QAction* createTorrentAction = fileMenu->addAction(tr("🔨 &Create Torrent..."));
    createTorrentAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N));
    createTorrentAction->setToolTip(tr("Create a torrent from a file or folder and start seeding"));
    connect(createTorrentAction, &QAction::triggered, this, &MainWindow::createTorrent);

    fileMenu->addSeparator();

    QAction* exportDbAction = fileMenu->addAction(tr("📦 &Export Database..."));
    exportDbAction->setToolTip(tr("Write the whole search index to a file you can share"));
    connect(exportDbAction, &QAction::triggered, this, &MainWindow::exportDatabase);

    QAction* importDbAction = fileMenu->addAction(tr("📂 &Import Database..."));
    importDbAction->setToolTip(tr("Merge a database file from another user into your index"));
    connect(importDbAction, &QAction::triggered, this, &MainWindow::importDatabase);

    QAction* pullDbAction = fileMenu->addAction(tr("🌐 Download Database from &Peer..."));
    pullDbAction->setToolTip(tr("Ask a connected peer to send you its whole index"));
    connect(pullDbAction, &QAction::triggered, this, &MainWindow::pullDatabaseFromPeer);

    fileMenu->addSeparator();

    QAction* settingsAction = fileMenu->addAction(tr("&Settings"));
    connect(settingsAction, &QAction::triggered, this, &MainWindow::showSettings);

    fileMenu->addSeparator();

    QAction* quitAction = fileMenu->addAction(tr("&Quit"));
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, this, [this]() {
        forceQuit_ = true;
        close();
    });

    QMenu* helpMenu = menuBar()->addMenu(tr("&Help"));

    QAction* changelogAction = helpMenu->addAction(tr("📋 &Changelog"));
    connect(changelogAction, &QAction::triggered, this, &MainWindow::showChangelog);

    helpMenu->addSeparator();

    QAction* reportBugAction = helpMenu->addAction(tr("🐛 &Report a Bug..."));
    reportBugAction->setToolTip(tr("Open a bug report on GitHub"));
    connect(reportBugAction, &QAction::triggered, this, &MainWindow::reportBug);

    QAction* featureAction = helpMenu->addAction(tr("💡 Request a &Feature..."));
    featureAction->setToolTip(tr("Suggest a new feature on GitHub"));
    connect(featureAction, &QAction::triggered, this, &MainWindow::requestFeature);

    helpMenu->addSeparator();

    QAction* aboutAction = helpMenu->addAction(tr("&About"));
    connect(aboutAction, &QAction::triggered, this, &MainWindow::showAbout);
}

void MainWindow::setupStatusBar()
{
    p2pStatusLabel = new QLabel();
    p2pStatusLabel->setTextFormat(Qt::RichText);
    p2pState_ = P2PState::NotStarted;
    paintP2PIndicator();

    peerCountLabel = new QLabel(tr("👥 Peers: %1").arg(0));
    dhtNodeCountLabel = new QLabel(tr("🌐 DHT: %1").arg(0));
    torrentCountLabel = new QLabel(tr("📦 Torrents: %1").arg(0));
    spiderStatusLabel = new QLabel();
    refreshSpiderStatus();

    // Transient status text gets its own slot at the right end of the bar. It is
    // free to shrink (Ignored policy + elision) so a long torrent name never
    // pushes the counters around, and nothing here ever calls
    // QStatusBar::showMessage(), which would hide the counters instead.
    statusMessageLabel = new QLabel();
    statusMessageLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    statusMessageLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    statusMessageTimer_ = new QTimer(this);
    statusMessageTimer_->setSingleShot(true);
    connect(statusMessageTimer_, &QTimer::timeout, this, &MainWindow::clearStatusMessage);

    statusBar()->addWidget(p2pStatusLabel);
    statusBar()->addWidget(peerCountLabel);
    statusBar()->addWidget(dhtNodeCountLabel);
    statusBar()->addWidget(torrentCountLabel);
    statusBar()->addWidget(spiderStatusLabel);
    statusBar()->addWidget(statusMessageLabel, 1);

    // Periodic network status refresh (DHT node count etc.)
    statusUpdateTimer_ = new QTimer(this);
    connect(statusUpdateTimer_, &QTimer::timeout, this, &MainWindow::updateNetworkStatus);
    statusUpdateTimer_->start(30000);
}

void MainWindow::showStatusMessage(const QString& message, int timeoutMs)
{
    if (!statusMessageLabel)
        return;

    statusMessageText_ = message;
    statusMessageLabel->setToolTip(message);
    updateStatusMessageElide();

    if (timeoutMs > 0)
        statusMessageTimer_->start(timeoutMs);
    else
        statusMessageTimer_->stop();
}

void MainWindow::clearStatusMessage()
{
    statusMessageText_.clear();
    if (statusMessageLabel) {
        statusMessageLabel->clear();
        statusMessageLabel->setToolTip(QString());
    }
}

void MainWindow::updateStatusMessageElide()
{
    if (!statusMessageLabel)
        return;

    const int width = statusMessageLabel->width();
    if (width <= 0 || statusMessageText_.isEmpty()) {
        statusMessageLabel->setText(statusMessageText_);
        return;
    }
    statusMessageLabel->setText(
        statusMessageLabel->fontMetrics().elidedText(statusMessageText_, Qt::ElideRight, width));
}

void MainWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);
    // The message slot shrinks with the window — re-elide so the counters to its
    // left keep their space.
    updateStatusMessageElide();
}

void MainWindow::wireWidgets()
{
    // Every tab/panel reaches its services through the shared Application.
    if (topTorrentsWidget)
        topTorrentsWidget->setApplication(app_);
    if (feedWidget)
        feedWidget->setApplication(app_);
    if (downloadsWidget)
        downloadsWidget->setApplication(app_);
    if (activityWidget)
        activityWidget->setApplication(app_);
    if (favoritesWidget)
        favoritesWidget->setApplication(app_);
    if (detailsPanel)
        detailsPanel->setApplication(app_);
    if (filesWidget)
        filesWidget->setApplication(app_);
}

void MainWindow::connectSignals()
{
    connectSearchSignals();
    connectTabSignals();
    connectDetailsSignals();
    connectServiceSignals();
    connectPeerSignals();

    // Immediate settings application.
    // Language changes are handled by Application, which owns the translators.
    if (app_->config())
        connect(app_->config(), &rats::app::ConfigStore::darkModeChanged, this, &MainWindow::onDarkModeChanged);
}

void MainWindow::connectSearchSignals()
{
    connect(searchButton, &QPushButton::clicked, this, &MainWindow::onSearchButtonClicked);
    connect(searchLineEdit, &QLineEdit::returnPressed, this, &MainWindow::onSearchButtonClicked);
    connect(searchLineEdit, &QLineEdit::textChanged, this, &MainWindow::onSearchTextChanged);
    connect(searchLineEdit, &QWidget::customContextMenuRequested, this, &MainWindow::showSearchContextMenu);
    if (app_ && app_->searchHistory()) {
        connect(app_->searchHistory(), &rats::app::SearchHistoryStore::historyChanged, this,
            &MainWindow::refreshSearchHistory);
    }
    connect(sortComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onSortOrderChanged);

    // Changing a filter re-runs the current query so results update in place.
    connect(typeComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        QSettings settings(QStringLiteral("RatsSearch"), QStringLiteral("RatsSearch"));
        settings.setValue(QStringLiteral("search/contentType"),
            typeComboBox->currentData().toString());
        settings.sync();
        if (!currentSearchQuery_.isEmpty())
            performSearch(currentSearchQuery_);
    });
    // Adjusting a range re-runs the query when the popup closes, not on every
    // keystroke inside it — a spin box passes through a dozen values on the way
    // to the one the user means.
    connect(filtersMenu, &QMenu::aboutToShow, this, [this]() { filtersOnOpen_ = currentSearchFilters(); });
    connect(filtersMenu, &QMenu::aboutToHide, this, [this]() {
        if (currentSearchFilters() == filtersOnOpen_ || currentSearchQuery_.isEmpty())
            return;
        performSearch(currentSearchQuery_);
    });
    connect(safeSearchCheckBox, &QCheckBox::toggled, this, [this](bool checked) {
        if (app_ && app_->config())
            app_->config()->setSafeSearch(checked);
        if (!currentSearchQuery_.isEmpty())
            performSearch(currentSearchQuery_);
    });

    connect(resultsTableView->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
        [this](const QModelIndex& current, const QModelIndex&) { onTorrentSelected(current); });
    connect(resultsTableView, &QTableView::doubleClicked, this, &MainWindow::onTorrentDoubleClicked);
    connect(resultsTableView, &QTableView::customContextMenuRequested, this, &MainWindow::showTorrentContextMenu);
}

void MainWindow::connectTabSignals()
{
    connect(tabWidget, &QTabWidget::currentChanged, this, &MainWindow::onTabChanged);

    // Every torrent-table / activity tab exposes the same three signals (payload
    // is rats::domain::Torrent). Selecting shows the details panel and remembers
    // the selection per tab so switching back can restore it.

    // Top torrents
    connect(topTorrentsWidget, &TopTorrentsWidget::torrentSelected, this, [this](const Torrent& t) {
        tabSelection_[topTorrentsWidget] = t;
        showTorrentDetails(t);
    });
    connect(topTorrentsWidget, &TopTorrentsWidget::torrentDoubleClicked, this, &MainWindow::openMagnetLink);
    connect(topTorrentsWidget, &TopTorrentsWidget::exportTorrentRequested, this, &MainWindow::exportTorrentToFile);

    // Feed
    connect(feedWidget, &FeedWidget::torrentSelected, this, [this](const Torrent& t) {
        tabSelection_[feedWidget] = t;
        showTorrentDetails(t);
    });
    connect(feedWidget, &FeedWidget::torrentDoubleClicked, this, &MainWindow::openMagnetLink);
    connect(feedWidget, &FeedWidget::exportTorrentRequested, this, &MainWindow::exportTorrentToFile);

    // Activity
    connect(activityWidget, &ActivityWidget::torrentSelected, this, [this](const Torrent& t) {
        tabSelection_[activityWidget] = t;
        showTorrentDetails(t);
    });
    connect(activityWidget, &ActivityWidget::torrentDoubleClicked, this, &MainWindow::openMagnetLink);
    connect(activityWidget, &ActivityWidget::exportTorrentRequested, this, &MainWindow::exportTorrentToFile);

    // Favorites
    connect(favoritesWidget, &FavoritesWidget::torrentSelected, this, [this](const Torrent& t) {
        tabSelection_[favoritesWidget] = t;
        showTorrentDetails(t);
    });
    connect(favoritesWidget, &FavoritesWidget::torrentDoubleClicked, this, &MainWindow::openMagnetLink);
    connect(favoritesWidget, &FavoritesWidget::exportTorrentRequested, this, &MainWindow::exportTorrentToFile);

    // Activity tab: "Top" shortcut button switches to the Top tab.
    connect(activityWidget, &ActivityWidget::navigateToTop, this,
        [this]() { tabWidget->setCurrentWidget(topTorrentsWidget); });
}

void MainWindow::connectDetailsSignals()
{
    connect(detailsPanel, &TorrentDetailsPanel::closeRequested, this, &MainWindow::onDetailsPanelCloseRequested);
    connect(detailsPanel, &TorrentDetailsPanel::downloadRequested, this, &MainWindow::onDownloadRequested);
    connect(detailsPanel, &TorrentDetailsPanel::goToDownloadsRequested, this,
        [this]() { tabWidget->setCurrentWidget(downloadsWidget); });
    connect(detailsPanel, &TorrentDetailsPanel::downloadCancelRequested, this, [this](const QString& hash) {
        if (app_->downloads()) {
            app_->downloads()->remove(hash, /*saveResumeData*/ false);
            showStatusMessage(tr("Download cancelled"), 2000);
        }
    });
}

void MainWindow::connectServiceSignals()
{
    // Transport: peer count + P2P indicator state.
    if (app_->transport()) {
        auto* transport = app_->transport();
        connect(transport, &rats::net::P2PTransport::peerCountChanged, this, &MainWindow::onPeerCountChanged);
        connect(transport, &rats::net::P2PTransport::started, this, [this]() { refreshP2PStatus(); });
        connect(transport, &rats::net::P2PTransport::stopped, this, [this]() { refreshP2PStatus(); });
        connect(
            transport, &rats::net::P2PTransport::peerConnected, this, [this](const QString&) { refreshP2PStatus(); });
        connect(transport, &rats::net::P2PTransport::peerDisconnected, this, [this](const QString&) {
            if (app_->peers())
                cachedRemoteTorrentCount_ = app_->peers()->remoteTorrentsCount();
            updateStatusBar();
            refreshP2PStatus();
        });
    }

    // Peer registry: swarm-wide torrent totals.
    if (app_->peers()) {
        connect(app_->peers(), &rats::service::PeerRegistry::peerStatsReceived, this,
            [this](const QString&, const rats::domain::PeerStats&) {
                cachedRemoteTorrentCount_ = app_->peers()->remoteTorrentsCount();
                updateStatusBar();
            });
    }

    // Repository statistics drive the local torrent count authoritatively.
    if (app_->torrents()) {
        connect(app_->torrents(), &rats::data::TorrentRepository::statisticsChanged, this,
            [this](qint64 torrents, qint64, qint64) {
                cachedTorrentCount_ = torrents;
                updateStatusBar();
            });
    }

    // Crawler status text.
    if (app_->crawler()) {
        connect(app_->crawler(), &rats::net::Crawler::statusChanged, this, &MainWindow::refreshSpiderStatus);
    }

    // New torrents indexed (from any source) update the status message.
    if (app_->indexing()) {
        connect(app_->indexing(), &rats::service::IndexingService::torrentIndexed, this, &MainWindow::onTorrentIndexed);
    }

    // Update service: surface availability / progress / errors.
    if (app_->updates()) {
        auto* updates = app_->updates();
        connect(updates, &UpdateService::updateAvailable, this,
            [this](const UpdateService::UpdateInfo& info) { onUpdateAvailable(info.version, info.releaseNotes); });
        connect(updates, &UpdateService::downloadProgressChanged, this, &MainWindow::onUpdateDownloadProgress);
        connect(updates, &UpdateService::updateReady, this, &MainWindow::onUpdateReady);
        connect(updates, &UpdateService::errorOccurred, this, &MainWindow::onUpdateError);
    }

    // Download progress mirrored into the details panel if it is showing that
    // torrent.
    if (app_->downloads()) {
        auto* downloads = app_->downloads();
        connect(downloads, &rats::service::DownloadService::progressUpdated, this,
            [this](const QString& hash, const QJsonObject& progress) {
                if (detailsPanel && detailsPanel->currentHash() == hash) {
                    qint64 downloaded = progress["downloaded"].toVariant().toLongLong();
                    // progressJson emits the full size under "total" (there is no
                    // "size" key) — reading "size" always yielded 0, so the panel
                    // showed "90 MB / 0" while downloading.
                    qint64 total = progress["total"].toVariant().toLongLong();
                    double progressVal = progress["progress"].toDouble();
                    int speed = static_cast<int>(progress["downloadSpeed"].toDouble());
                    detailsPanel->setDownloadProgress(progressVal, downloaded, total, speed);
                }
            });
        connect(downloads, &rats::service::DownloadService::downloadCompleted, this, [this](const QString& hash) {
            if (detailsPanel && detailsPanel->currentHash() == hash)
                detailsPanel->setDownloadCompleted();
        });
        connect(downloads, &rats::service::DownloadService::torrentRemoved, this, [this](const QString& hash) {
            if (detailsPanel && detailsPanel->currentHash() == hash)
                detailsPanel->resetDownloadState();
        });
    }

    // Torrent export: fetch runs in the service; here we drive the save dialog,
    // surface failures, and mirror progress into the status bar.
    if (app_->exporter()) {
        auto* exporter = app_->exporter();
        connect(exporter, &rats::service::TorrentExporter::exportReady, this, &MainWindow::onExportReady);
        connect(exporter, &rats::service::TorrentExporter::exportFailed, this,
            [this](const QString&, const QString& reason) {
                QMessageBox::warning(this, tr("Export Torrent"), tr("Could not export the torrent.\n\n%1").arg(reason));
            });
        connect(exporter, &rats::service::TorrentExporter::statusMessage, this,
            [this](const QString& message, int timeoutMs) { showStatusMessage(message, timeoutMs); });
    }

    // Whole-database export/import/peer transfer. The service does the work on a
    // worker thread and reports the same JSON payload the REST API pushes; here
    // it becomes a status-bar line and a final message box.
    //
    // Only the *local* lane may interrupt: syncFinished belongs to the operation
    // this user started. Serving another peer reports through serveProgress /
    // serveFinished and stays in the status bar — a peer that gave up on our
    // upload is not this user's problem, and putting that in a modal dialog is
    // what the "database sync did not finish" reports were about.
    if (app_->databaseSync()) {
        auto* sync = app_->databaseSync();
        connect(sync, &rats::service::DatabaseSyncService::syncProgress, this,
            [this](const QJsonObject& info) { showStatusMessage(databaseSyncStatusText(info), 0); });
        connect(sync, &rats::service::DatabaseSyncService::statusMessage, this,
            [this](const QString& message, int timeoutMs) { showStatusMessage(message, timeoutMs); });
        connect(sync, &rats::service::DatabaseSyncService::serveProgress, this, [this](const QJsonObject& info) {
            const QString text = databaseServeStatusText(info);
            if (!text.isEmpty())
                showStatusMessage(text, 0);
        });
        connect(sync, &rats::service::DatabaseSyncService::serveFinished, this,
            [](const QString& peerId, bool success, const QJsonObject& summary) {
                // Deliberately log-only. See the note above.
                qInfo() << "[MainWindow] database serve to" << peerId.left(8) << (success ? "completed" : "ended:")
                        << summary["reason"].toString();
            });
        connect(sync, &rats::service::DatabaseSyncService::syncFinished, this,
            [this](bool success, const QJsonObject& summary) {
                if (!success) {
                    showStatusMessage(tr("Database sync failed."), 5000);
                    QMessageBox::warning(this, tr("Database Sync"),
                        tr("The database sync did not finish.\n\n%1").arg(summary["error"].toString()));
                    return;
                }

                const QString operation = summary["operation"].toString();
                const qint64 processed = summary["processed"].toVariant().toLongLong();
                if (operation == QLatin1String("export")) {
                    showStatusMessage(tr("Exported %n torrent(s).", nullptr, static_cast<int>(processed)), 8000);
                    QMessageBox::information(this, tr("Export Database"),
                        tr("Exported %n torrent(s) to:\n%1", nullptr, static_cast<int>(processed))
                            .arg(summary["path"].toString()));
                    return;
                }
                const qint64 inserted = summary["inserted"].toVariant().toLongLong();
                const qint64 merged = summary["merged"].toVariant().toLongLong();
                showStatusMessage(
                    tr("Imported %n new torrent(s) (%1 already known).", nullptr, static_cast<int>(inserted))
                        .arg(merged),
                    8000);
                QMessageBox::information(this, tr("Import Database"),
                    tr("Merge finished.\n\n%1 new torrents added\n%2 already in your index").arg(inserted).arg(merged));
            });
    }

    // Background data migrations run (in a worker thread) while the window is up
    // — surface their progress in the status bar and report failures. Sync
    // pre-start migrations finish before this window exists (see
    // Application::start()).
    if (app_->migrations()) {
        auto* migrations = app_->migrations();
        connect(
            migrations, &rats::service::MigrationService::migrationProgress, this, &MainWindow::onMigrationProgress);
        connect(migrations, &rats::service::MigrationService::migrationError, this,
            [this](const QString& migrationId, const QString& error) {
                QMessageBox::warning(this, tr("Data Migration"),
                    tr("A background data migration failed; it "
                       "will retry on the next start.\n\n%1: %2")
                        .arg(migrationId, error));
            });
        connect(migrations, &rats::service::MigrationService::allMigrationsCompleted, this,
            [this]() { showStatusMessage(tr("Data migration complete"), 4000); });
    }
}

void MainWindow::connectPeerSignals()
{
    // Source-first discovery only. Both clients emit a Torrent only after their
    // concrete tracker page has re-proved the exact info-hash and passed the
    // strict rich-release completeness rule.
    if (auto* source = app_->rutorSearch()) {
        connect(source, &rats::net::RutorSearchClient::resultReady, this,
            [this](const QString& query, const Torrent& torrent) {
                addVerifiedSourceResult(query, torrent);
            });
        connect(source, &rats::net::RutorSearchClient::searchFinished, this,
            [this](const QString& query, int accepted, int rejected, const QString& error) {
                Q_UNUSED(accepted);
                finishStrictSource(query, QStringLiteral("Rutor"), rejected, error);
            });
    }

    if (auto* source = app_->ruTrackerRuSearch()) {
        connect(source, &rats::net::RuTrackerRuSearchClient::resultReady, this,
            [this](const QString& query, const Torrent& torrent) {
                addVerifiedSourceResult(query, torrent);
            });
        connect(source, &rats::net::RuTrackerRuSearchClient::searchFinished, this,
            [this](const QString& query, int accepted, int rejected, const QString& error) {
                Q_UNUSED(accepted);
                finishStrictSource(query, QStringLiteral("RuTracker.RU"), rejected, error);
            });
    }

    if (auto* source = app_->megaPeerSearch()) {
        connect(source, &rats::net::MegaPeerSearchClient::resultReady, this,
            [this](const QString& query, const Torrent& torrent) {
                addVerifiedSourceResult(query, torrent);
            });
        connect(source, &rats::net::MegaPeerSearchClient::searchFinished, this,
            [this](const QString& query, int accepted, int rejected, const QString& error) {
                Q_UNUSED(accepted);
                finishStrictSource(query, QStringLiteral("MegaPeer"), rejected, error);
            });
    }

    // Peer single-torrent replies are still useful for an already selected
    // verified result (for example to obtain its file list). Peer SEARCH replies
    // are intentionally not connected to the Search Results model anymore.
    if (!app_->peerApi())
        return;
    auto* peerApi = app_->peerApi();

    connect(peerApi, &rats::peer::PeerApi::remoteTorrentReceived, this,
        [this](const QString& hash, const QJsonObject& data) {
            if (!filesWidget || hash.isEmpty())
                return;
            if (detailsPanel && detailsPanel->currentHash() != hash)
                return;

            const rats::domain::Torrent t = codec::torrentFromJson(data);
            if (!t.fileList.isEmpty()) {
                filesWidget->setFiles(hash, t.name, t.fileList);
                filesWidget->show();
                verticalSplitter->setSizes({ 600, 200 });
            }
        });
}

void MainWindow::addVerifiedSourceResult(
    const QString& query, const Torrent& incoming)
{
    if (query != currentSearchQuery_)
        return;

    const QJsonObject& info = incoming.info;
    if (!info.value(QStringLiteral("sourceVerified")).toBool(false)
        || !info.value(QStringLiteral("strictComplete")).toBool(false)
        || info.value(QStringLiteral("sourceUrl")).toString().isEmpty()) {
        return;
    }

    // The same BitTorrent payload may be published on both Rutor and
    // RuTracker.RU. The info-hash is the identity, so show/index it once per
    // search rather than presenting duplicate rows with different source URLs.
    if (strictSearchHashes_.contains(incoming.hash))
        return;

    Torrent normalized = incoming;

    // A source-first result has not been in the local index yet, so there is no
    // local "added" timestamp on its very first appearance. Assign it before the
    // initial insert/display; otherwise Date is blank until the same hash is
    // searched a second time and read back from the database.
    if (!normalized.added.isValid())
        normalized.added = QDateTime::currentDateTimeUtc();

    Torrent torrent = normalized;
    if (app_->indexing()) {
        const auto inserted = app_->indexing()->insert(normalized);
        if (!inserted.success)
            return;
        torrent = inserted.torrent;

        // Heal older exact-source rows that were stored before this first-pass
        // classification fix.
        if (torrent.contentType == rats::domain::ContentType::Unknown
            && normalized.contentType != rats::domain::ContentType::Unknown) {
            torrent.contentType = normalized.contentType;
            if (app_->torrents()) {
                app_->torrents()->updateClassification(
                    torrent.hash, torrent.contentType, torrent.contentCategory);
            }
        }

        if (!torrent.added.isValid())
            torrent.added = normalized.added;
    }

    const SearchFilters filters = currentSearchFilters();
    if ((filters.sizeMin > 0 || filters.sizeMax > 0) && torrent.size <= 0)
        return;
    if (filters.sizeMin > 0 && torrent.size < filters.sizeMin)
        return;
    if (filters.sizeMax > 0 && torrent.size > filters.sizeMax)
        return;

    if ((filters.filesMin > 0 || filters.filesMax > 0) && torrent.files <= 0)
        return;
    if (filters.filesMin > 0 && torrent.files < filters.filesMin)
        return;
    if (filters.filesMax > 0 && torrent.files > filters.filesMax)
        return;

    const QString wantedType = typeComboBox->currentData().toString();
    if (!wantedType.isEmpty()
        && rats::domain::toString(torrent.contentType).compare(
               wantedType, Qt::CaseInsensitive)
            != 0) {
        return;
    }
    if (safeSearchCheckBox->isChecked()
        && torrent.contentCategory == rats::domain::ContentCategory::XXX) {
        return;
    }

    strictSearchHashes_.insert(incoming.hash);

    SearchHit hit;
    hit.torrent = torrent;
    searchResultModel->addResult(hit);
    showStatusMessage(
        tr("✅ Verified releases: %1").arg(searchResultModel->resultCount()),
        1500);
}

void MainWindow::finishStrictSource(
    const QString& query, const QString& provider, int rejected,
    const QString& error)
{
    if (query != currentSearchQuery_)
        return;

    strictSourcesRejected_ += qMax(0, rejected);
    if (!error.isEmpty())
        strictSourceErrors_ << provider + QStringLiteral(": ") + error;

    if (strictSourcesPending_ > 0)
        --strictSourcesPending_;
    if (strictSourcesPending_ > 0)
        return;

    const int visible = searchResultModel->resultCount();
    if (visible == 0 && !strictSourceErrors_.isEmpty()) {
        showStatusMessage(
            tr("⚠️ Source search failed: %1")
                .arg(strictSourceErrors_.join(QStringLiteral(" · "))),
            7000);
        return;
    }

    QString message = tr("✅ Verified: %1 · hidden incomplete/unverified: %2")
                          .arg(visible)
                          .arg(strictSourcesRejected_);
    if (!strictSourceErrors_.isEmpty()) {
        message += tr(" · unavailable: %1")
                       .arg(strictSourceErrors_.join(QStringLiteral(" · ")));
    }
    showStatusMessage(message, 6000);
}

// --- Search filters (size / file-count ranges) ------------------------------

namespace {
// Unit multipliers offered next to each size field. The combo carries the
// multiplier as its item data, so reading a field back is one multiplication.
void fillSizeUnits(QComboBox* combo, int defaultIndex)
{
    combo->addItem(QObject::tr("KB"), QVariant::fromValue<qint64>(1024LL));
    combo->addItem(QObject::tr("MB"), QVariant::fromValue<qint64>(1024LL * 1024));
    combo->addItem(QObject::tr("GB"), QVariant::fromValue<qint64>(1024LL * 1024 * 1024));
    combo->addItem(QObject::tr("TB"), QVariant::fromValue<qint64>(1024LL * 1024 * 1024 * 1024));
    combo->setCurrentIndex(defaultIndex);
}

qint64 sizeFieldBytes(const QDoubleSpinBox* spin, const QComboBox* unit)
{
    if (spin->value() <= 0.0)
        return 0; // "any"
    return static_cast<qint64>(spin->value() * static_cast<double>(unit->currentData().toLongLong()));
}
} // namespace

void MainWindow::setupSearchFilters()
{
    filtersButton = new QToolButton(this);
    // Styled through the theme sheets to match the combo boxes it sits between;
    // the plain QToolButton rule is the flat toolbar look.
    filtersButton->setObjectName("searchFiltersButton");
    filtersButton->setPopupMode(QToolButton::InstantPopup);
    filtersButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    filtersButton->setMinimumHeight(44);
    filtersButton->setCursor(Qt::PointingHandCursor);

    // A drop-down rather than four more widgets in the search bar: the ranges
    // are used far less often than the type/sort combos, and the button label
    // still reports how many of them are active.
    filtersMenu = new QMenu(filtersButton);
    QWidget* panel = new QWidget(filtersMenu);
    QFormLayout* form = new QFormLayout(panel);
    form->setContentsMargins(12, 10, 12, 10);
    form->setSpacing(8);

    // Every field treats 0 as "no bound", which is what the special value text
    // spells out — an empty-looking spin box would read as "zero bytes".
    auto makeSizeRow = [panel](QDoubleSpinBox*& spin, QComboBox*& unit, int defaultUnitIndex) {
        spin = new QDoubleSpinBox(panel);
        spin->setRange(0.0, 999999.0);
        spin->setDecimals(2);
        spin->setSpecialValueText(tr("any"));
        spin->setMinimumWidth(110);
        unit = new QComboBox(panel);
        unit->setObjectName("searchFilterUnit"); // narrower than the global combo min-width
        fillSizeUnits(unit, defaultUnitIndex);

        QWidget* row = new QWidget(panel);
        QHBoxLayout* layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(6);
        layout->addWidget(spin, 1);
        layout->addWidget(unit);
        return row;
    };

    form->addRow(tr("Size from:"), makeSizeRow(sizeMinSpin, sizeMinUnit, 1)); // MB
    form->addRow(tr("Size to:"), makeSizeRow(sizeMaxSpin, sizeMaxUnit, 2)); // GB

    auto makeCountSpin = [panel](QSpinBox*& spin) {
        spin = new QSpinBox(panel);
        spin->setRange(0, 1000000);
        spin->setSpecialValueText(tr("any"));
        spin->setMinimumWidth(110);
        return spin;
    };
    form->addRow(tr("Files from:"), makeCountSpin(filesMinSpin));
    form->addRow(tr("Files to:"), makeCountSpin(filesMaxSpin));

    QPushButton* resetButton = new QPushButton(tr("Reset filters"), panel);
    // Neutral: the popup has no accept button, so nothing here should read as
    // the primary action.
    resetButton->setObjectName("secondaryButton");
    resetButton->setCursor(Qt::PointingHandCursor);
    form->addRow(resetButton);

    QWidgetAction* panelAction = new QWidgetAction(filtersMenu);
    panelAction->setDefaultWidget(panel);
    filtersMenu->addAction(panelAction);
    filtersButton->setMenu(filtersMenu);

    connect(resetButton, &QPushButton::clicked, this, &MainWindow::resetSearchFilters);
    // The label has to stay truthful while the popup is open, so every editor
    // feeds it; the search itself only re-runs once the popup closes.
    connect(sizeMinSpin, &QDoubleSpinBox::valueChanged, this, &MainWindow::updateSearchFiltersButton);
    connect(sizeMaxSpin, &QDoubleSpinBox::valueChanged, this, &MainWindow::updateSearchFiltersButton);
    connect(filesMinSpin, &QSpinBox::valueChanged, this, &MainWindow::updateSearchFiltersButton);
    connect(filesMaxSpin, &QSpinBox::valueChanged, this, &MainWindow::updateSearchFiltersButton);
    connect(sizeMinUnit, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
        [this](int) { updateSearchFiltersButton(); });
    connect(sizeMaxUnit, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
        [this](int) { updateSearchFiltersButton(); });

    updateSearchFiltersButton();
}

MainWindow::SearchFilters MainWindow::currentSearchFilters() const
{
    SearchFilters filters;
    if (!sizeMinSpin)
        return filters;
    filters.sizeMin = sizeFieldBytes(sizeMinSpin, sizeMinUnit);
    filters.sizeMax = sizeFieldBytes(sizeMaxSpin, sizeMaxUnit);
    filters.filesMin = filesMinSpin->value();
    filters.filesMax = filesMaxSpin->value();
    return filters;
}

void MainWindow::updateSearchFiltersButton()
{
    if (!filtersButton)
        return;

    // An upper bound below the lower one can only ever return nothing, so the
    // one the user is not editing follows along instead of silently emptying
    // the result list. Raising the max to the min settles on the second pass.
    const SearchFilters filters = currentSearchFilters();
    if (filters.sizeMin > 0 && filters.sizeMax > 0 && filters.sizeMax < filters.sizeMin) {
        const qint64 unit = sizeMaxUnit->currentData().toLongLong();
        QSignalBlocker block(sizeMaxSpin);
        sizeMaxSpin->setValue(static_cast<double>(filters.sizeMin) / static_cast<double>(unit));
    }
    if (filters.filesMin > 0 && filters.filesMax > 0 && filters.filesMax < filters.filesMin) {
        QSignalBlocker block(filesMaxSpin);
        filesMaxSpin->setValue(filters.filesMin);
    }

    const SearchFilters settled = currentSearchFilters();
    const int active = settled.activeCount();
    filtersButton->setText(active > 0 ? tr("Filters (%1)").arg(active) : tr("Filters"));

    QStringList parts;
    if (settled.sizeMin > 0)
        parts << tr("size from %1").arg(rats::ui::formatSize(settled.sizeMin));
    if (settled.sizeMax > 0)
        parts << tr("size to %1").arg(rats::ui::formatSize(settled.sizeMax));
    if (settled.filesMin > 0)
        parts << tr("from %n file(s)", nullptr, settled.filesMin);
    if (settled.filesMax > 0)
        parts << tr("to %n file(s)", nullptr, settled.filesMax);
    filtersButton->setToolTip(
        parts.isEmpty() ? tr("Filter results by size and file count") : parts.join(QLatin1String(", ")));
}

void MainWindow::resetSearchFilters()
{
    if (!sizeMinSpin)
        return;
    sizeMinSpin->setValue(0.0);
    sizeMaxSpin->setValue(0.0);
    filesMinSpin->setValue(0);
    filesMaxSpin->setValue(0);
}

void MainWindow::performSearch(const QString& query)
{
    const QString trimmed = query.trimmed();
    if (trimmed.isEmpty())
        return;

    currentSearchQuery_ = trimmed;
    if (app_->searchHistory())
        app_->searchHistory()->add(trimmed);

    qInfo() << "Strict multi-source search started:" << trimmed.left(80);
    tabWidget->setCurrentIndex(0);
    searchResultModel->clearResults();

    // A new query owns a new selection context. Do not leave the previous
    // torrent's details/files visible while the new source results stream in.
    ++fileMetadataRequestSerial_;
    fileMetadataLoadingHash_.clear();
    resultsTableView->clearSelection();
    detailsPanel->hide();
    filesWidget->clear();
    filesWidget->hide();

    strictSearchHashes_.clear();
    strictSourcesRejected_ = 0;
    strictSourceErrors_.clear();
    strictSourcesPending_ = 0;

    auto* rutor = app_->rutorSearch();
    auto* rutracker = app_->ruTrackerRuSearch();
    auto* megapeer = app_->megaPeerSearch();
    if (rutor)
        ++strictSourcesPending_;
    if (rutracker)
        ++strictSourcesPending_;
    if (megapeer)
        ++strictSourcesPending_;

    if (strictSourcesPending_ == 0) {
        showStatusMessage(
            tr("⚠️ Exact-source search is unavailable in this build."), 5000);
        return;
    }

    showStatusMessage(
        tr("🔍 Searching verified releases on Rutor + RuTracker.RU + MegaPeer…"), 0);

    // These are deliberately the ONLY discovery sources for Search Results.
    // Local index, P2P search and DHT-only hits cannot enter this table because
    // they do not prove a concrete release page.
    const QString sortKey = sortComboBox->currentData().toString();
    const QString contentType = typeComboBox->currentData().toString();
    if (rutor)
        rutor->search(trimmed, 50, sortKey, contentType);
    if (rutracker)
        rutracker->search(trimmed, 50, sortKey, contentType);
    if (megapeer)
        megapeer->search(trimmed, 50, sortKey, contentType);
}

void MainWindow::updateStatusBar()
{
    if (cachedRemoteTorrentCount_ > 0) {
        torrentCountLabel->setText(tr("📦 Torrents: %1 + %2").arg(cachedTorrentCount_).arg(cachedRemoteTorrentCount_));
    } else {
        torrentCountLabel->setText(tr("📦 Torrents: %1").arg(cachedTorrentCount_));
    }
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    rats::app::ConfigStore* config = app_ ? app_->config() : nullptr;

    // Explicit Quit and update installation are deliberate shutdowns. Never
    // intercept them with close-to-tray, and never rewrite tray preferences to
    // force the close.
    if (updateInstalling_ || forceQuit_) {
        saveSettings();
        event->accept();
        QApplication::quit();
        return;
    }

    // Hide to tray instead of closing if enabled.
    bool closeToTray = config ? config->trayOnClose() : false;
    if (closeToTray && trayIcon && trayIcon->isVisible()) {
        hiddenToTray_ = true;
        hide();
        if (!trayNotificationShown_) {
            trayIcon->showMessage(tr("Rats Search"), tr("Application is still running in the system tray."),
                QSystemTrayIcon::Information, 2000);
            trayNotificationShown_ = true;
        }
        event->ignore();
        return;
    }

    // Close immediately. The previous confirmation dialog made an ordinary
    // macOS Quit/close action require a redundant second confirmation.
    // Persist UI state; service shutdown (DB, P2P, session) is
    // main()/Application's job.
    saveSettings();
    event->accept();

    if (isHidden())
        QApplication::quit();
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->mimeData()->hasUrls()) {
        for (const QUrl& url : event->mimeData()->urls()) {
            if (url.isLocalFile() && url.toLocalFile().endsWith(".torrent", Qt::CaseInsensitive)) {
                event->acceptProposedAction();
                return;
            }
        }
    }
    event->ignore();
}

void MainWindow::dropEvent(QDropEvent* event)
{
    if (!event->mimeData()->hasUrls()) {
        event->ignore();
        return;
    }

    QStringList torrentFiles;
    for (const QUrl& url : event->mimeData()->urls()) {
        if (url.isLocalFile()) {
            QString filePath = url.toLocalFile();
            if (filePath.endsWith(".torrent", Qt::CaseInsensitive))
                torrentFiles.append(filePath);
        }
    }

    if (torrentFiles.isEmpty()) {
        event->ignore();
        return;
    }

    event->acceptProposedAction();

    for (const QString& filePath : torrentFiles) {
        if (!app_->api())
            continue;
        app_->api()->call(
            "torrent.import", QJsonObject { { "file", filePath } }, [this, filePath](const Result& response) {
                if (response.ok()) {
                    QJsonObject data = response.data().toObject();
                    QString name = data["name"].toString();
                    bool alreadyExists = data["alreadyExists"].toBool();
                    showStatusMessage(
                        alreadyExists ? tr("Already indexed: %1").arg(name) : tr("Added: %1").arg(name), 2000);
                    // Auto-favorite imported torrents.
                    Torrent t = codec::torrentFromJson(data);
                    if (t.isValid())
                        addToFavorites(t);
                } else {
                    qWarning() << "Failed to add torrent file:" << filePath << "-" << response.error();
                }
            });
    }

    showStatusMessage(tr("Processing %n torrent file(s)...", nullptr, static_cast<int>(torrentFiles.size())), 3000);
}

// ============================================================================
// Search / selection slots
// ============================================================================

void MainWindow::onSearchButtonClicked()
{
    performSearch(searchLineEdit->text());
}

void MainWindow::onSearchTextChanged(const QString& text)
{
    searchButton->setEnabled(!text.isEmpty());
}

// ============================================================================
// Search history
// ============================================================================

void MainWindow::setupSearchHistory()
{
    searchHistoryModel = new QStringListModel(this);
    searchCompleter = new QCompleter(searchHistoryModel, this);
    searchCompleter->setCaseSensitivity(Qt::CaseInsensitive);
    // Substring matching, so "ubuntu 24" also surfaces "xubuntu 24.04"; with an
    // empty prefix every entry matches, which is what the click-to-drop-down
    // path below relies on.
    searchCompleter->setFilterMode(Qt::MatchContains);
    searchCompleter->setCompletionMode(QCompleter::PopupCompletion);
    searchCompleter->setMaxVisibleItems(12);
    searchLineEdit->setCompleter(searchCompleter);

    // Picking an entry from the dropdown runs it immediately — activated fires
    // after the line edit has been filled in.
    connect(searchCompleter, QOverload<const QString&>::of(&QCompleter::activated), this,
        [this](const QString& text) { performSearch(text); });

    // Focus/click on an empty field drops down the whole history.
    searchLineEdit->installEventFilter(this);

    // The history lives next to the standard cut/copy/paste menu.
    searchLineEdit->setContextMenuPolicy(Qt::CustomContextMenu);

    refreshSearchHistory();
}

void MainWindow::refreshSearchHistory()
{
    if (!searchHistoryModel || !app_ || !app_->searchHistory())
        return;
    searchHistoryModel->setStringList(app_->searchHistory()->queries());
}

void MainWindow::showSearchHistoryPopup()
{
    if (!searchCompleter || searchHistoryModel->rowCount() == 0)
        return;
    searchCompleter->setCompletionPrefix(QString());
    searchCompleter->complete();
}

void MainWindow::showSearchContextMenu(const QPoint& pos)
{
    // Start from the built-in menu so cut/copy/paste/undo stay available.
    QMenu* menu = searchLineEdit->createStandardContextMenu();
    menu->setAttribute(Qt::WA_DeleteOnClose);

    const QStringList history = app_ && app_->searchHistory() ? app_->searchHistory()->queries(10) : QStringList();
    if (!history.isEmpty()) {
        menu->addSeparator();
        QMenu* recent = menu->addMenu(tr("🕘 Recent searches"));
        for (const QString& query : history) {
            QAction* action = recent->addAction(query);
            connect(action, &QAction::triggered, this, [this, query]() {
                searchLineEdit->setText(query);
                performSearch(query);
            });
        }
        recent->addSeparator();
        recent->addAction(tr("Clear search history"), this, &MainWindow::clearSearchHistory);
    }

    menu->popup(searchLineEdit->mapToGlobal(pos));
}

void MainWindow::clearSearchHistory()
{
    if (app_ && app_->searchHistory() && app_->searchHistory()->clear())
        showStatusMessage(tr("🕘 Search history cleared"), 3000);
}

void MainWindow::onTorrentSelected(const QModelIndex& index)
{
    if (!index.isValid())
        return;
    const SearchHit hit = searchResultModel->getHit(index.row());
    if (!hit.torrent.isValid())
        return;

    showTorrentDetails(hit.torrent);

    // A remote hit arrives with metadata only — search replies never carry file
    // lists. If the files panel came up empty (nothing embedded, nothing local),
    // pull the full torrent from the peer that offered it. The reply repopulates
    // the panel and clones the torrent, with its files, into our database.
    if (hit.remote && !hit.sourcePeerId.isEmpty() && !filesWidget->hasFiles() && app_->peerApi())
        app_->peerApi()->requestTorrent(hit.sourcePeerId, hit.torrent.hash, /*includeFiles*/ true);
}

void MainWindow::onTorrentDoubleClicked(const QModelIndex& index)
{
    if (!index.isValid())
        return;
    Torrent torrent = searchResultModel->getTorrent(index.row());
    if (torrent.isValid())
        openMagnetLink(torrent);
}

void MainWindow::onSortOrderChanged(int index)
{
    Q_UNUSED(index);
    if (!currentSearchQuery_.isEmpty())
        performSearch(currentSearchQuery_);
}

void MainWindow::onTabChanged(int index)
{
    Q_UNUSED(index);

    QWidget* currentWidget = tabWidget->currentWidget();
    if (!currentWidget)
        return;

    Torrent selectedTorrent;

    if (currentWidget == resultsTableView->parentWidget()) {
        QModelIndex idx = resultsTableView->currentIndex();
        if (idx.isValid())
            selectedTorrent = searchResultModel->getTorrent(idx.row());
    } else if (currentWidget == downloadsWidget) {
        // Downloads tab has no torrent selection — hide the detail panels and
        // invalidate any metadata callback belonging to the previous selection.
        ++fileMetadataRequestSerial_;
        fileMetadataLoadingHash_.clear();
        detailsPanel->hide();
        filesWidget->clear();
        filesWidget->hide();
        return;
    } else {
        // Any of the torrent-table / activity tabs: restore its remembered
        // selection.
        selectedTorrent = tabSelection_.value(currentWidget);
    }

    if (selectedTorrent.isValid()) {
        showTorrentDetails(selectedTorrent);
    } else {
        ++fileMetadataRequestSerial_;
        fileMetadataLoadingHash_.clear();
        detailsPanel->hide();
        filesWidget->clear();
        filesWidget->hide();
    }
}

void MainWindow::onDetailsPanelCloseRequested()
{
    ++fileMetadataRequestSerial_;
    fileMetadataLoadingHash_.clear();
    detailsPanel->hide();
    filesWidget->clear();
    filesWidget->hide();
    resultsTableView->clearSelection();
}

namespace {

QVector<rats::domain::File> domainFilesFromMetadata(
    const rats::net::TorrentMetadata& metadata)
{
    QVector<rats::domain::File> files;
    files.reserve(metadata.files.size());
    for (const rats::net::EngineFile& source : metadata.files) {
        if (source.path.isEmpty() || source.size < 0)
            continue;
        rats::domain::File file;
        file.path = source.path;
        file.size = source.size;
        files.append(std::move(file));
    }
    return files;
}

QNetworkRequest sourceTorrentRequest(const QUrl& url)
{
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader,
        QStringLiteral("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
                       "AppleWebKit/537.36 RatsSearch/2"));
    request.setRawHeader("Accept",
        "application/x-bittorrent,application/octet-stream,*/*;q=0.5");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
        QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(10000);
    return request;
}

} // namespace

void MainWindow::acceptResolvedTorrentFiles(
    const Torrent& torrent,
    const rats::net::TorrentMetadata& metadata,
    quint64 requestSerial)
{
    if (requestSerial != fileMetadataRequestSerial_)
        return;
    if (!metadata.valid || metadata.hash.compare(torrent.hash, Qt::CaseInsensitive) != 0) {
        requestTorrentFilesViaBep9(
            torrent, requestSerial, tr("source metadata hash mismatch"));
        return;
    }

    const QVector<rats::domain::File> files
        = domainFilesFromMetadata(metadata);
    if (files.isEmpty()) {
        requestTorrentFilesViaBep9(
            torrent, requestSerial, tr("source metadata has no file list"));
        return;
    }

    if (app_->torrents())
        app_->torrents()->updateFiles(torrent.hash, files);

    fileMetadataLoadingHash_.clear();
    if (detailsPanel && detailsPanel->currentHash() == torrent.hash) {
        filesWidget->setFiles(torrent.hash,
            metadata.name.isEmpty() ? torrent.name : metadata.name, files);
        showStatusMessage(
            tr("📁 Loaded %n torrent file(s)", nullptr, files.size()), 2500);
    }
}

void MainWindow::requestTorrentFilesViaBep9(
    const Torrent& torrent,
    quint64 requestSerial,
    const QString& previousError)
{
    if (requestSerial != fileMetadataRequestSerial_)
        return;

    auto* engine = app_ ? app_->engine() : nullptr;
    if (!engine || !engine->isReady()) {
        fileMetadataLoadingHash_.clear();
        if (detailsPanel && detailsPanel->currentHash() == torrent.hash) {
            filesWidget->setError(torrent.hash, torrent.name,
                previousError.isEmpty()
                    ? tr("BitTorrent metadata service is unavailable")
                    : previousError);
        }
        return;
    }

    QPointer<MainWindow> guard(this);
    const bool started = engine->fetchMetadata(
        torrent.hash,
        [guard, torrent, requestSerial, previousError](
            const rats::net::TorrentMetadata& metadata,
            const QString& error) {
            if (!guard)
                return;
            QMetaObject::invokeMethod(guard,
                [guard, torrent, requestSerial, metadata, error, previousError]() {
                    if (!guard || requestSerial != guard->fileMetadataRequestSerial_)
                        return;

                    if (metadata.valid
                        && metadata.hash.compare(
                               torrent.hash, Qt::CaseInsensitive) == 0) {
                        const QVector<rats::domain::File> files
                            = domainFilesFromMetadata(metadata);
                        if (!files.isEmpty()) {
                            if (guard->app_->torrents())
                                guard->app_->torrents()->updateFiles(
                                    torrent.hash, files);
                            guard->fileMetadataLoadingHash_.clear();
                            if (guard->detailsPanel
                                && guard->detailsPanel->currentHash()
                                    == torrent.hash) {
                                guard->filesWidget->setFiles(
                                    torrent.hash,
                                    metadata.name.isEmpty()
                                        ? torrent.name : metadata.name,
                                    files);
                                guard->showStatusMessage(
                                    tr("📁 Loaded %n torrent file(s)",
                                        nullptr, files.size()),
                                    2500);
                            }
                            return;
                        }
                    }

                    guard->fileMetadataLoadingHash_.clear();
                    if (guard->detailsPanel
                        && guard->detailsPanel->currentHash() == torrent.hash) {
                        QString reason = error.trimmed();
                        if (reason.isEmpty())
                            reason = previousError.trimmed();
                        if (reason.isEmpty())
                            reason = tr("metadata not available from the swarm");
                        guard->filesWidget->setError(
                            torrent.hash, torrent.name, reason);
                    }
                },
                Qt::QueuedConnection);
        },
        20000);

    if (!started) {
        fileMetadataLoadingHash_.clear();
        if (detailsPanel && detailsPanel->currentHash() == torrent.hash) {
            filesWidget->setError(torrent.hash, torrent.name,
                previousError.isEmpty()
                    ? tr("could not start BitTorrent metadata request")
                    : previousError);
        }
    }
}

void MainWindow::requestTorrentFiles(const Torrent& torrent)
{
    if (!torrent.isValid() || filesWidget->hasFiles())
        return;

    if (fileMetadataLoadingHash_ == torrent.hash) {
        filesWidget->setLoading(torrent.hash, torrent.name);
        return;
    }

    const quint64 requestSerial = ++fileMetadataRequestSerial_;
    fileMetadataLoadingHash_ = torrent.hash;
    filesWidget->setLoading(torrent.hash, torrent.name);

    // Best path for exact sources that expose a direct .torrent: the SAME
    // source row binds the detail URL and download URL. Parse the metainfo and
    // accept it only if its computed hash is identical to the verified result.
    const QString provider
        = torrent.info.value(QStringLiteral("sourceProvider")).toString();
    const QUrl sourceTorrentUrl(
        torrent.info.value(QStringLiteral("sourceTorrentUrl")).toString());
    if ((provider == QStringLiteral("rutor")
            || provider == QStringLiteral("megapeer"))
        && sourceTorrentUrl.isValid()
        && (sourceTorrentUrl.scheme() == QStringLiteral("https")
            || sourceTorrentUrl.scheme() == QStringLiteral("http"))
        && fileMetadataNetwork_) {
        QNetworkReply* reply
            = fileMetadataNetwork_->get(sourceTorrentRequest(sourceTorrentUrl));
        connect(reply, &QNetworkReply::finished, this,
            [this, reply, torrent, requestSerial]() {
                const auto error = reply->error();
                const QString errorText = reply->errorString();
                QByteArray bytes;
                if (error == QNetworkReply::NoError)
                    bytes = reply->readAll();
                reply->deleteLater();

                if (requestSerial != fileMetadataRequestSerial_)
                    return;

                // Torrent metainfo is normally tiny. Refuse unexpectedly large
                // bodies (HTML challenge/error pages included) and fall back to
                // BEP 9 rather than feeding arbitrary data into the parser.
                constexpr int kMaxTorrentMetadataBytes = 32 * 1024 * 1024;
                if (error == QNetworkReply::NoError
                    && !bytes.isEmpty()
                    && bytes.size() <= kMaxTorrentMetadataBytes
                    && app_->engine()) {
                    QTemporaryFile temp(
                        QDir::tempPath()
                        + QStringLiteral("/rats-source-XXXXXX.torrent"));
                    if (temp.open()
                        && temp.write(bytes) == bytes.size()) {
                        temp.flush();
                        temp.close();
                        const rats::net::TorrentMetadata metadata
                            = app_->engine()->readTorrentFile(
                                temp.fileName());
                        if (metadata.valid
                            && metadata.hash.compare(
                                   torrent.hash, Qt::CaseInsensitive) == 0
                            && !metadata.files.isEmpty()) {
                            acceptResolvedTorrentFiles(
                                torrent, metadata, requestSerial);
                            return;
                        }
                    }
                }

                requestTorrentFilesViaBep9(
                    torrent, requestSerial,
                    error == QNetworkReply::NoError
                        ? tr("exact source .torrent could not be verified")
                        : tr("exact source .torrent failed: %1")
                              .arg(errorText));
            });
        return;
    }

    // RuTracker.RU public search provides exact topic+magnet provenance but no
    // stable direct .torrent URL. BEP 9 is still exact: metadata is addressed by
    // this already-verified info-hash, not by title.
    requestTorrentFilesViaBep9(torrent, requestSerial);
}

void MainWindow::showTorrentDetails(const Torrent& torrent)
{
    detailsPanel->setTorrent(torrent);
    detailsPanel->show();

    // Show the file list. TorrentFilesWidget fetches the files itself (from the
    // torrent's embedded list or the repository) via the Application. For a remote
    // torrent with no local files, onTorrentSelected kicks off a peer fetch whose
    // reply repopulates this panel through the peerApi::remoteTorrentReceived
    // handler.
    filesWidget->setTorrent(torrent);
    filesWidget->show();
    verticalSplitter->setSizes({ 600, 200 });

    if (!filesWidget->hasFiles())
        requestTorrentFiles(torrent);
}

void MainWindow::openMagnetLink(const Torrent& torrent)
{
    QDesktopServices::openUrl(QUrl(torrent.magnetLink()));
}

void MainWindow::addToFavorites(const Torrent& torrent)
{
    if (app_->favorites() && torrent.isValid())
        app_->favorites()->add(torrent);
}

void MainWindow::onDownloadRequested(const QString& hash)
{
    qInfo() << "Download requested for torrent:" << hash.left(16);

    QMenu menu(this);
    menu.setStyleSheet(this->styleSheet());

    QString defaultPath = app_->config() ? app_->config()->downloadPath() : QString();

    QString defaultText = tr("📥 Download to default folder");
    if (!defaultPath.isEmpty()) {
        QString shortPath = defaultPath;
        if (shortPath.length() > 40)
            shortPath = "..." + shortPath.right(37);
        defaultText = tr("📥 Download to: %1").arg(shortPath);
    }
    QAction* defaultAction = menu.addAction(defaultText);
    defaultAction->setToolTip(defaultPath);

    QAction* customAction = menu.addAction(tr("📂 Choose download location..."));
    menu.addSeparator();
    QAction* cancelAction = menu.addAction(tr("❌ Cancel"));

    QAction* selectedAction = menu.exec(QCursor::pos());
    if (selectedAction == cancelAction || selectedAction == nullptr)
        return;

    QString downloadPath;
    if (selectedAction == defaultAction) {
        downloadPath = defaultPath;
    } else if (selectedAction == customAction) {
        QString startDir
            = defaultPath.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::DownloadLocation) : defaultPath;
        downloadPath = QFileDialog::getExistingDirectory(this, tr("Select Download Location"), startDir);
        if (downloadPath.isEmpty())
            return;
    }

    if (app_->downloads()) {
        qInfo() << "Starting download:" << hash.left(16) << "to:" << downloadPath;
        bool ok = app_->downloads()->add(hash, downloadPath);
        if (ok) {
            showStatusMessage(tr("⬇️ Download started"), 2000);
            if (detailsPanel && detailsPanel->currentHash() == hash)
                detailsPanel->setDownloadProgress(0.0, 0, 0, 0);
        } else {
            QMessageBox::warning(this, tr("Download Failed"),
                tr("Could not start the download. Check the info "
                   "hash and try again."));
        }
    }
}

void MainWindow::showTorrentContextMenu(const QPoint& pos)
{
    QModelIndex index = resultsTableView->indexAt(pos);
    if (!index.isValid())
        return;

    Torrent torrent = searchResultModel->getTorrent(index.row());
    if (!torrent.isValid())
        return;

    QMenu contextMenu(this);
    rats::ui::addTorrentActions(
        &contextMenu, this, torrent, [this](const QString& message) { showStatusMessage(message, 2000); });

    contextMenu.addSeparator();

    // Favorites.
    if (app_->favorites()) {
        if (app_->favorites()->isFavorite(torrent.hash)) {
            QAction* removeFavAction = contextMenu.addAction(tr("★ Remove from Favorites"));
            connect(removeFavAction, &QAction::triggered, [this, torrent]() {
                app_->favorites()->remove(torrent.hash);
                showStatusMessage(tr("Removed from favorites"), 2000);
            });
        } else {
            QAction* addFavAction = contextMenu.addAction(tr("⭐ Add to Favorites"));
            connect(addFavAction, &QAction::triggered, [this, torrent]() {
                addToFavorites(torrent);
                showStatusMessage(tr("Added to favorites: %1").arg(torrent.name), 2000);
            });
        }
    }

    contextMenu.addSeparator();

    QAction* exportAction = contextMenu.addAction(tr("💾 Export to .torrent file..."));
    connect(exportAction, &QAction::triggered, [this, torrent]() { exportTorrentToFile(torrent); });

    QAction* detailsAction = contextMenu.addAction(tr("Show Details"));
    connect(detailsAction, &QAction::triggered, [this, index]() { onTorrentSelected(index); });

    contextMenu.exec(resultsTableView->viewport()->mapToGlobal(pos));
}

void MainWindow::exportTorrentToFile(const Torrent& torrent)
{
    if (!torrent.isValid())
        return;
    if (!app_->exporter()) {
        QMessageBox::warning(this, tr("Export Torrent"), tr("Export is not available in this build."));
        return;
    }
    // Async: TorrentExporter fetches metadata (BEP 9) if needed, caches the
    // .torrent, then fires exportReady → onExportReady prompts for a save path.
    app_->exporter()->requestExport(torrent.hash, torrent.name);
}

void MainWindow::onExportReady(const QString& hash, const QString& name, const QString& cachePath)
{
    // Build a readable default filename from the torrent name.
    QString suggested = name.trimmed();
    static const QRegularExpression illegal(R"([<>:"/\\|?*\x00-\x1f])");
    suggested.replace(illegal, QStringLiteral("_"));
    if (suggested.isEmpty())
        suggested = hash;
    if (suggested.length() > 200)
        suggested.truncate(200);
    const QString defaultPath = QDir(QDir::homePath()).absoluteFilePath(suggested + QStringLiteral(".torrent"));

    QString destination = QFileDialog::getSaveFileName(
        this, tr("Export Torrent As"), defaultPath, tr("Torrent Files (*.torrent);;All Files (*)"));
    if (destination.isEmpty())
        return; // user cancelled
    if (!destination.endsWith(QStringLiteral(".torrent"), Qt::CaseInsensitive))
        destination += QStringLiteral(".torrent");

    if (QFile::exists(destination))
        QFile::remove(destination);
    if (!QFile::copy(cachePath, destination)) {
        QMessageBox::critical(this, tr("Export Torrent"), tr("Failed to save torrent to:\n%1").arg(destination));
        return;
    }
    showStatusMessage(tr("Torrent exported to %1").arg(destination), 4000);
}

void MainWindow::onMigrationProgress(const QString& migrationId, qint64 current, qint64 total)
{
    Q_UNUSED(migrationId);
    // Pull the human-readable description from the service (thread-safe).
    const auto progress = app_->migrations()->currentProgress();
    const QString what = progress.description.isEmpty() ? tr("Migrating data") : progress.description;
    // Shown in the dedicated message slot; the peer/DHT/torrent counters are
    // unaffected. 0 = persist until the next progress tick / completion.
    if (total > 0) {
        const int percent = static_cast<int>((current * 100) / total);
        showStatusMessage(tr("%1: %2 / %3 (%4%)").arg(what).arg(current).arg(total).arg(percent), 0);
    } else {
        showStatusMessage(tr("%1…").arg(what), 0);
    }
}

// ============================================================================
// Network status
// ============================================================================

void MainWindow::onPeerCountChanged(int count)
{
    peerCountLabel->setText(tr("👥 Peers: %1").arg(count));
    refreshP2PStatus();
}

void MainWindow::refreshP2PStatus()
{
    auto* transport = app_ ? app_->transport() : nullptr;
    if (!transport || !transport->isRunning()) {
        p2pState_ = P2PState::NotStarted;
    } else if (transport->peerCount() > 0) {
        p2pState_ = P2PState::Connected;
    } else {
        p2pState_ = P2PState::NoConnection;
    }
    paintP2PIndicator();
}

void MainWindow::refreshSpiderStatus()
{
    // Read the crawler's live state instead of latching onto its statusChanged
    // signal: Application::start() runs before MainWindow exists, so the initial
    // "Active" is emitted with nobody connected yet.
    auto* crawler = app_ ? app_->crawler() : nullptr;
    const bool running = crawler && crawler->isRunning();
    spiderStatusLabel->setText(tr("🕷️ Spider: %1").arg(running ? tr("Active") : tr("Stopped")));
}

void MainWindow::paintP2PIndicator()
{
    QString indicator;
    QString statusText;

    switch (p2pState_) {
    case P2PState::NotStarted:
        indicator = "<span style='color: #e74c3c; font-size: 14px;'>●</span>";
        statusText = tr("P2P: Not Started");
        break;
    case P2PState::NoConnection:
        indicator = "<span style='color: #f39c12; font-size: 14px;'>●</span>";
        statusText = tr("P2P: No Peers");
        break;
    case P2PState::Connected:
        indicator = "<span style='color: #27ae60; font-size: 14px;'>●</span>";
        statusText = tr("P2P: Connected");
        break;
    }

    p2pStatusLabel->setText(QString("%1 %2").arg(indicator, statusText));
}

void MainWindow::updateNetworkStatus()
{
    auto* transport = app_ ? app_->transport() : nullptr;
    if (transport && transport->isRunning()) {
        size_t dhtNodes = transport->dhtNodeCount();
        if (transport->isDhtRunning())
            dhtNodeCountLabel->setText(tr("🌐 DHT: %n node(s)", nullptr, static_cast<int>(dhtNodes)));
        else
            dhtNodeCountLabel->setText(tr("🌐 DHT: Offline"));
    } else if (p2pState_ != P2PState::NotStarted) {
        dhtNodeCountLabel->setText(tr("🌐 DHT: Offline"));
    }

    refreshSpiderStatus();
}

void MainWindow::onTorrentIndexed(const Torrent& torrent)
{
    showStatusMessage(tr("📥 Indexed: %1").arg(torrent.name), 2000);
    // The torrent count follows the repository's statisticsChanged signal, and
    // tracker checks are driven by TrackerService (wired inside Application), so
    // nothing else to do here.
}

// ============================================================================
// Settings / theme / language
// ============================================================================

void MainWindow::onDarkModeChanged(bool enabled)
{
    qInfo() << "Theme changed to:" << (enabled ? "dark" : "light");
    applyTheme(enabled);
}

void MainWindow::showSettings()
{
    qInfo() << "Opening settings dialog";
    SettingsDialog dialog(app_, this);
    dialog.setStyleSheet(this->styleSheet());

    if (dialog.exec() == QDialog::Accepted) {
        qInfo() << "Settings saved by user";
        if (dialog.needsRestart()) {
            QMessageBox::information(this, tr("Restart Required"),
                tr("Some changes (network ports or data directory) will take effect "
                   "after restarting the "
                   "application."));
        }
        saveSettings();
    }
}

void MainWindow::showAbout()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("About Rats Search"));
    dialog.setFixedSize(420, 380);
    dialog.setStyleSheet(this->styleSheet());

    QVBoxLayout* layout = new QVBoxLayout(&dialog);
    layout->setSpacing(12);
    layout->setContentsMargins(32, 24, 32, 24);

    QLabel* logoLabel = new QLabel("🐀");
    logoLabel->setAlignment(Qt::AlignCenter);
    logoLabel->setObjectName("aboutLogoLabel");
    layout->addWidget(logoLabel);

    QLabel* titleLabel = new QLabel(QString("Rats Search %1").arg(RATSSEARCH_VERSION_STRING));
    titleLabel->setAlignment(Qt::AlignCenter);
    titleLabel->setObjectName("aboutTitleLabel");
    layout->addWidget(titleLabel);

    QLabel* subtitleLabel = new QLabel(tr("BitTorrent P2P Search Engine"));
    subtitleLabel->setAlignment(Qt::AlignCenter);
    subtitleLabel->setObjectName("subtitleLabel");
    layout->addWidget(subtitleLabel);

    QLabel* gitLabel = new QLabel(QString("Git: %1").arg(RATSSEARCH_GIT_DESCRIBE));
    gitLabel->setAlignment(Qt::AlignCenter);
    gitLabel->setObjectName("hintLabel");
    layout->addWidget(gitLabel);

    layout->addSpacing(8);

    QLabel* descLabel = new QLabel(QString(tr("Built with Qt %1 and librats\n\n"
                                              "A powerful decentralized torrent search engine\n"
                                              "with DHT crawling and full-text search."))
            .arg(qVersion()));
    descLabel->setAlignment(Qt::AlignCenter);
    descLabel->setWordWrap(true);
    layout->addWidget(descLabel);

    layout->addSpacing(8);

    QLabel* copyrightLabel = new QLabel(tr("Copyright © 2026"));
    copyrightLabel->setAlignment(Qt::AlignCenter);
    copyrightLabel->setObjectName("hintLabel");
    layout->addWidget(copyrightLabel);

    QLabel* linkLabel
        = new QLabel(QString("<a href='https://github.com/DEgITx/rats-search'>%1</a>").arg(tr("GitHub Repository")));
    linkLabel->setAlignment(Qt::AlignCenter);
    linkLabel->setObjectName("linkLabel");
    linkLabel->setOpenExternalLinks(true);
    layout->addWidget(linkLabel);

    layout->addStretch();

    QPushButton* okButton = new QPushButton(tr("OK"));
    okButton->setFixedWidth(100);
    connect(okButton, &QPushButton::clicked, &dialog, &QDialog::accept);

    QHBoxLayout* buttonLayout = new QHBoxLayout();
    buttonLayout->addStretch();
    buttonLayout->addWidget(okButton);
    buttonLayout->addStretch();
    layout->addLayout(buttonLayout);

    dialog.exec();
}

void MainWindow::showChangelog()
{
    qInfo() << "Showing changelog dialog";

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Changelog - What's New"));
    dialog.setMinimumSize(650, 550);
    dialog.setStyleSheet(this->styleSheet());

    QVBoxLayout* layout = new QVBoxLayout(&dialog);
    layout->setSpacing(16);
    layout->setContentsMargins(24, 24, 24, 24);

    QHBoxLayout* headerLayout = new QHBoxLayout();

    QLabel* iconLabel = new QLabel("📋");
    iconLabel->setObjectName("aboutLogoLabel");
    headerLayout->addWidget(iconLabel);

    QLabel* titleLabel = new QLabel(tr("Changelog"));
    titleLabel->setObjectName("aboutTitleLabel");
    headerLayout->addWidget(titleLabel);

    headerLayout->addStretch();

    QLabel* versionLabel = new QLabel(QString("v%1").arg(RATSSEARCH_VERSION_STRING));
    versionLabel->setObjectName("subtitleLabel");
    headerLayout->addWidget(versionLabel);

    layout->addLayout(headerLayout);

    QLabel* subtitleLabel = new QLabel(tr("Recent changes and updates to Rats Search"));
    subtitleLabel->setObjectName("hintLabel");
    layout->addWidget(subtitleLabel);

    QTextEdit* changelogText = new QTextEdit();
    changelogText->setReadOnly(true);
    changelogText->setMinimumHeight(350);

    QString changelogContent;
    QFile resourceFile(":/CHANGELOG.md");
    if (resourceFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        changelogContent = QString::fromUtf8(resourceFile.readAll());
        resourceFile.close();
    } else {
        QString appPath = QApplication::applicationDirPath();
        QFile localFile(appPath + "/CHANGELOG.md");
        if (localFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            changelogContent = QString::fromUtf8(localFile.readAll());
            localFile.close();
        } else {
            QFile devFile("CHANGELOG.md");
            if (devFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
                changelogContent = QString::fromUtf8(devFile.readAll());
                devFile.close();
            }
        }
    }

    if (changelogContent.isEmpty())
        changelogContent = tr("# Changelog\n\nNo changelog available.");

    changelogText->setMarkdown(changelogContent);
    layout->addWidget(changelogText, 1);

    QHBoxLayout* buttonLayout = new QHBoxLayout();
    buttonLayout->addStretch();

    QPushButton* closeBtn = new QPushButton(tr("Close"));
    closeBtn->setMinimumWidth(100);
    closeBtn->setCursor(Qt::PointingHandCursor);
    connect(closeBtn, &QPushButton::clicked, &dialog, &QDialog::accept);

    buttonLayout->addWidget(closeBtn);
    layout->addLayout(buttonLayout);

    dialog.exec();
}

// ============================================================================
// Torrent management (add / create)
// ============================================================================

void MainWindow::addTorrentFile()
{
    qInfo() << "Add torrent file dialog opened";

    QString filePath = QFileDialog::getOpenFileName(this, tr("Add Torrent File"),
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation),
        tr("Torrent Files (*.torrent);;All Files (*)"));

    if (filePath.isEmpty())
        return;

    if (!app_->api()) {
        QMessageBox::warning(this, tr("Error"), tr("API not initialized"));
        return;
    }

    app_->api()->call("torrent.import", QJsonObject { { "file", filePath } }, [this, filePath](const Result& response) {
        if (response.ok()) {
            QJsonObject data = response.data().toObject();
            QString name = data["name"].toString();
            bool alreadyExists = data["alreadyExists"].toBool();

            showStatusMessage(
                alreadyExists ? tr("Torrent already in index: %1").arg(name) : tr("Added to index: %1").arg(name),
                3000);

            Torrent t = codec::torrentFromJson(data);
            if (t.isValid())
                addToFavorites(t);

            if (trayIcon && trayIcon->isVisible()) {
                trayIcon->showMessage(tr("Torrent Added"), tr("%1 has been added to the search index").arg(name),
                    QSystemTrayIcon::Information, 3000);
            }
        } else {
            qWarning() << "Failed to add torrent file:" << filePath << "-" << response.error();
            QMessageBox::warning(this, tr("Error"), tr("Failed to add torrent file:\n%1").arg(response.error()));
        }
    });
}

void MainWindow::createTorrent()
{
    qInfo() << "Create torrent dialog opened";

    QMenu menu(this);
    menu.setStyleSheet(this->styleSheet());

    QAction* fileAction = menu.addAction(tr("📄 Create from File..."));
    QAction* folderAction = menu.addAction(tr("📁 Create from Folder..."));
    menu.addSeparator();
    QAction* cancelAction = menu.addAction(tr("❌ Cancel"));

    QAction* selected = menu.exec(QCursor::pos());
    if (selected == cancelAction || selected == nullptr)
        return;

    QString path;
    if (selected == fileAction) {
        path = QFileDialog::getOpenFileName(this, tr("Select File to Create Torrent From"),
            QStandardPaths::writableLocation(QStandardPaths::HomeLocation), tr("All Files (*)"));
    } else if (selected == folderAction) {
        path = QFileDialog::getExistingDirectory(this, tr("Select Folder to Create Torrent From"),
            QStandardPaths::writableLocation(QStandardPaths::HomeLocation));
    }

    if (path.isEmpty())
        return;

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Create Torrent"));
    dialog.setMinimumWidth(450);
    dialog.setStyleSheet(this->styleSheet());

    QVBoxLayout* layout = new QVBoxLayout(&dialog);
    layout->setSpacing(12);
    layout->setContentsMargins(20, 20, 20, 20);

    QLabel* pathLabel = new QLabel(tr("Source: %1").arg(QFileInfo(path).fileName()));
    pathLabel->setWordWrap(true);
    pathLabel->setObjectName("subtitleLabel");
    layout->addWidget(pathLabel);

    QLabel* trackersLabel = new QLabel(tr("Trackers (one per line, optional):"));
    layout->addWidget(trackersLabel);

    QTextEdit* trackersEdit = new QTextEdit();
    trackersEdit->setObjectName("monoEdit"); // one URL per line reads better fixed-width
    trackersEdit->setPlaceholderText("udp://tracker.example.com:6969/announce\nhttp://tracker2.example.com/"
                                     "announce");
    trackersEdit->setMaximumHeight(80);
    layout->addWidget(trackersEdit);

    QLabel* commentLabel = new QLabel(tr("Comment (optional):"));
    layout->addWidget(commentLabel);

    QLineEdit* commentEdit = new QLineEdit();
    commentEdit->setPlaceholderText(tr("Created with Rats Search"));
    layout->addWidget(commentEdit);

    QCheckBox* seedCheckBox = new QCheckBox(tr("Start seeding immediately"));
    seedCheckBox->setChecked(true);
    layout->addWidget(seedCheckBox);

    QProgressBar* progressBar = new QProgressBar();
    progressBar->setVisible(false);
    progressBar->setFormat(tr("Hashing pieces... %p%"));
    layout->addWidget(progressBar);

    QLabel* statusLabel = new QLabel();
    statusLabel->setObjectName("hintLabel");
    layout->addWidget(statusLabel);

    layout->addStretch();

    QHBoxLayout* buttonLayout = new QHBoxLayout();
    QPushButton* cancelBtn = new QPushButton(tr("Cancel"));
    QPushButton* createBtn = new QPushButton(tr("🔨 Create Torrent"));
    createBtn->setObjectName("primaryButton");

    buttonLayout->addStretch();
    buttonLayout->addWidget(cancelBtn);
    buttonLayout->addWidget(createBtn);
    layout->addLayout(buttonLayout);

    connect(cancelBtn, &QPushButton::clicked, &dialog, &QDialog::reject);

    connect(createBtn, &QPushButton::clicked, [&]() {
        if (!app_->creator()) {
            QMessageBox::warning(this, tr("Error"), tr("Torrent creator not available"));
            return;
        }

        createBtn->setEnabled(false);
        cancelBtn->setEnabled(false);
        progressBar->setVisible(true);
        progressBar->setValue(0);
        statusLabel->setText(tr("Creating torrent..."));

        QStringList trackers;
        QString trackersText = trackersEdit->toPlainText().trimmed();
        if (!trackersText.isEmpty()) {
            trackers = trackersText.split('\n', Qt::SkipEmptyParts);
            for (QString& t : trackers)
                t = t.trimmed();
            trackers.removeAll("");
        }

        QString comment = commentEdit->text().trimmed();
        if (comment.isEmpty())
            comment = "Created with Rats Search";

        auto progressCallback = [progressBar](int current, int total) {
            if (total > 0) {
                int percent = (current * 100) / total;
                QMetaObject::invokeMethod(
                    progressBar, [progressBar, percent]() { progressBar->setValue(percent); }, Qt::QueuedConnection);
            }
        };

        // TorrentCreator hashes the content, starts seeding and registers the
        // download. This is a blocking call (hashing) — acceptable for the modal.
        QString hash = app_->creator()->createAndSeed(path, trackers, comment,
            /*saveTorrentFilePath*/ QString(), progressCallback);

        if (!hash.isEmpty()) {
            statusLabel->setText(tr("✅ Torrent created successfully!"));
            QMessageBox::information(
                &dialog, tr("Torrent Created"), tr("Torrent created and seeding.\n\nHash: %1").arg(hash));
            dialog.accept();
        } else {
            statusLabel->setText(tr("❌ Failed to create torrent"));
            createBtn->setEnabled(true);
            cancelBtn->setEnabled(true);
            QMessageBox::warning(&dialog, tr("Error"), tr("Failed to create torrent."));
        }
    });

    dialog.exec();
}

// ============================================================================
// Whole-database replication
// ============================================================================

QString MainWindow::databaseSyncStatusText(const QJsonObject& info) const
{
    const QString stage = info["stage"].toString();
    const qint64 processed = info["processed"].toVariant().toLongLong();
    const qint64 total = info["total"].toVariant().toLongLong();
    const qint64 bytes = info["bytes"].toVariant().toLongLong();
    const qint64 totalBytes = info["totalBytes"].toVariant().toLongLong();

    if (stage == QLatin1String("transferring")) {
        // Worth saying out loud: a pull that broke at 6 of 9 GB shows 66% from its
        // first second, and without a word for it that reads like a stuck transfer.
        const bool continued = info["resumedFrom"].toVariant().toLongLong() > 0;
        if (totalBytes > 0) {
            return (continued ? tr("Continuing database transfer: %1 / %2 (%3%)")
                              : tr("Transferring database: %1 / %2 (%3%)"))
                .arg(rats::ui::formatSize(bytes), rats::ui::formatSize(totalBytes))
                .arg((bytes * 100) / totalBytes);
        }
        return (continued ? tr("Continuing database transfer: %1") : tr("Transferring database: %1"))
            .arg(rats::ui::formatSize(bytes));
    }
    if (stage == QLatin1String("waiting") || stage == QLatin1String("preparing"))
        return tr("Waiting for the peer to prepare its database…");

    const QString what = stage == QLatin1String("exporting") ? tr("Exporting database") : tr("Importing database");
    if (total > 0)
        return tr("%1: %2 / %3 (%4%)").arg(what).arg(processed).arg(total).arg((processed * 100) / total);
    return tr("%1: %n torrent(s)", nullptr, static_cast<int>(processed)).arg(what);
}

QString MainWindow::databaseServeStatusText(const QJsonObject& info) const
{
    const int sending = info["sending"].toInt();
    const int waiting = info["waiting"].toInt();

    if (info["generating"].toBool()) {
        const qint64 processed = info["processed"].toVariant().toLongLong();
        const qint64 total = info["total"].toVariant().toLongLong();
        if (total > 0) {
            return tr("Preparing the database for %n peer(s): %1%", nullptr, qMax(1, waiting))
                .arg((processed * 100) / total);
        }
        return tr("Preparing the database for %n peer(s)...", nullptr, qMax(1, waiting));
    }
    if (sending > 0)
        return tr("Sharing the database with %n peer(s)...", nullptr, sending);
    // Nothing to say: leave whatever the user's own operation put there.
    return QString();
}

void MainWindow::exportDatabase()
{
    if (!app_->api())
        return;

    const QString suggested = QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
                                  .absoluteFilePath(QStringLiteral("rats-index-%1.ratsdb")
                                          .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd"))));

    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export Database"), suggested, tr("Rats Database (*.ratsdb);;All Files (*)"));
    if (path.isEmpty())
        return;

    app_->api()->call("database.export", QJsonObject { { "path", path } }, [this](const Result& response) {
        if (!response.ok()) {
            QMessageBox::warning(
                this, tr("Export Database"), tr("Could not start the export.\n\n%1").arg(response.error()));
        }
        // Progress and the final summary arrive through the sync service signals
        // wired in the constructor.
    });
}

void MainWindow::importDatabase()
{
    if (!app_->api())
        return;

    const QString path = QFileDialog::getOpenFileName(this, tr("Import Database"),
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation),
        tr("Rats Database (*.ratsdb);;All Files (*)"));
    if (path.isEmpty())
        return;

    // Importing merges — nothing local is replaced — but a foreign index can be
    // huge and carry content this user filters out, so both facts are stated
    // before it starts.
    QMessageBox confirm(this);
    confirm.setIcon(QMessageBox::Question);
    confirm.setWindowTitle(tr("Import Database"));
    confirm.setText(tr("Merge this database into your index?"));
    confirm.setInformativeText(
        tr("Torrents you already have are kept; only new ones are added.\n\n%1").arg(QFileInfo(path).fileName()));
    confirm.setStandardButtons(QMessageBox::Yes | QMessageBox::Cancel);
    confirm.setDefaultButton(QMessageBox::Yes);

    QCheckBox* filterBox = new QCheckBox(tr("Apply my content filters to the imported torrents"), &confirm);
    filterBox->setChecked(true);
    confirm.setCheckBox(filterBox);

    if (confirm.exec() != QMessageBox::Yes)
        return;

    app_->api()->call("database.import", QJsonObject { { "path", path }, { "applyFilters", filterBox->isChecked() } },
        [this](const Result& response) {
            if (!response.ok()) {
                QMessageBox::warning(
                    this, tr("Import Database"), tr("Could not start the import.\n\n%1").arg(response.error()));
            }
        });
}

void MainWindow::pullDatabaseFromPeer()
{
    if (!app_->api())
        return;

    // database.peers, not peers.list: a peer that does not share its database can
    // only answer with a refusal, so offering it as a choice would be a dead end.
    app_->api()->call("database.peers", QJsonObject {}, [this](const Result& response) {
        if (!response.ok()) {
            QMessageBox::warning(this, tr("Download Database"), response.error());
            return;
        }

        const QJsonArray peers = response.data().toArray();
        if (peers.isEmpty()) {
            QMessageBox::information(this, tr("Download Database"),
                tr("None of the connected peers is sharing its database.\n\n"
                   "Sharing is something each user enables for themselves, and older clients do not support "
                   "it at all. Try again later, or import a database file instead."));
            return;
        }

        // Label each peer with what it claims to hold, so the choice is informed
        // rather than a list of hex ids.
        QStringList labels;
        QStringList ids;
        for (const QJsonValue& value : peers) {
            const QJsonObject peer = value.toObject();
            const QString id = peer["peerId"].toString();
            const qint64 torrents = peer["torrents"].toVariant().toLongLong();
            ids << id;
            labels << tr("%1… — %n torrent(s) (%2)", nullptr, static_cast<int>(torrents))
                          .arg(id.left(12))
                          .arg(peer["clientVersion"].toString());
        }

        bool accepted = false;
        const QString chosen = QInputDialog::getItem(
            this, tr("Download Database"), tr("Ask which peer for its whole index?"), labels, 0, false, &accepted);
        if (!accepted || chosen.isEmpty())
            return;

        const int index = labels.indexOf(chosen);
        if (index < 0)
            return;

        app_->api()->call("database.pull", QJsonObject { { "peer", ids.at(index) } }, [this](const Result& result) {
            if (!result.ok()) {
                QMessageBox::warning(
                    this, tr("Download Database"), tr("Could not ask the peer.\n\n%1").arg(result.error()));
            }
        });
    });
}

// ============================================================================
// System tray
// ============================================================================

void MainWindow::setupSystemTray()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        qWarning() << "System tray is not available";
        return;
    }

    trayIcon = new QSystemTrayIcon(this);
    trayIcon->setIcon(QIcon(":/images/icon.png"));
    trayIcon->setToolTip(tr("Rats Search - P2P Torrent Search Engine"));

    trayMenu = new QMenu(this);
    trayMenu->setStyleSheet(this->styleSheet());

    QAction* showAction = trayMenu->addAction(tr("Show Window"));
    connect(showAction, &QAction::triggered, this, &MainWindow::toggleWindowVisibility);

    trayMenu->addSeparator();

    QAction* settingsAction = trayMenu->addAction(tr("Settings"));
    connect(settingsAction, &QAction::triggered, [this]() {
        show();
        activateWindow();
        showSettings();
    });

    trayMenu->addSeparator();

    QAction* quitAction = trayMenu->addAction(tr("Quit"));
    connect(quitAction, &QAction::triggered, [this]() {
        forceQuit_ = true;
        close();
    });

    trayIcon->setContextMenu(trayMenu);
    connect(trayIcon, &QSystemTrayIcon::activated, this, &MainWindow::onTrayIconActivated);
    trayIcon->show();
}

void MainWindow::onTrayIconActivated(QSystemTrayIcon::ActivationReason reason)
{
    switch (reason) {
    case QSystemTrayIcon::Trigger:
    case QSystemTrayIcon::DoubleClick:
        toggleWindowVisibility();
        break;
    default:
        break;
    }
}

void MainWindow::toggleWindowVisibility()
{
    if (isVisible() && !isMinimized()) {
        hiddenToTray_ = true;
        hide();
    } else {
        bringToFront();
    }
}

void MainWindow::bringToFront()
{
    hiddenToTray_ = false;

    // Clear the minimized native-window state BEFORE making a hidden window
    // visible. The previous show() -> clear-minimized order can produce an empty
    // NSWindow surface after a minimize-to-tray cycle on macOS.
    if (isMinimized())
        setWindowState(windowState() & ~Qt::WindowMinimized);

    show();

    // Defensive re-exposure of the central hierarchy. Qt normally keeps these
    // widgets visible while the top-level window is hidden, but explicitly
    // restoring them makes Dock/tray activation deterministic on macOS.
    if (centralWidget()) {
        centralWidget()->show();
        if (centralWidget()->layout())
            centralWidget()->layout()->activate();
    }
    if (verticalSplitter)
        verticalSplitter->show();
    if (tabWidget)
        tabWidget->show();

    raise();
    activateWindow();
    update();
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
    // Clicking (or tabbing into) an empty search field shows the recent
    // queries, the way a browser address bar does. The popup is deferred to the
    // next event-loop turn: opening it from inside the press handler would let
    // the very same click close it again.
    if (watched == searchLineEdit && searchLineEdit->text().isEmpty()) {
        bool wantsHistory = event->type() == QEvent::MouseButtonPress;
        // Only *deliberate* keyboard navigation counts — the search field also
        // takes focus when the window opens, and greeting the user with a
        // dropdown at launch is not what they asked for.
        if (event->type() == QEvent::FocusIn)
            wantsHistory = static_cast<QFocusEvent*>(event)->reason() == Qt::TabFocusReason;
        if (wantsHistory)
            QTimer::singleShot(0, this, &MainWindow::showSearchHistoryPopup);
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::changeEvent(QEvent* event)
{
    QMainWindow::changeEvent(event);

    if (event->type() == QEvent::WindowStateChange) {
        bool minimizeToTray = app_ && app_->config() ? app_->config()->trayOnMinimize() : false;
        if (isMinimized() && minimizeToTray && trayIcon && trayIcon->isVisible()) {
            hiddenToTray_ = true;
            QTimer::singleShot(0, this, &QWidget::hide);
            if (trayIcon && !trayNotificationShown_) {
                trayIcon->showMessage(tr("Rats Search"), tr("Application minimized to tray. Click to restore."),
                    QSystemTrayIcon::Information, 2000);
                trayNotificationShown_ = true;
            }
        }
    }
}

// ============================================================================
// UI settings persistence. Application behaviour settings remain in ConfigStore;
// this block owns only per-window/view state.
// ============================================================================

void MainWindow::loadSettings()
{
    QSettings s("RatsSearch", "RatsSearch");

    if (s.contains("window/geometry"))
        restoreGeometry(s.value("window/geometry").toByteArray());
    if (s.contains("window/state"))
        restoreState(s.value("window/state").toByteArray());

    if (resultsTableView) {
        QHeaderView* header = resultsTableView->horizontalHeader();
        s.remove(QStringLiteral("search/headerState"));

        if (s.contains("search/headerSortColumn")) {
            const int column = s.value("search/headerSortColumn").toInt();
            const auto order = static_cast<Qt::SortOrder>(
                s.value("search/headerSortOrder", static_cast<int>(Qt::DescendingOrder)).toInt());
            if (column >= 0 && column < SearchResultModel::ColumnCount)
                resultsTableView->sortByColumn(column, order);
        }
    }

    if (mainSplitter && s.contains("splitters/main"))
        mainSplitter->restoreState(s.value("splitters/main").toByteArray());
    if (verticalSplitter && s.contains("splitters/vertical"))
        verticalSplitter->restoreState(s.value("splitters/vertical").toByteArray());

    if (typeComboBox) {
        const QString savedType
            = s.value(QStringLiteral("search/contentType"), QString()).toString();
        const int index = typeComboBox->findData(savedType);
        typeComboBox->setCurrentIndex(index >= 0 ? index : 0);
    }
    if (sortComboBox) {
        const int index = sortComboBox->findData(s.value("search/order", QStringLiteral("seeders_desc")).toString());
        if (index >= 0)
            sortComboBox->setCurrentIndex(index);
    }

    auto restoreUnit = [&s](QComboBox* combo, const char* key) {
        if (!combo || !s.contains(key))
            return;
        const qint64 multiplier = s.value(key).toLongLong();
        const int index = combo->findData(QVariant::fromValue<qint64>(multiplier));
        if (index >= 0)
            combo->setCurrentIndex(index);
    };
    restoreUnit(sizeMinUnit, "filters/sizeMinUnit");
    restoreUnit(sizeMaxUnit, "filters/sizeMaxUnit");

    if (sizeMinSpin)
        sizeMinSpin->setValue(s.value("filters/sizeMin", 0.0).toDouble());
    if (sizeMaxSpin)
        sizeMaxSpin->setValue(s.value("filters/sizeMax", 0.0).toDouble());
    if (filesMinSpin)
        filesMinSpin->setValue(s.value("filters/filesMin", 0).toInt());
    if (filesMaxSpin)
        filesMaxSpin->setValue(s.value("filters/filesMax", 0).toInt());

    if (tabWidget) {
        const int tab = s.value("tabs/current", 0).toInt();
        if (tab >= 0 && tab < tabWidget->count())
            tabWidget->setCurrentIndex(tab);
    }

    updateSearchFiltersButton();
    qInfo() << "UI settings loaded";
}

void MainWindow::saveSettings()
{
    if (app_ && app_->config())
        app_->config()->save();

    QSettings s("RatsSearch", "RatsSearch");
    s.setValue("window/geometry", saveGeometry());
    s.setValue("window/state", saveState());

    if (resultsTableView) {
        QHeaderView* header = resultsTableView->horizontalHeader();
        s.setValue("search/headerSortColumn", header->sortIndicatorSection());
        s.setValue("search/headerSortOrder", static_cast<int>(header->sortIndicatorOrder()));
    }

    if (mainSplitter)
        s.setValue("splitters/main", mainSplitter->saveState());
    if (verticalSplitter)
        s.setValue("splitters/vertical", verticalSplitter->saveState());

    if (typeComboBox)
        s.setValue("search/contentType", typeComboBox->currentData().toString());
    if (sortComboBox)
        s.setValue("search/order", sortComboBox->currentData().toString());

    if (sizeMinSpin)
        s.setValue("filters/sizeMin", sizeMinSpin->value());
    if (sizeMaxSpin)
        s.setValue("filters/sizeMax", sizeMaxSpin->value());
    if (sizeMinUnit)
        s.setValue("filters/sizeMinUnit", sizeMinUnit->currentData().toLongLong());
    if (sizeMaxUnit)
        s.setValue("filters/sizeMaxUnit", sizeMaxUnit->currentData().toLongLong());
    if (filesMinSpin)
        s.setValue("filters/filesMin", filesMinSpin->value());
    if (filesMaxSpin)
        s.setValue("filters/filesMax", filesMaxSpin->value());

    if (tabWidget)
        s.setValue("tabs/current", tabWidget->currentIndex());

    s.sync();
    qInfo() << "UI settings saved";
}

// ============================================================================
// Update management
// ============================================================================

void MainWindow::checkForUpdates()
{
    if (!app_->updates())
        return;
    auto* updates = app_->updates();

    showStatusMessage(tr("Checking for updates..."), 3000);

    disconnect(updates, &UpdateService::noUpdateAvailable, nullptr, nullptr);

    connect(
        updates, &UpdateService::noUpdateAvailable, this,
        [this]() {
            QMessageBox::information(this, tr("No Updates Available"),
                tr("You are running the latest version of Rats Search (%1).").arg(UpdateService::currentVersion()));
        },
        Qt::SingleShotConnection);

    updates->checkForUpdates();
}

void MainWindow::onUpdateAvailable(const QString& version, const QString& releaseNotes)
{
    auto* updates = app_->updates();
    if (!updates)
        return;

    qInfo() << "Update available:" << version << "current:" << UpdateService::currentVersion();

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Update Available"));
    dialog.setMinimumSize(500, 400);
    dialog.setStyleSheet(this->styleSheet());

    QVBoxLayout* layout = new QVBoxLayout(&dialog);
    layout->setSpacing(16);
    layout->setContentsMargins(24, 24, 24, 24);

    QLabel* headerLabel = new QLabel(QString("🎉 %1").arg(tr("New Version Available!")));
    headerLabel->setObjectName("headerLabel");
    layout->addWidget(headerLabel);

    QLabel* versionLabel = new QLabel(tr("A new version of Rats Search is available.\n\n"
                                         "Current version: %1\n"
                                         "New version: %2")
            .arg(UpdateService::currentVersion(), version));
    layout->addWidget(versionLabel);

    if (!releaseNotes.isEmpty()) {
        QLabel* notesHeaderLabel = new QLabel(tr("What's new:"));
        layout->addWidget(notesHeaderLabel);

        QTextEdit* notesEdit = new QTextEdit();
        notesEdit->setReadOnly(true);
        notesEdit->setMarkdown(releaseNotes);
        notesEdit->setMaximumHeight(150);
        layout->addWidget(notesEdit);
    }

    QProgressBar* progressBar = new QProgressBar();
    progressBar->setVisible(false);
    progressBar->setTextVisible(true);
    progressBar->setFormat(tr("Downloading... %p%"));
    layout->addWidget(progressBar);

    QLabel* statusLabel = new QLabel();
    statusLabel->setObjectName("subtitleLabel");
    layout->addWidget(statusLabel);

    layout->addStretch();

    QHBoxLayout* buttonLayout = new QHBoxLayout();

    QPushButton* laterBtn = new QPushButton(tr("Remind Me Later"));
    laterBtn->setObjectName("secondaryButton");

    QPushButton* downloadBtn = new QPushButton(tr("Download && Install"));
    downloadBtn->setObjectName("successButton");

    buttonLayout->addWidget(laterBtn);
    buttonLayout->addStretch();
    buttonLayout->addWidget(downloadBtn);
    layout->addLayout(buttonLayout);

    connect(laterBtn, &QPushButton::clicked, &dialog, &QDialog::reject);

    connect(downloadBtn, &QPushButton::clicked, [&]() {
        downloadBtn->setEnabled(false);
        laterBtn->setText(tr("Cancel"));
        progressBar->setVisible(true);
        statusLabel->setText(tr("Starting download..."));

        connect(updates, &UpdateService::downloadProgressChanged, progressBar, &QProgressBar::setValue);

        connect(updates, &UpdateService::stateChanged, [statusLabel](UpdateService::UpdateState state) {
            switch (state) {
            case UpdateService::UpdateState::Downloading:
                statusLabel->setText(tr("Downloading update..."));
                break;
            case UpdateService::UpdateState::Extracting:
                statusLabel->setText(tr("Extracting update..."));
                break;
            case UpdateService::UpdateState::ReadyToInstall:
                statusLabel->setText(tr("Ready to install!"));
                break;
            case UpdateService::UpdateState::Error:
                statusLabel->setText(tr("Error occurred"));
                break;
            default:
                break;
            }
        });

        updates->downloadUpdate();
    });

    // Just close the dialog; the install prompt is driven by the global
    // updateReady handler wired in connectServiceSignals().
    connect(updates, &UpdateService::updateReady, &dialog, &QDialog::accept);

    connect(
        updates, &UpdateService::errorOccurred, &dialog, [statusLabel, downloadBtn, laterBtn](const QString& error) {
            statusLabel->setText(tr("Error: %1").arg(error));
            downloadBtn->setEnabled(true);
            downloadBtn->setText(tr("Retry"));
            laterBtn->setText(tr("Close"));
        });

    dialog.exec();
}

void MainWindow::onUpdateDownloadProgress(int percent)
{
    showStatusMessage(tr("Downloading update: %1%").arg(percent), 1000);
}

void MainWindow::onUpdateReady()
{
    qInfo() << "Update downloaded and ready to install";

    QMessageBox::StandardButton reply = QMessageBox::question(this, tr("Install Update"),
        tr("The update has been downloaded and is ready to install.\n\n"
           "The application will close and restart automatically.\n\n"
           "Do you want to install the update now?"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);

    if (reply == QMessageBox::Yes) {
        qInfo() << "User accepted update installation, preparing to restart...";
        saveSettings();

        // The service only launches the external updater; shutting the app down
        // is the frontend's job. Bail out if it failed to start so we don't close
        // the app with no updater running.
        if (!app_->updates() || !app_->updates()->executeUpdateScript())
            return;

        // From here the shutdown is non-negotiable: mark it so closeEvent() stops
        // prompting or hiding to tray. closeAllWindows() then tears every window
        // down — including any nested modal loop (e.g. the download dialog still
        // in exec()) — and each window's closeEvent drives the actual quit.
        // main()'s exec() returns and app_->stop() releases the database/searchd
        // locks; the updater, blocked waiting for us to exit, then proceeds.
        updateInstalling_ = true;
        QApplication::closeAllWindows();
    }
}

void MainWindow::onUpdateError(const QString& error)
{
    showStatusMessage(tr("Update error: %1").arg(error), 5000);
}

// ============================================================================
// GitHub helpers
// ============================================================================

void MainWindow::reportBug()
{
    QString body
        = QString("**Describe the bug**\n"
                  "A clear description of what the bug is.\n\n"
                  "**To Reproduce**\n"
                  "Steps to reproduce the behavior:\n"
                  "1. Go to '...'\n"
                  "2. Click on '...'\n"
                  "3. See error\n\n"
                  "**Expected behavior**\n"
                  "What you expected to happen.\n\n"
                  "**Screenshots**\n"
                  "If applicable, add screenshots.\n\n"
                  "**Environment:**\n"
                  "- App Version: %1\n"
                  "- Git: %2\n"
                  "- OS: %3\n"
                  "- Qt: %4\n")
              .arg(RATSSEARCH_VERSION_STRING, RATSSEARCH_GIT_DESCRIBE, QSysInfo::prettyProductName(), qVersion());

    QUrl url("https://github.com/DEgITx/rats-search/issues/new");
    QUrlQuery query;
    query.addQueryItem("labels", "bug");
    query.addQueryItem("title", "[Bug] ");
    query.addQueryItem("body", body);
    url.setQuery(query);

    QDesktopServices::openUrl(url);
}

void MainWindow::requestFeature()
{
    QString body = QString("**Is your feature request related to a problem?**\n"
                           "A clear description of the problem.\n\n"
                           "**Describe the solution you'd like**\n"
                           "What you want to happen.\n\n"
                           "**Additional context**\n"
                           "Any other context or screenshots.\n\n"
                           "---\n"
                           "App Version: %1\n")
                       .arg(RATSSEARCH_VERSION_STRING);

    QUrl url("https://github.com/DEgITx/rats-search/issues/new");
    QUrlQuery query;
    query.addQueryItem("labels", "enhancement");
    query.addQueryItem("title", "[Feature] ");
    query.addQueryItem("body", body);
    url.setQuery(query);

    QDesktopServices::openUrl(url);
}

// ============================================================================
// End User License Agreement
// ============================================================================

bool MainWindow::showAgreementDialog()
{
    qInfo() << "Showing End User License Agreement dialog";

    rats::app::ConfigStore* config = app_->config();

    QDialog dialog;
    dialog.setWindowTitle(tr("End User License Agreement"));
    dialog.setMinimumSize(700, 600);
    dialog.setModal(true);

    // Shown before MainWindow is styled, so it cannot inherit a sheet.
    rats::ui::Theme::instance().setDark(config && config->darkMode());
    dialog.setStyleSheet(rats::ui::Theme::instance().styleSheet());

    QVBoxLayout* layout = new QVBoxLayout(&dialog);
    layout->setSpacing(16);
    layout->setContentsMargins(24, 24, 24, 24);

    QHBoxLayout* headerLayout = new QHBoxLayout();

    QLabel* logoLabel = new QLabel("🐀");
    logoLabel->setObjectName("aboutLogoLabel");
    headerLayout->addWidget(logoLabel);

    QLabel* titleLabel = new QLabel(tr("Rats Search - License Agreement"));
    titleLabel->setObjectName("aboutTitleLabel");
    headerLayout->addWidget(titleLabel);
    headerLayout->addStretch();
    layout->addLayout(headerLayout);

    QLabel* subtitleLabel = new QLabel(tr("Please read and accept the following End User License "
                                          "Agreement before using this software."));
    subtitleLabel->setWordWrap(true);
    subtitleLabel->setObjectName("subtitleLabel");
    layout->addWidget(subtitleLabel);

    QTextEdit* agreementText = new QTextEdit();
    agreementText->setReadOnly(true);
    agreementText->setMinimumHeight(350);

    QString agreementContent;
    QFile resourceFile(":/AGREEMENT.md");
    if (resourceFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        agreementContent = QString::fromUtf8(resourceFile.readAll());
        resourceFile.close();
    } else {
        QString appPath = QApplication::applicationDirPath();
        QFile localFile(appPath + "/AGREEMENT.md");
        if (localFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            agreementContent = QString::fromUtf8(localFile.readAll());
            localFile.close();
        } else {
            QFile devFile("AGREEMENT.md");
            if (devFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
                agreementContent = QString::fromUtf8(devFile.readAll());
                devFile.close();
            }
        }
    }

    if (agreementContent.isEmpty()) {
        agreementContent = R"(# END USER LICENSE AGREEMENT

**IMPORTANT — READ CAREFULLY BEFORE USING THIS SOFTWARE**

By using Rats Search, you acknowledge and agree that:

1. **ASSUMPTION OF RISK**: You assume ALL risks associated with the use of this Software, including legal, security, and privacy risks.

2. **CONTENT RESPONSIBILITY**: You are SOLELY responsible for determining the legality of any content you access, download, or share.

3. **NO WARRANTIES**: The Software is provided "AS IS" without warranty of any kind.

4. **NO LIABILITY**: The Developers shall not be liable for any damages arising from your use of the Software.

5. **COMPLIANCE**: You agree to comply with all applicable laws and regulations.

6. **INDEMNIFICATION**: You agree to indemnify and hold harmless the Developers from any claims arising from your use of the Software.

**IF YOU DO NOT AGREE TO THESE TERMS, DO NOT USE THIS SOFTWARE.**
)";
    }

    agreementText->setMarkdown(agreementContent);
    layout->addWidget(agreementText, 1);

    QCheckBox* readCheckbox = new QCheckBox(tr("I have read and understood the End User License Agreement"));
    layout->addWidget(readCheckbox);

    QLabel* warningLabel = new QLabel(tr("⚠️ By clicking 'I Accept', you acknowledge that you have "
                                         "read, understood, and agree to be bound by all "
                                         "terms and conditions of this Agreement. You accept full "
                                         "responsibility for your use of this Software."));
    warningLabel->setWordWrap(true);
    warningLabel->setObjectName("hintLabel");
    layout->addWidget(warningLabel);

    QHBoxLayout* buttonLayout = new QHBoxLayout();

    QPushButton* declineBtn = new QPushButton(tr("Decline && Exit"));
    declineBtn->setObjectName("dangerButton");
    declineBtn->setMinimumWidth(140);
    declineBtn->setCursor(Qt::PointingHandCursor);

    QPushButton* acceptBtn = new QPushButton(tr("I Accept"));
    acceptBtn->setObjectName("successButton");
    acceptBtn->setMinimumWidth(140);
    acceptBtn->setEnabled(false);
    acceptBtn->setCursor(Qt::PointingHandCursor);

    buttonLayout->addWidget(declineBtn);
    buttonLayout->addStretch();
    buttonLayout->addWidget(acceptBtn);
    layout->addLayout(buttonLayout);

    connect(readCheckbox, &QCheckBox::toggled, acceptBtn, &QPushButton::setEnabled);

    bool accepted = false;
    connect(declineBtn, &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(acceptBtn, &QPushButton::clicked, [&dialog, &accepted]() {
        accepted = true;
        dialog.accept();
    });

    dialog.exec();

    if (accepted) {
        qInfo() << "User accepted the End User License Agreement";
        if (config)
            config->setAgreementAccepted(true);
        return true;
    }
    qInfo() << "User declined the End User License Agreement";
    return false;
}
