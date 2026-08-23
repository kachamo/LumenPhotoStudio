// ==============================================================================
// ui/LibraryView.cpp
//
// The Library workspace: folder list + import on the left, filter bar on top,
// thumbnail grid in the centre, status line at the bottom.
//
// Threading
// ---------
// CatalogDatabase is documented as "one instance per thread", so this view
// keeps its single instance on a private worker thread (CatalogWorker) and
// never touches SQLite from the UI thread. Queries, counts, stats and the
// small rating/flag writes are all posted to that thread and their results
// posted back; the only blocking call is openCatalog(), which has to return a
// bool synchronously and is a one-shot at startup.
//
// Every cross-thread hop uses QMetaObject::invokeMethod with a functor rather
// than a signal, which keeps catalog structs out of the metatype system
// entirely and makes the stale-result rule explicit: each reload carries a
// token and anything but the newest token is dropped on arrival.
//
// Empty states
// ------------
// The centre is a QStackedWidget, never a bare grid: no catalog, no photos
// yet, and no matches for the current filter each get a page that says what
// happened and offers the obvious next action.
// ==============================================================================
#include "LibraryView.h"

#include "CatalogGridView.h"

#include "catalog/CatalogDatabase.h"
#include "catalog/CatalogImporter.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QLocale>
#include <QMetaObject>
#include <QPainter>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QSplitter>
#include <QStackedWidget>
#include <QString>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVector>
#include <QWidget>

#include <algorithm>

namespace {

// A quarter of a second: long enough that a fast typist issues one query
// instead of twenty, short enough that the grid still feels live.
constexpr int kSearchDebounceMs = 250;

// Hard ceiling on rows handed to the grid. The grid itself scales further, but
// a runaway catalog should not turn one keystroke into a gigabyte of structs.
constexpr int kMaxGridImages = 50000;

// Progress signals can arrive once per file; repainting the bar 40,000 times
// would cost more than the import.
constexpr int kProgressPaintMs = 80;

// statusMessage() is rate limited to this interval so a burst of row updates
// cannot flood MainWindow's status bar.
constexpr int kStatusEmitMs = 120;

// Empty-state prose is capped so it never stretches across a 4K viewport.
constexpr int kEmptyTextWidth = 520;

constexpr int kMinThumbPx = 96;
constexpr int kMaxThumbPx = 320;

QString styleSheetForLibrary()
{
    return QStringLiteral(R"(
        QWidget#libraryRoot {
            background: #0E0F12;
            color: #E7E9EE;
        }
        QFrame#libraryCard {
            background: #16181D;
            border: 1px solid #2A2D35;
            border-radius: 10px;
        }
        QFrame#libraryBar {
            background: #111318;
            border: 1px solid #2A2D35;
            border-radius: 10px;
        }
        QLabel#librarySectionTitle {
            color: #9EA4AE;
            font-size: 11px;
        }
        QLabel#libraryStatus {
            color: #9EA4AE;
            padding: 2px 4px;
        }
        QLabel#emptyTitle {
            color: #E7E9EE;
        }
        QLabel#emptyBody {
            color: #9EA4AE;
        }
        QLabel#progressCaption {
            color: #9EA4AE;
            font-size: 11px;
        }
        QProgressBar {
            background: #111318;
            border: 1px solid #2A2D35;
            border-radius: 6px;
            height: 8px;
            text-align: center;
            color: #9EA4AE;
        }
        QProgressBar::chunk {
            background: #CCFF00;
            border-radius: 5px;
        }
        QLabel {
            color: #DDE0E7;
            background: transparent;
        }
        QLineEdit, QComboBox {
            background: #111318;
            color: #E7E9EE;
            border: 1px solid #2A2D35;
            border-radius: 7px;
            padding: 5px 8px;
        }
        QLineEdit:focus, QComboBox:focus {
            border-color: #CCFF00;
        }
        QComboBox QAbstractItemView {
            background: #16181D;
            color: #E7E9EE;
            border: 1px solid #2A2D35;
            selection-background-color: #22262C;
            selection-color: #CCFF00;
        }
        QPushButton, QToolButton {
            background: #1E2026;
            color: #E7E9EE;
            border: 1px solid #2A2D35;
            border-radius: 7px;
            padding: 5px 9px;
        }
        QPushButton:hover, QToolButton:hover {
            background: #24272E;
            border-color: #3D424E;
            color: #FFFFFF;
        }
        QPushButton:disabled, QToolButton:disabled {
            background: #15171B;
            color: #666A72;
            border-color: #24272B;
        }
        QToolButton:checked {
            background: rgba(204, 255, 0, 31);
            color: #CCFF00;
            border-color: #CCFF00;
        }
        QListWidget {
            background: #111318;
            color: #E7E9EE;
            border: 1px solid #2A2D35;
            border-radius: 8px;
            padding: 4px;
            outline: 0;
        }
        QListWidget::item {
            padding: 5px 6px;
            border-radius: 5px;
        }
        QListWidget::item:selected {
            background: rgba(204, 255, 0, 36);
            color: #FFFFFF;
        }
        QSlider::groove:horizontal {
            height: 3px;
            background: #2A2D35;
            border-radius: 2px;
        }
        QSlider::sub-page:horizontal {
            background: #CCFF00;
            border-radius: 2px;
        }
        QSlider::handle:horizontal {
            width: 12px;
            height: 12px;
            margin: -5px 0;
            border-radius: 6px;
            background: #8B929D;
            border: 1px solid #B0B7C2;
        }
        QSlider::handle:horizontal:hover {
            background: #CCFF00;
            border-color: #CCFF00;
        }
        QScrollBar:vertical {
            background: transparent;
            width: 8px;
            margin: 4px 0;
        }
        QScrollBar::handle:vertical {
            background: #2A2D35;
            border-radius: 4px;
            min-height: 28px;
        }
        QScrollBar::handle:vertical:hover {
            background: #3D424E;
        }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
            height: 0;
        }
        QSplitter::handle {
            background: transparent;
        }
    )");
}

QLabel* makeSectionTitle(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text.toUpper(), parent);
    label->setObjectName(QStringLiteral("librarySectionTitle"));
    QFont font = label->font();
    font.setBold(true);
    label->setFont(font);
    return label;
}

QIcon labelSwatch(const QColor& color)
{
    QPixmap pixmap(12, 12);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(QColor(0x2A, 0x2D, 0x35), 1.0));
    painter.setBrush(color);
    painter.drawRoundedRect(QRectF(0.5, 0.5, 11.0, 11.0), 3.0, 3.0);
    return QIcon(pixmap);
}

QString formatCount(int value)
{
    return QLocale().toString(value);
}

// ==============================================================================
// CatalogSnapshot
// Everything one reload needs, gathered on the worker thread in a single trip.
// ==============================================================================
struct CatalogSnapshot
{
    bool                        ok = false;
    QString                     error;
    QVector<lps::CatalogFolder> folders;
    QVector<lps::CatalogImage>  images;
    lps::CatalogStats           stats;
    int                         matchCount = 0;
    bool                        truncated  = false;
};

} // namespace

// ==============================================================================
// CatalogWorker
//
// Owns the CatalogDatabase and lives on a private QThread. Every method here
// runs on that thread; the database object is created there too, because
// QSqlDatabase connections belong to the thread that opened them.
// ==============================================================================
class CatalogWorker final : public QObject
{
    Q_OBJECT

public:
    CatalogWorker() = default;

    ~CatalogWorker() override
    {
        // The owner shuts the thread down before deleting us, so this only
        // catches the case where open() was never called.
        m_db.reset();
    }

    bool openDatabase(const QString& path, QString* errorOut)
    {
        m_db = std::make_unique<lps::CatalogDatabase>();
        if (!m_db->open(path)) {
            if (errorOut)
                *errorOut = m_db->lastError();
            m_db.reset();
            return false;
        }
        return true;
    }

    void closeDatabase()
    {
        if (m_db)
            m_db->close();
        m_db.reset();
    }

    bool isOpen() const { return m_db && m_db->isOpen(); }

    CatalogSnapshot snapshot(const lps::CatalogFilter& filter, int limit)
    {
        CatalogSnapshot out;
        if (!isOpen()) {
            out.error = QCoreApplication::translate(
                "LibraryView", "The catalog is not open.");
            return out;
        }

        out.ok         = true;
        out.folders    = m_db->folders(true);
        out.stats      = m_db->stats();
        out.matchCount = m_db->queryCount(filter);
        out.images     = m_db->query(filter, limit, 0);
        out.truncated  = (limit > 0 && out.matchCount > limit);
        if (out.matchCount < out.images.size())
            out.matchCount = static_cast<int>(out.images.size());
        return out;
    }

    bool setRating(qint64 imageId, int rating)
    {
        return isOpen() && m_db->setRating(imageId, rating);
    }

    bool setFlag(qint64 imageId, lps::ImageFlag flag)
    {
        return isOpen() && m_db->setFlag(imageId, flag);
    }

    lps::CatalogImage image(qint64 imageId)
    {
        return isOpen() ? m_db->image(imageId) : lps::CatalogImage();
    }

private:
    std::unique_ptr<lps::CatalogDatabase> m_db;
};

// ==============================================================================
// LibraryView::Impl
// ==============================================================================
struct LibraryView::Impl
{
    explicit Impl(LibraryView* owner) : q(owner) {}

    // ---- construction -------------------------------------------------------
    void buildUi();
    QFrame* buildFilterBar();
    QFrame* buildSidebar();
    QWidget* buildEmptyPage(const QString& title,
                            const QString& body,
                            const QString& actionText,
                            QLabel** bodyOut,
                            QPushButton** actionOut);
    void startWorkerThread();
    void stopWorkerThread();

    // ---- data flow ----------------------------------------------------------
    lps::CatalogFilter buildFilter() const;
    void reload();
    void applySnapshot(quint64 token, const CatalogSnapshot& snapshot);
    void rebuildFolderList(const QVector<lps::CatalogFolder>& folders,
                           int totalImages);
    void updateCentrePage();
    void updateStatus();
    void flushStatus();
    void announce(const QString& message);
    QString composeStatus() const;
    void showCatalogError(const QString& detail);
    void clearFilters();

    // ---- writes -------------------------------------------------------------
    void writeRating(qint64 imageId, int rating);
    void writeFlag(qint64 imageId, lps::ImageFlag flag);
    void patchLocalImage(const lps::CatalogImage& image);

    // ---- import -------------------------------------------------------------
    void beginImport(const QString& path);
    void setImportRunning(bool running);
    void reportImportProblem(const QString& message);

    enum Page : int { PageGrid = 0, PageNoPhotos, PageNoMatches, PageError };

    LibraryView* q = nullptr;

    // Worker thread
    QThread*       thread = nullptr;
    CatalogWorker* worker = nullptr;
    QString        catalogPath;
    bool           catalogOpen = false;

    // Widgets
    QLineEdit*      searchEdit      = nullptr;
    QComboBox*      ratingCombo     = nullptr;
    QToolButton*    pickedButton    = nullptr;
    QToolButton*    hideRejectedBtn = nullptr;
    QComboBox*      labelCombo      = nullptr;
    QComboBox*      sortCombo       = nullptr;
    QToolButton*    sortDirButton   = nullptr;
    QSlider*        sizeSlider      = nullptr;
    QListWidget*    folderList      = nullptr;
    QPushButton*    importButton    = nullptr;
    QFrame*         progressCard    = nullptr;
    QProgressBar*   progressBar     = nullptr;
    QLabel*         progressCaption = nullptr;
    QPushButton*    cancelButton    = nullptr;
    QStackedWidget* centre          = nullptr;
    CatalogGridView* grid           = nullptr;
    QLabel*         statusLabel     = nullptr;
    QTimer*         statusTimer     = nullptr;
    QLabel*         noPhotosBody    = nullptr;
    QLabel*         errorBody       = nullptr;
    QPushButton*    noPhotosAction  = nullptr;
    QPushButton*    noMatchAction   = nullptr;
    QPushButton*    errorAction     = nullptr;
    QTimer*         searchTimer     = nullptr;

    // Import
    lps::CatalogImporter* importer = nullptr;
    QElapsedTimer         progressClock;

    // Query state
    quint64                    requestToken = 0;
    qint64                     folderId     = -1;
    bool                       ascending    = false;
    QString                    pendingStatus;
    QString                    emittedStatus;
    QVector<lps::CatalogImage> images;
    lps::CatalogStats          stats;
    int                        matchCount   = 0;
    bool                       truncated    = false;
    int                        folderCount  = 0;
    int                        selectedCount = 0;
};

// ==============================================================================
// Worker thread lifecycle
// ==============================================================================
void LibraryView::Impl::startWorkerThread()
{
    thread = new QThread;
    thread->setObjectName(QStringLiteral("lps-catalog"));

    worker = new CatalogWorker;      // no parent: ownership is manual so the
    worker->moveToThread(thread);    // object can follow the thread
    thread->start();
}

void LibraryView::Impl::stopWorkerThread()
{
    if (!thread)
        return;

    if (worker) {
        CatalogWorker* w = worker;
        QMetaObject::invokeMethod(w, [w]() { w->closeDatabase(); },
                                  Qt::BlockingQueuedConnection);
    }

    thread->quit();
    thread->wait();

    delete worker;   // thread is stopped: deleting from here is safe
    worker = nullptr;
    delete thread;
    thread = nullptr;
}

// ==============================================================================
// UI construction
// ==============================================================================
QWidget* LibraryView::Impl::buildEmptyPage(const QString& title,
                                           const QString& body,
                                           const QString& actionText,
                                           QLabel** bodyOut,
                                           QPushButton** actionOut)
{
    auto* page = new QWidget(q);
    auto* outer = new QVBoxLayout(page);
    outer->setContentsMargins(24, 24, 24, 24);
    outer->addStretch(1);

    auto* titleLabel = new QLabel(title, page);
    titleLabel->setObjectName(QStringLiteral("emptyTitle"));
    titleLabel->setAlignment(Qt::AlignCenter);
    QFont titleFont = titleLabel->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() * 1.35);
    titleFont.setBold(true);
    titleLabel->setFont(titleFont);
    outer->addWidget(titleLabel);

    auto* bodyLabel = new QLabel(body, page);
    bodyLabel->setObjectName(QStringLiteral("emptyBody"));
    bodyLabel->setAlignment(Qt::AlignCenter);
    bodyLabel->setWordWrap(true);
    bodyLabel->setMaximumWidth(kEmptyTextWidth);   // keep the measure readable
    outer->addSpacing(6);

    // Stretch 10 against 1 on each side: the label takes as much width as it is
    // allowed (up to kEmptyTextWidth) and stays centred in whatever is left.
    auto* bodyRow = new QHBoxLayout;
    bodyRow->addStretch(1);
    bodyRow->addWidget(bodyLabel, 10);
    bodyRow->addStretch(1);
    outer->addLayout(bodyRow);
    if (bodyOut)
        *bodyOut = bodyLabel;

    if (!actionText.isEmpty()) {
        auto* row = new QHBoxLayout;
        row->addStretch(1);
        auto* button = new QPushButton(actionText, page);
        button->setCursor(Qt::PointingHandCursor);
        row->addWidget(button);
        row->addStretch(1);
        outer->addSpacing(16);
        outer->addLayout(row);
        if (actionOut)
            *actionOut = button;
    }

    outer->addStretch(1);
    return page;
}

QFrame* LibraryView::Impl::buildFilterBar()
{
    auto* bar = new QFrame(q);
    bar->setObjectName(QStringLiteral("libraryBar"));

    auto* row = new QHBoxLayout(bar);
    row->setContentsMargins(10, 8, 10, 8);
    row->setSpacing(8);

    searchEdit = new QLineEdit(bar);
    searchEdit->setPlaceholderText(
        LibraryView::tr("Search filename, camera, lens or keyword"));
    searchEdit->setClearButtonEnabled(true);
    searchEdit->setMinimumWidth(220);
    row->addWidget(searchEdit, 2);

    ratingCombo = new QComboBox(bar);
    ratingCombo->addItem(LibraryView::tr("Any rating"), 0);
    for (int stars = 1; stars <= 5; ++stars) {
        ratingCombo->addItem(
            LibraryView::tr("%n star(s) and up", nullptr, stars), stars);
    }
    ratingCombo->setToolTip(LibraryView::tr("Minimum rating"));
    row->addWidget(ratingCombo);

    labelCombo = new QComboBox(bar);
    labelCombo->addItem(LibraryView::tr("Any label"),
                        static_cast<int>(lps::ColorLabel::None));
    labelCombo->addItem(labelSwatch(QColor(0xE2, 0x60, 0x5F)), LibraryView::tr("Red"),
                        static_cast<int>(lps::ColorLabel::Red));
    labelCombo->addItem(labelSwatch(QColor(0xE6, 0xC5, 0x4A)), LibraryView::tr("Yellow"),
                        static_cast<int>(lps::ColorLabel::Yellow));
    labelCombo->addItem(labelSwatch(QColor(0x6B, 0xD1, 0x7A)), LibraryView::tr("Green"),
                        static_cast<int>(lps::ColorLabel::Green));
    labelCombo->addItem(labelSwatch(QColor(0x6C, 0x9C, 0xE8)), LibraryView::tr("Blue"),
                        static_cast<int>(lps::ColorLabel::Blue));
    labelCombo->addItem(labelSwatch(QColor(0xB0, 0x82, 0xE8)), LibraryView::tr("Purple"),
                        static_cast<int>(lps::ColorLabel::Purple));
    labelCombo->setToolTip(LibraryView::tr("Colour label"));
    row->addWidget(labelCombo);

    pickedButton = new QToolButton(bar);
    pickedButton->setText(LibraryView::tr("Picked"));
    pickedButton->setCheckable(true);
    pickedButton->setToolTip(LibraryView::tr("Show only picked photos"));
    row->addWidget(pickedButton);

    hideRejectedBtn = new QToolButton(bar);
    hideRejectedBtn->setText(LibraryView::tr("Hide rejected"));
    hideRejectedBtn->setCheckable(true);
    hideRejectedBtn->setChecked(true);
    hideRejectedBtn->setToolTip(LibraryView::tr("Hide photos flagged as rejected"));
    row->addWidget(hideRejectedBtn);

    row->addStretch(1);

    row->addWidget(new QLabel(LibraryView::tr("Sort"), bar));
    sortCombo = new QComboBox(bar);
    sortCombo->addItem(LibraryView::tr("Capture time"),
                       static_cast<int>(lps::SortKey::CaptureTime));
    sortCombo->addItem(LibraryView::tr("File name"),
                       static_cast<int>(lps::SortKey::FileName));
    sortCombo->addItem(LibraryView::tr("Rating"),
                       static_cast<int>(lps::SortKey::Rating));
    sortCombo->addItem(LibraryView::tr("Import date"),
                       static_cast<int>(lps::SortKey::ImportedAt));
    sortCombo->addItem(LibraryView::tr("File size"),
                       static_cast<int>(lps::SortKey::FileSize));
    row->addWidget(sortCombo);

    sortDirButton = new QToolButton(bar);
    sortDirButton->setCheckable(true);
    sortDirButton->setText(LibraryView::tr("Newest first"));
    sortDirButton->setToolTip(LibraryView::tr("Toggle sort direction"));
    row->addWidget(sortDirButton);

    auto* sizeLabel = new QLabel(LibraryView::tr("Size"), bar);
    row->addWidget(sizeLabel);
    sizeSlider = new QSlider(Qt::Horizontal, bar);
    sizeSlider->setRange(kMinThumbPx, kMaxThumbPx);
    sizeSlider->setSingleStep(8);
    sizeSlider->setPageStep(32);
    sizeSlider->setFixedWidth(110);
    sizeSlider->setToolTip(LibraryView::tr("Thumbnail size"));
    row->addWidget(sizeSlider);

    return bar;
}

QFrame* LibraryView::Impl::buildSidebar()
{
    auto* card = new QFrame(q);
    card->setObjectName(QStringLiteral("libraryCard"));
    card->setMinimumWidth(200);

    auto* column = new QVBoxLayout(card);
    column->setContentsMargins(12, 12, 12, 12);
    column->setSpacing(8);

    column->addWidget(makeSectionTitle(LibraryView::tr("Folders"), card));

    folderList = new QListWidget(card);
    folderList->setUniformItemSizes(true);
    folderList->setAlternatingRowColors(false);
    folderList->setSelectionMode(QAbstractItemView::SingleSelection);
    folderList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    column->addWidget(folderList, 1);

    importButton = new QPushButton(LibraryView::tr("Import Folder…"), card);
    importButton->setCursor(Qt::PointingHandCursor);
    column->addWidget(importButton);

    // ---- import progress (hidden until an import is running) ---------------
    progressCard = new QFrame(card);
    progressCard->setObjectName(QStringLiteral("libraryBar"));
    auto* progressColumn = new QVBoxLayout(progressCard);
    progressColumn->setContentsMargins(10, 8, 10, 8);
    progressColumn->setSpacing(6);

    progressCaption = new QLabel(LibraryView::tr("Scanning…"), progressCard);
    progressCaption->setObjectName(QStringLiteral("progressCaption"));
    progressCaption->setWordWrap(false);
    progressColumn->addWidget(progressCaption);

    progressBar = new QProgressBar(progressCard);
    progressBar->setRange(0, 0);
    progressBar->setTextVisible(false);
    progressColumn->addWidget(progressBar);

    cancelButton = new QPushButton(LibraryView::tr("Cancel"), progressCard);
    progressColumn->addWidget(cancelButton, 0, Qt::AlignRight);

    progressCard->hide();
    column->addWidget(progressCard);

    return card;
}

void LibraryView::Impl::buildUi()
{
    q->setObjectName(QStringLiteral("libraryRoot"));
    q->setStyleSheet(styleSheetForLibrary());

    auto* root = new QVBoxLayout(q);
    root->setContentsMargins(10, 10, 10, 8);
    root->setSpacing(8);

    root->addWidget(buildFilterBar());

    auto* splitter = new QSplitter(Qt::Horizontal, q);
    splitter->setChildrenCollapsible(false);
    splitter->setHandleWidth(8);
    splitter->addWidget(buildSidebar());

    centre = new QStackedWidget(q);
    grid = new CatalogGridView(centre);
    centre->insertWidget(PageGrid, grid);
    centre->insertWidget(
        PageNoPhotos,
        buildEmptyPage(LibraryView::tr("Your library is empty"),
                       LibraryView::tr("Import a folder of photos and Lumen will "
                                       "catalog them here — ratings, flags and "
                                       "edits stay with the catalog, never with "
                                       "your originals."),
                       LibraryView::tr("Import Folder…"),
                       &noPhotosBody,
                       &noPhotosAction));
    centre->insertWidget(
        PageNoMatches,
        buildEmptyPage(LibraryView::tr("No photos match this filter."),
                       LibraryView::tr("Try a different search, a lower minimum "
                                       "rating, or clear the filters."),
                       LibraryView::tr("Clear Filters"),
                       nullptr,
                       &noMatchAction));
    centre->insertWidget(
        PageError,
        buildEmptyPage(LibraryView::tr("Catalog unavailable"),
                       LibraryView::tr("The photo catalog has not been opened yet."),
                       LibraryView::tr("Try Again"),
                       &errorBody,
                       &errorAction));
    centre->setCurrentIndex(PageError);

    splitter->addWidget(centre);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({ 220, 900 });
    root->addWidget(splitter, 1);

    statusLabel = new QLabel(LibraryView::tr("Catalog not opened."), q);
    statusLabel->setObjectName(QStringLiteral("libraryStatus"));
    root->addWidget(statusLabel);

    sizeSlider->setValue(grid->thumbnailSize());

    statusTimer = new QTimer(q);
    statusTimer->setSingleShot(true);
    statusTimer->setInterval(kStatusEmitMs);
    QObject::connect(statusTimer, &QTimer::timeout, q, [this]() { flushStatus(); });

    // ---- filter wiring ------------------------------------------------------
    searchTimer = new QTimer(q);
    searchTimer->setSingleShot(true);
    searchTimer->setInterval(kSearchDebounceMs);
    QObject::connect(searchTimer, &QTimer::timeout, q, [this]() { reload(); });

    QObject::connect(searchEdit, &QLineEdit::textChanged, q,
                     [this](const QString&) { searchTimer->start(); });
    QObject::connect(searchEdit, &QLineEdit::returnPressed, q, [this]() {
        searchTimer->stop();
        reload();
    });

    QObject::connect(ratingCombo, &QComboBox::currentIndexChanged, q,
                     [this](int) { reload(); });
    QObject::connect(labelCombo, &QComboBox::currentIndexChanged, q,
                     [this](int) { reload(); });
    QObject::connect(sortCombo, &QComboBox::currentIndexChanged, q,
                     [this](int) { reload(); });
    QObject::connect(pickedButton, &QToolButton::toggled, q,
                     [this](bool) { reload(); });
    QObject::connect(hideRejectedBtn, &QToolButton::toggled, q,
                     [this](bool) { reload(); });
    QObject::connect(sortDirButton, &QToolButton::toggled, q, [this](bool checked) {
        ascending = checked;
        sortDirButton->setText(checked ? LibraryView::tr("Oldest first")
                                       : LibraryView::tr("Newest first"));
        reload();
    });

    QObject::connect(sizeSlider, &QSlider::valueChanged, q,
                     [this](int value) { grid->setThumbnailSize(value); });

    QObject::connect(folderList, &QListWidget::currentItemChanged, q,
                     [this](QListWidgetItem* current, QListWidgetItem*) {
                         if (!current)
                             return;
                         const qint64 id = current->data(Qt::UserRole).toLongLong();
                         if (id == folderId)
                             return;
                         folderId = id;
                         reload();
                     });

    // ---- grid wiring --------------------------------------------------------
    QObject::connect(grid, &CatalogGridView::imageActivated, q,
                     &LibraryView::imageActivated);
    QObject::connect(grid, &CatalogGridView::ratingRequested, q,
                     [this](qint64 id, int rating) { writeRating(id, rating); });
    QObject::connect(grid, &CatalogGridView::flagRequested, q,
                     [this](qint64 id, lps::ImageFlag flag) { writeFlag(id, flag); });
    QObject::connect(grid, &CatalogGridView::selectionChanged, q,
                     [this](const QVector<qint64>& ids) {
                         selectedCount = static_cast<int>(ids.size());
                         updateStatus();
                     });

    // ---- import wiring ------------------------------------------------------
    QObject::connect(importButton, &QPushButton::clicked, q,
                     &LibraryView::importFolder);
    QObject::connect(noPhotosAction, &QPushButton::clicked, q,
                     &LibraryView::importFolder);
    QObject::connect(noMatchAction, &QPushButton::clicked, q,
                     [this]() { clearFilters(); });
    QObject::connect(errorAction, &QPushButton::clicked, q,
                     [this]() { q->openCatalog(catalogPath); });
    QObject::connect(cancelButton, &QPushButton::clicked, q, [this]() {
        if (importer) {
            cancelButton->setEnabled(false);
            progressCaption->setText(LibraryView::tr("Cancelling…"));
            importer->cancel();
        }
    });
}

// ==============================================================================
// Filter and query
// ==============================================================================
lps::CatalogFilter LibraryView::Impl::buildFilter() const
{
    lps::CatalogFilter filter;
    filter.folderId     = folderId;
    filter.minRating    = ratingCombo->currentData().toInt();
    filter.pickedOnly   = pickedButton->isChecked();
    filter.hideRejected = hideRejectedBtn->isChecked();
    filter.colorLabel   =
        static_cast<lps::ColorLabel>(labelCombo->currentData().toInt());
    filter.searchText   = searchEdit->text().trimmed();
    filter.sortKey      = static_cast<lps::SortKey>(sortCombo->currentData().toInt());
    filter.ascending    = ascending;
    return filter;
}

// Posts one query round trip to the worker thread. The token makes stale
// answers harmless: if the user keeps typing, only the newest reply is applied.
void LibraryView::Impl::reload()
{
    if (!catalogOpen || !worker)
        return;

    const quint64 token = ++requestToken;
    const lps::CatalogFilter filter = buildFilter();

    CatalogWorker* w = worker;
    LibraryView* self = q;

    QMetaObject::invokeMethod(w, [w, self, filter, token]() {
        const CatalogSnapshot snapshot = w->snapshot(filter, kMaxGridImages);
        QMetaObject::invokeMethod(
            self,
            [self, token, snapshot]() { self->d->applySnapshot(token, snapshot); },
            Qt::QueuedConnection);
    });
}

void LibraryView::Impl::applySnapshot(quint64 token, const CatalogSnapshot& snapshot)
{
    if (token != requestToken)
        return;   // superseded by a newer request

    if (!snapshot.ok) {
        showCatalogError(snapshot.error);
        return;
    }

    images      = snapshot.images;
    stats       = snapshot.stats;
    matchCount  = snapshot.matchCount;
    truncated   = snapshot.truncated;
    folderCount = static_cast<int>(snapshot.folders.size());

    rebuildFolderList(snapshot.folders, snapshot.stats.imageCount);
    grid->setImages(images);
    updateCentrePage();
    updateStatus();
}

void LibraryView::Impl::rebuildFolderList(const QVector<lps::CatalogFolder>& folders,
                                          int totalImages)
{
    const QSignalBlocker blocker(folderList);
    folderList->clear();

    auto* all = new QListWidgetItem(
        LibraryView::tr("All Photos  (%1)").arg(formatCount(totalImages)),
        folderList);
    all->setData(Qt::UserRole, QVariant::fromValue<qint64>(-1));
    all->setToolTip(LibraryView::tr("Every folder in the catalog"));

    for (const lps::CatalogFolder& folder : folders) {
        const QString name = QDir(folder.path).dirName().isEmpty()
            ? QDir::toNativeSeparators(folder.path)
            : QDir(folder.path).dirName();
        auto* item = new QListWidgetItem(
            LibraryView::tr("%1  (%2)").arg(name, formatCount(folder.imageCount)),
            folderList);
        item->setData(Qt::UserRole, QVariant::fromValue<qint64>(folder.id));
        item->setToolTip(QDir::toNativeSeparators(folder.path));
    }

    // Reselect whatever was selected before the rebuild.
    for (int row = 0; row < folderList->count(); ++row) {
        if (folderList->item(row)->data(Qt::UserRole).toLongLong() == folderId) {
            folderList->setCurrentRow(row);
            return;
        }
    }
    folderId = -1;
    folderList->setCurrentRow(0);
}

void LibraryView::Impl::updateCentrePage()
{
    if (!catalogOpen) {
        centre->setCurrentIndex(PageError);
        return;
    }
    if (folderCount == 0 || stats.imageCount == 0) {
        noPhotosBody->setText(
            folderCount == 0
                ? LibraryView::tr("Import a folder of photos and Lumen will catalog "
                                  "them here — ratings, flags and edits stay with "
                                  "the catalog, never with your originals.")
                : LibraryView::tr("The folders you added do not contain any photos "
                                  "Lumen can read yet. Import another folder to get "
                                  "started."));
        centre->setCurrentIndex(PageNoPhotos);
        return;
    }
    centre->setCurrentIndex(images.isEmpty() ? PageNoMatches : PageGrid);
}

// The label is always current; the signal is rate limited. Confirming a rating
// on a hundred selected photos updates the count a hundred times, and MainWindow
// does not need a hundred status-bar messages to show the last one.
void LibraryView::Impl::updateStatus()
{
    const QString message = composeStatus();
    statusLabel->setText(message);
    pendingStatus = message;

    if (!statusTimer->isActive()) {
        flushStatus();
        statusTimer->start();
    }
}

void LibraryView::Impl::flushStatus()
{
    if (pendingStatus == emittedStatus)
        return;
    emittedStatus = pendingStatus;
    emit q->statusMessage(emittedStatus);
}

QString LibraryView::Impl::composeStatus() const
{
    QString message;

    if (!catalogOpen) {
        message = LibraryView::tr("Catalog unavailable.");
    } else if (stats.imageCount == 0) {
        message = LibraryView::tr("No photos in the catalog yet.");
    } else {
        int picked = 0;
        for (const lps::CatalogImage& image : images) {
            if (image.flag == lps::ImageFlag::Picked)
                ++picked;
        }

        if (matchCount == stats.imageCount) {
            message = LibraryView::tr("%1 photos · %2 picked")
                          .arg(formatCount(matchCount), formatCount(picked));
        } else {
            message = LibraryView::tr("%1 of %2 photos · %3 picked")
                          .arg(formatCount(matchCount),
                               formatCount(stats.imageCount),
                               formatCount(picked));
        }

        if (truncated) {
            message += LibraryView::tr(" · showing the first %1")
                           .arg(formatCount(kMaxGridImages));
        }
        if (selectedCount > 1) {
            message += LibraryView::tr(" · %1 selected").arg(formatCount(selectedCount));
        }
    }

    return message;
}

void LibraryView::Impl::showCatalogError(const QString& detail)
{
    catalogOpen = false;
    images.clear();
    stats = lps::CatalogStats();
    matchCount = 0;
    truncated = false;
    grid->clear();

    QString body = LibraryView::tr(
        "Lumen could not open the photo catalog, so the library is empty. Your "
        "photos are untouched — only the catalog index is affected.");
    if (!detail.trimmed().isEmpty())
        body += QStringLiteral("\n\n") + detail.trimmed();
    if (!catalogPath.isEmpty())
        body += QStringLiteral("\n") + QDir::toNativeSeparators(catalogPath);

    errorBody->setText(body);
    centre->setCurrentIndex(PageError);
    importButton->setEnabled(false);
    updateStatus();
}

void LibraryView::Impl::clearFilters()
{
    {
        const QSignalBlocker b1(searchEdit);
        const QSignalBlocker b2(ratingCombo);
        const QSignalBlocker b3(labelCombo);
        const QSignalBlocker b4(pickedButton);
        const QSignalBlocker b5(hideRejectedBtn);

        searchEdit->clear();
        ratingCombo->setCurrentIndex(0);
        labelCombo->setCurrentIndex(0);
        pickedButton->setChecked(false);
        hideRejectedBtn->setChecked(true);
    }
    searchTimer->stop();
    reload();
}

// ==============================================================================
// Rating / flag writes
//
// The grid has already applied the change optimistically, so the round trip
// only has to confirm it. On success we push the authoritative row back into
// the grid; on failure we say so and leave the optimistic value in place until
// the next refresh corrects it, rather than yanking the tile out from under
// the user mid-cull.
// ==============================================================================
void LibraryView::Impl::writeRating(qint64 imageId, int rating)
{
    if (!catalogOpen || !worker)
        return;

    CatalogWorker* w = worker;
    LibraryView* self = q;

    QMetaObject::invokeMethod(w, [w, self, imageId, rating]() {
        const bool ok = w->setRating(imageId, rating);
        const lps::CatalogImage fresh = ok ? w->image(imageId) : lps::CatalogImage();
        QMetaObject::invokeMethod(
            self,
            [self, ok, fresh]() {
                if (ok && fresh.isValid()) {
                    self->d->grid->updateImage(fresh);
                    self->d->patchLocalImage(fresh);
                    self->d->updateStatus();
                } else if (!ok) {
                    self->d->announce(
                        LibraryView::tr("Could not save the rating to the catalog."));
                }
            },
            Qt::QueuedConnection);
    });
}

void LibraryView::Impl::writeFlag(qint64 imageId, lps::ImageFlag flag)
{
    if (!catalogOpen || !worker)
        return;

    CatalogWorker* w = worker;
    LibraryView* self = q;

    QMetaObject::invokeMethod(w, [w, self, imageId, flag]() {
        const bool ok = w->setFlag(imageId, flag);
        const lps::CatalogImage fresh = ok ? w->image(imageId) : lps::CatalogImage();
        QMetaObject::invokeMethod(
            self,
            [self, ok, fresh]() {
                if (ok && fresh.isValid()) {
                    self->d->grid->updateImage(fresh);
                    self->d->patchLocalImage(fresh);
                    self->d->updateStatus();
                } else if (!ok) {
                    self->d->announce(
                        LibraryView::tr("Could not save the flag to the catalog."));
                }
            },
            Qt::QueuedConnection);
    });
}

void LibraryView::Impl::patchLocalImage(const lps::CatalogImage& image)
{
    for (lps::CatalogImage& row : images) {
        if (row.id == image.id) {
            row = image;
            return;
        }
    }
}

// ==============================================================================
// Import
// ==============================================================================
void LibraryView::Impl::setImportRunning(bool running)
{
    importButton->setEnabled(!running && catalogOpen);
    if (noPhotosAction)
        noPhotosAction->setEnabled(!running && catalogOpen);
    cancelButton->setEnabled(running);
    progressCard->setVisible(running);
    if (running) {
        progressBar->setRange(0, 0);
        progressBar->setValue(0);
        progressCaption->setText(LibraryView::tr("Scanning…"));
        progressClock.start();
    }
}

// Import problems are surfaced in the status line, never in a modal box: a
// background import can fail long after the click that started it, and a
// dialog that steals focus at that moment is worse than the failure.
void LibraryView::Impl::reportImportProblem(const QString& message)
{
    announce(message);
}

// One-off messages jump the rate limiter: they describe an event, not a count.
void LibraryView::Impl::announce(const QString& message)
{
    statusLabel->setText(message);
    pendingStatus = message;
    flushStatus();
}

void LibraryView::Impl::beginImport(const QString& path)
{
    if (path.isEmpty())
        return;

    // Reported in the status line rather than a modal box: import problems are
    // never worth blocking the whole window for, and a background failure must
    // not be able to steal focus from whatever the user is doing.
    if (!QFileInfo(path).isDir()) {
        reportImportProblem(LibraryView::tr("%1 is not a folder.")
                                .arg(QDir::toNativeSeparators(path)));
        return;
    }
    if (importer && importer->isRunning()) {
        announce(LibraryView::tr("An import is already running."));
        return;
    }
    if (!catalogOpen) {
        announce(
            LibraryView::tr("The catalog is not open, so nothing can be imported."));
        return;
    }

    if (!importer) {
        importer = new lps::CatalogImporter(q);

        QObject::connect(importer, &lps::CatalogImporter::started, q,
                         [this](const QString& folder) {
                             announce(LibraryView::tr("Importing %1…")
                                          .arg(QDir::toNativeSeparators(folder)));
                         });

        QObject::connect(importer, &lps::CatalogImporter::progress, q,
                         [this](int current, int total, const QString& file) {
                             // Throttled: an importer can emit this once per file.
                             if (progressClock.isValid()
                                 && progressClock.elapsed() < kProgressPaintMs
                                 && (total <= 0 || current < total)) {
                                 return;
                             }
                             progressClock.restart();

                             if (total > 0) {
                                 progressBar->setRange(0, total);
                                 progressBar->setValue(current);
                                 progressCaption->setText(
                                     LibraryView::tr("%1 of %2 · %3")
                                         .arg(formatCount(current),
                                              formatCount(total),
                                              QFileInfo(file).fileName()));
                             } else {
                                 progressBar->setRange(0, 0);
                                 progressCaption->setText(
                                     LibraryView::tr("Scanning… %1 files")
                                         .arg(formatCount(current)));
                             }
                         });

        QObject::connect(importer, &lps::CatalogImporter::failed, q,
                         [this](const QString& message) {
                             setImportRunning(false);
                             reportImportProblem(
                                 LibraryView::tr("Import failed: %1").arg(message));
                         });

        QObject::connect(importer, &lps::CatalogImporter::finished, q,
                         [this](int imported, int skipped, int failed, bool cancelled) {
                             setImportRunning(false);
                             const QString summary =
                                 cancelled
                                     ? LibraryView::tr("Import cancelled — %1 added, "
                                                       "%2 skipped, %3 failed")
                                           .arg(formatCount(imported),
                                                formatCount(skipped),
                                                formatCount(failed))
                                     : LibraryView::tr("Imported %1 photos — %2 "
                                                       "skipped, %3 failed")
                                           .arg(formatCount(imported),
                                                formatCount(skipped),
                                                formatCount(failed));
                             announce(summary);
                             reload();
                         });
    }

    importer->setCatalogPath(catalogPath.isEmpty()
                                 ? lps::CatalogDatabase::defaultCatalogPath()
                                 : catalogPath);
    setImportRunning(true);
    importer->startImport(path, true);
}

// ==============================================================================
// LibraryView
// ==============================================================================
LibraryView::LibraryView(QWidget* parent)
    : QWidget(parent)
    , d(new Impl(this))
{
    d->startWorkerThread();
    d->buildUi();
    d->importButton->setEnabled(false);   // until a catalog is open
    if (d->noPhotosAction)
        d->noPhotosAction->setEnabled(false);
}

LibraryView::~LibraryView()
{
    // The importer's queued handlers capture the pimpl, so it has to go first.
    // Its own destructor asks the worker to stop and joins it.
    delete d->importer;
    d->importer = nullptr;

    d->stopWorkerThread();
    delete d;
}

// Blocking by design: the contract returns a bool. It is one round trip to a
// thread that is otherwise idle at this point, and it happens once at startup.
bool LibraryView::openCatalog(const QString& catalogPath)
{
    d->catalogPath = catalogPath.isEmpty()
                         ? lps::CatalogDatabase::defaultCatalogPath()
                         : catalogPath;

    if (!d->worker) {
        d->showCatalogError(tr("The catalog worker thread is not running."));
        return false;
    }

    bool ok = false;
    QString error;
    CatalogWorker* w = d->worker;
    const QString path = d->catalogPath;

    QMetaObject::invokeMethod(
        w,
        [w, path, &ok, &error]() {
            w->closeDatabase();
            ok = w->openDatabase(path, &error);
        },
        Qt::BlockingQueuedConnection);

    d->catalogOpen = ok;
    if (!ok) {
        d->showCatalogError(error);
        return false;
    }

    d->importButton->setEnabled(true);
    if (d->noPhotosAction)
        d->noPhotosAction->setEnabled(true);
    refresh();
    return true;
}

void LibraryView::refresh()
{
    if (!d->catalogOpen) {
        d->updateCentrePage();
        d->updateStatus();
        return;
    }
    d->reload();
}

void LibraryView::importFolder()
{
    const QString start = d->catalogPath.isEmpty()
                              ? QDir::homePath()
                              : QFileInfo(d->catalogPath).absolutePath();

    const QString folder = QFileDialog::getExistingDirectory(
        this,
        tr("Import Folder"),
        start,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);

    if (folder.isEmpty())
        return;   // user cancelled

    d->beginImport(folder);
}

void LibraryView::importFolderPath(const QString& path)
{
    d->beginImport(path);
}

#include "LibraryView.moc"
