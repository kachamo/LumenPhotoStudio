// ==============================================================================
// ui/MainWindow.cpp
// ==============================================================================
#include "MainWindow.h"

#include "ColorWheelWidget.h"
#include "CurveEditorWidget.h"
#include "EmptyStateOverlay.h"
#include "ExportDialog.h"
#include "HistogramWidget.h"
#include "NodeGraphWidget.h"
#include "PreviewWidget.h"
#include "SecondaryViewerWindow.h"
#include "WelcomeScreenWidget.h"

#include "core/ImagePipeline.h"
#include "io/ImageMetadataReader.h"
#include "io/RawImageLoader.h"
#include "plugins/PluginManager.h"
#include "preset/LookSerializer.h"
#include "project/AutosaveManager.h"
#include "project/ProjectSerializer.h"
#include "settings/SettingsManager.h"

#include <QAction>
#include <QAbstractItemView>
#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QCloseEvent>
#include <QColor>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFuture>
#include <QFutureWatcher>
#include <QFrame>
#include <QGridLayout>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QImageWriter>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QObject>
#include <QPixmap>
#include <QPushButton>
#include <QRectF>
#include <QScrollArea>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSize>
#include <QSlider>
#include <QStackedLayout>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStringList>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtConcurrent>

#include <algorithm>
#include <cmath>
#include <array>
#include <memory>
#include <utility>

// ==============================================================================
// Tunables
// ==============================================================================
namespace {

// Debounce interval — coalesces rapid slider signals.
constexpr int kDebounceMs = 30;

// Max edge length for the preview-sized copy used during interactive edits.
// 1800px fills a QHD viewport at 1:1 on most screens while keeping pixel
// counts ~10-30x lower than a modern full-res source.
constexpr int kPreviewMaxEdge = 1800;

// Slider ranges. We store floats in lps::Look, so int slider values are
// scaled by 100 for the "unit" sliders and directly for "-100..+100" sliders.
constexpr int kExposureScale = 100;   // slider int / 100 → stops (e.g. 150 → 1.5 stops)

class NoWheelSlider final : public QSlider
{
public:
    explicit NoWheelSlider(Qt::Orientation orientation, QWidget* parent = nullptr)
        : QSlider(orientation, parent)
    {}

protected:
    void wheelEvent(QWheelEvent* event) override
    {
        event->ignore();
    }
};

QString lumenDarkTheme()
{
    return QStringLiteral(R"(
        QMainWindow, QWidget {
            background-color: #0E0F12;
            color: #E7E9EE;
            selection-background-color: #CCFF00;
            selection-color: #101114;
        }
        QMenuBar {
            background: #0E0F12;
            color: #D7DAE0;
            border-bottom: 1px solid #2A2D35;
            padding: 3px 8px;
        }
        QMenuBar::item {
            background: transparent;
            padding: 5px 10px;
            border-radius: 5px;
        }
        QMenuBar::item:selected {
            background: #1E2026;
            color: #CCFF00;
        }
        QMenu {
            background: #16181D;
            color: #E7E9EE;
            border: 1px solid #2A2D35;
            border-radius: 8px;
            padding: 6px;
        }
        QMenu::item {
            padding: 6px 24px 6px 10px;
            border-radius: 5px;
        }
        QMenu::item:selected {
            background: #22262C;
            color: #CCFF00;
        }
        QWidget#centralWorkspace {
            background: #0E0F12;
        }
        QWidget#topWorkspaceBar {
            background: #111318;
            border: 1px solid #2A2D35;
            border-radius: 10px;
        }
        QWidget#workspaceBody {
            background: transparent;
        }
        QWidget#canvasHost {
            background: transparent;
        }
        QTabBar#documentTabs {
            background: transparent;
            border: 0;
        }
        QTabBar#documentTabs::tab {
            background: #16181D;
            color: #9EA4AE;
            border: 1px solid #2A2D35;
            border-bottom-color: #1E2026;
            border-top-left-radius: 8px;
            border-top-right-radius: 8px;
            padding: 7px 28px 7px 14px;
            margin-right: 3px;
            min-width: 110px;
        }
        QTabBar#documentTabs::tab:hover {
            background: #1E2026;
            color: #E7E9EE;
        }
        QTabBar#documentTabs::tab:selected {
            background: #1E2026;
            color: #CCFF00;
            border-color: #CCFF00;
        }
        QWidget#leftToolRail {
            background: #111318;
            border: 1px solid #2A2D35;
            border-radius: 10px;
        }
        QWidget#analysisPanel {
            background: #111318;
            border: 1px solid #2A2D35;
            border-radius: 10px;
        }
        QWidget#analysisHeader {
            background: #1E2026;
            border-bottom: 1px solid #2A2D35;
            border-top-left-radius: 10px;
            border-top-right-radius: 10px;
        }
        QScrollArea#analysisScroll {
            background: transparent;
            border: 0;
        }
        QFrame#analysisCard {
            background: #16181D;
            border: 1px solid #2A2D35;
            border-radius: 8px;
        }
        QLabel#navigatorPreview {
            background: #0E0F12;
            border: 1px solid #2A2D35;
            border-radius: 6px;
        }
        QLabel#metadataName {
            color: #9EA4AE;
            font-size: 11px;
        }
        QLabel#metadataValue {
            color: #DDE0E7;
            font-size: 11px;
        }
        QWidget#sidebarHost {
            background: transparent;
        }
        QWidget#sidebarFull, QWidget#sidebarMini, QWidget#controlPanelCard {
            background: #16181D;
            border: 1px solid #2A2D35;
            border-radius: 10px;
        }
        QWidget#controlHeader {
            background: #1E2026;
            border-bottom: 1px solid #2A2D35;
            border-top-left-radius: 10px;
            border-top-right-radius: 10px;
        }
        QScrollArea#controlScroll {
            background: transparent;
            border: 0;
        }
        QScrollArea#controlScroll > QWidget > QWidget {
            background: transparent;
        }
        QLabel {
            color: #DDE0E7;
            background: transparent;
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
        QPushButton:pressed, QToolButton:pressed {
            background: #CCFF00;
            border-color: #CCFF00;
            color: #101114;
        }
        QPushButton:disabled, QToolButton:disabled {
            background: #15171B;
            color: #666A72;
            border-color: #24272B;
        }
        QToolButton[navButton="true"] {
            background: transparent;
            color: #7A7A7A;
            border: 1px solid transparent;
            border-radius: 8px;
            padding: 4px;
        }
        QToolButton[navButton="true"]:hover {
            background: #1E2026;
            color: #B0B0B0;
            border-color: #2A2D35;
        }
        QToolButton[navButton="true"]:checked {
            background: rgba(204, 255, 0, 31);
            color: #CCFF00;
            border-color: #CCFF00;
        }
        QCheckBox {
            spacing: 8px;
            color: #DDE0E7;
            background: transparent;
        }
        QCheckBox::indicator {
            width: 14px;
            height: 14px;
            border: 1px solid #3D424E;
            border-radius: 4px;
            background: #111318;
        }
        QCheckBox::indicator:hover {
            border-color: #B0B0B0;
        }
        QCheckBox::indicator:checked {
            background: #CCFF00;
            border-color: #CCFF00;
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
        QSlider::add-page:horizontal {
            background: #2A2D35;
            border-radius: 2px;
        }
        QSlider::handle:horizontal {
            width: 14px;
            height: 14px;
            margin: -6px 0;
            border-radius: 7px;
            background: #8B929D;
            border: 1px solid #B0B7C2;
        }
        QSlider::handle:horizontal:hover {
            background: #CCFF00;
            border-color: #CCFF00;
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
        QScrollBar::add-line:vertical,
        QScrollBar::sub-line:vertical {
            height: 0;
        }
        QDockWidget {
            background: #16181D;
            color: #E7E9EE;
            border: 1px solid #2A2D35;
        }
        QDockWidget::title {
            background: #1E2026;
            border-bottom: 1px solid #2A2D35;
            padding: 6px 10px;
            text-align: left;
        }
        QWidget#bottomWorkspaceContainer {
            background: #111318;
        }
        QFrame#bottomWorkspaceHeader {
            background: #16181D;
            border-top: 1px solid #2A2D35;
            border-bottom: 1px solid #2A2D35;
        }
        QTabWidget#bottomPanelTabs::pane {
            background: #111318;
            border: 1px solid #2A2D35;
            border-radius: 10px;
            top: -1px;
        }
        QTabWidget#bottomPanelTabs QTabBar::tab {
            background: #16181D;
            color: #9EA4AE;
            border: 1px solid #2A2D35;
            border-bottom: 0;
            border-top-left-radius: 7px;
            border-top-right-radius: 7px;
            padding: 6px 16px;
            margin-right: 4px;
        }
        QTabWidget#bottomPanelTabs QTabBar::tab:selected {
            background: #1E2026;
            color: #CCFF00;
            border-color: #CCFF00;
        }
    )");
}

// ---- HSL channel table ------------------------------------------------------
// One entry per canonical photo hue. Drives:
//   - the channel-selector button grid (displayName + tintHex for styling)
//   - pointer-to-member dispatch when sliders are moved (field)
// Order matters: m_selectedHslChannel and m_hslChannelButtons index into
// this table by position. First entry = default selected channel.
struct HslChannelSpec {
    const char*                        displayName;
    lps::HSLChannel lps::HSLParams::*  field;
    const char*                        tintHex;   // subtle fill color for the button
};

constexpr std::array<HslChannelSpec, 8> kHslChannels = {{
    // tintHex values are low-saturation versions of each hue — dim enough to
    // not fight the dark UI chrome, bright enough that the 8 buttons are
    // instantly distinguishable at a glance.
    { "Red",     &lps::HSLParams::red,     "#3a2020" },
    { "Orange",  &lps::HSLParams::orange,  "#3a2d1b" },
    { "Yellow",  &lps::HSLParams::yellow,  "#3a351b" },
    { "Green",   &lps::HSLParams::green,   "#1f3520" },
    { "Aqua",    &lps::HSLParams::aqua,    "#1f3537" },
    { "Blue",    &lps::HSLParams::blue,    "#1f2a3a" },
    { "Purple",  &lps::HSLParams::purple,  "#2c1f3a" },
    { "Magenta", &lps::HSLParams::magenta, "#3a1f33" },
}};

// Pointer-to-member table for the three HSL controls. Paired index-for-index
// with m_hslHueSlider / m_hslSaturationSlider / m_hslLuminanceSlider for
// dispatch from the selected channel to the right float field.
constexpr std::array<float lps::HSLChannel::*, 3> kHslControls = {
    &lps::HSLChannel::hue,
    &lps::HSLChannel::saturation,
    &lps::HSLChannel::luminance,
};

// ---- Curve channel table ---------------------------------------------------
// One entry per tone curve (master + per-channel RGB). Drives both the
// channel-tab buttons and the pointer-to-member dispatch used when switching
// which CurvePoints the editor is editing.
struct CurveChannelSpec {
    const char*                      displayName;
    lps::CurvePoints lps::CurveParams::* field;
    const char*                      tintHex;   // button background accent
    QColor                           lineColor; // curve-line accent in the editor
};

// QColor can't be used in a constexpr global on all Qt versions, so this is
// const (not constexpr) — fine, it's read-only and lives for the program
// lifetime.
static const std::array<CurveChannelSpec, 4> kCurveChannels = {{
    { "Master", &lps::CurveParams::master, "#303034", QColor(235, 235, 240) },
    { "Red",    &lps::CurveParams::red,    "#3a2020", QColor(232,  96,  96) },
    { "Green",  &lps::CurveParams::green,  "#1f3520", QColor( 96, 210, 120) },
    { "Blue",   &lps::CurveParams::blue,   "#1f2a3a", QColor(112, 156, 232) },
}};

} // namespace

// ==============================================================================
// Construction
// ==============================================================================
MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle(tr("Lumen Photo Studio — Engine Test UI"));
    resize(1280, 800);
    setAcceptDrops(true);

    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(kDebounceMs);
    connect(m_debounce, &QTimer::timeout, this, &MainWindow::onDebounceFired);

    m_pluginManager = std::make_unique<lps::PluginManager>();
    m_settings = std::make_unique<lps::SettingsManager>();
    m_autosaveManager = std::make_unique<lps::AutosaveManager>();
    m_bottomWorkspaceEnabled = m_settings->bottomWorkspaceVisible();
    m_bottomWorkspaceCollapsed = m_settings->bottomWorkspaceCollapsed();
    m_analysisPanelCollapsed = m_settings->analysisPanelCollapsed();

    buildUi();

    // Catch Space globally — sliders have keyboard focus during drags, and
    // would otherwise eat the event before MainWindow sees it. eventFilter()
    // forwards Space to our keyPressEvent/keyReleaseEvent handlers.
    if (auto* app = QCoreApplication::instance())
        app->installEventFilter(this);

    // ---- Ctrl+Shift+Z alt-redo -------------------------------------------
    // The menu's Redo QAction owns QKeySequence::Redo (Ctrl+Y on Windows/
    // Linux, ⇧⌘Z on macOS). On Windows/Linux users also expect Ctrl+Shift+Z
    // as an alternate; QAction can only carry one primary shortcut, so we
    // attach the alternate via QShortcut. ApplicationShortcut context lets
    // it fire regardless of focused child widget.
    auto* redoScAlt = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+Z")), this);
    redoScAlt->setContext(Qt::ApplicationShortcut);
    connect(redoScAlt, &QShortcut::activated, this, &MainWindow::redo);

    QTimer::singleShot(0, this, [this]() {
        checkAutosaveRecovery();
    });
}

MainWindow::~MainWindow() = default;

// ==============================================================================
// UI construction
// ==============================================================================
void MainWindow::buildUi()
{
    setStyleSheet(lumenDarkTheme());

    // Top-level menus (File, Edit, View, Window, Plugins, Help). Built in
    // a dedicated method to keep buildUi() focused on widget layout.
    // buildMenus also stores QAction* members for actions that need to be
    // referenced after construction (Undo/Redo enable-state, panel
    // visibility toggles, etc.).
    buildMenus();

    m_workspaceStack = new QStackedWidget(this);
    m_workspaceStack->setObjectName(QStringLiteral("workspaceStack"));

    m_welcomeScreen = new WelcomeScreenWidget(m_workspaceStack);
    if (m_settings)
        m_welcomeScreen->setShowOnStartup(m_settings->showWelcomeOnStartup());
    connect(m_welcomeScreen, &WelcomeScreenWidget::showOnStartupChanged,
            this, [this](bool on) {
        if (m_settings) m_settings->setShowWelcomeOnStartup(on);
    });
    connect(m_welcomeScreen, &WelcomeScreenWidget::openImageRequested,
            this, &MainWindow::onOpenImage);
    connect(m_welcomeScreen, &WelcomeScreenWidget::openProjectRequested,
            this, &MainWindow::onOpenProject);
    connect(m_welcomeScreen, &WelcomeScreenWidget::recentImageRequested,
            this, [this](const QString& path) {
        if (!QFileInfo(path).isFile()) {
            if (m_settings) {
                m_settings->setRecentImages(m_settings->recentImages());
                refreshWelcomeRecentFiles();
            }
            QMessageBox::information(this, tr("Recent Image"),
                                     tr("This image no longer exists."));
            return;
        }
        loadImageFromPath(path);
    });
    connect(m_welcomeScreen, &WelcomeScreenWidget::recentProjectRequested,
            this, [this](const QString& path) {
        if (!QFileInfo(path).isFile()) {
            if (m_settings) {
                m_settings->setRecentProjects(m_settings->recentProjects());
                refreshWelcomeRecentFiles();
            }
            QMessageBox::information(this, tr("Recent Project"),
                                     tr("This project no longer exists."));
            return;
        }
        loadProjectFromPath(path);
    });
    connect(m_welcomeScreen, &WelcomeScreenWidget::newProjectRequested, this,
            [this]() {
        QMessageBox::information(this, tr("New Project"),
                                 tr("New Project is not yet available."));
    });
    connect(m_welcomeScreen, &WelcomeScreenWidget::preferencesRequested,
            this, &MainWindow::onPreferences);
    connect(m_welcomeScreen, &WelcomeScreenWidget::pluginsRequested,
            this, &MainWindow::onPluginManager);
    connect(m_welcomeScreen, &WelcomeScreenWidget::imageFileDropped, this,
            [this](const QString& droppedPath) {
        loadImageFromPath(droppedPath);
    });
    m_workspaceStack->addWidget(m_welcomeScreen);
    refreshWelcomeRecentFiles();

    auto* central = new QWidget(m_workspaceStack);
    m_editorWorkspace = central;
    central->setObjectName(QStringLiteral("centralWorkspace"));
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(10);

    // ---- Top workspace bar -------------------------------------------------
    m_workspaceBar = new QWidget(central);
    m_workspaceBar->setObjectName(QStringLiteral("topWorkspaceBar"));
    {
        auto* barLay = new QHBoxLayout(m_workspaceBar);
        barLay->setContentsMargins(12, 6, 12, 6);
        barLay->setSpacing(10);

        auto* logo = new QLabel(m_workspaceBar);
        logo->setPixmap(QIcon(QStringLiteral(":/icons/lumen_logo_512.png")).pixmap(24, 24));
        logo->setFixedSize(26, 26);
        logo->setAlignment(Qt::AlignCenter);
        barLay->addWidget(logo);

        auto* title = new QLabel(tr("Lumen Photo Studio"), m_workspaceBar);
        QFont titleFont = title->font();
        titleFont.setBold(true);
        title->setFont(titleFont);
        title->setStyleSheet("color: #F0F2F6;");
        barLay->addWidget(title);

        auto* workspace = new QLabel(tr("Edit Workspace"), m_workspaceBar);
        workspace->setStyleSheet(
            "QLabel { color: #CCFF00; background: #16181D;"
            "         border: 1px solid #2A2D35; border-radius: 8px;"
            "         padding: 5px 14px; }");
        barLay->addWidget(workspace);
        barLay->addStretch(1);

        auto* status = new QLabel(tr("Workspace Bar Placeholder"), m_workspaceBar);
        status->setStyleSheet("color: #7A7A7A;");
        barLay->addWidget(status);
    }
    root->addWidget(m_workspaceBar, 0);

    auto* body = new QWidget(central);
    body->setObjectName(QStringLiteral("workspaceBody"));
    auto* bodyLay = new QHBoxLayout(body);
    bodyLay->setContentsMargins(0, 0, 0, 0);
    bodyLay->setSpacing(10);

    // ---- Left vertical tool rail ------------------------------------------
    m_toolRail = new QWidget(body);
    m_toolRail->setObjectName(QStringLiteral("leftToolRail"));
    m_toolRail->setFixedWidth(86);
    {
        auto* railLay = new QVBoxLayout(m_toolRail);
        railLay->setContentsMargins(6, 10, 6, 10);
        railLay->setSpacing(4);

        struct RailAction {
            const char* iconPath;
            const char* text;
            const char* action;
        };
        const RailAction actions[] = {
            { ":/icons/sidebar/presets.svg",   "Library",   "Library"   },
            { ":/icons/sidebar/histogram.svg", "Histogram", "Histogram" },
            { ":/icons/sidebar/tone.svg",      "Tone",      "Tone"      },
            { ":/icons/sidebar/color.svg",     "Color",     "Color"     },
            { ":/icons/sidebar/hsl.svg",       "HSL",       "HSL"       },
            { ":/icons/sidebar/curves.svg",    "Curves",    "Curves"    },
            { ":/icons/sidebar/grading.svg",   "Grading",   "Grading"   },
            { ":/icons/sidebar/lens.svg",      "Lens",      "Lens"      },
            { ":/icons/sidebar/plugins.svg",   "Details",   "Details"   },
            { ":/icons/sidebar/mask.svg",      "Masks",     "Masks"     },
            { ":/icons/sidebar/presets.svg",   "Layers",    "Layers"    },
            { ":/icons/sidebar/nodes.svg",     "Nodes",     "Nodes"     },
            { ":/icons/sidebar/plugins.svg",   "Export",    "Export"    },
        };

        for (const auto& action : actions) {
            auto* btn = new QToolButton(m_toolRail);
            btn->setIcon(QIcon(QString::fromLatin1(action.iconPath)));
            btn->setIconSize(QSize(18, 18));
            btn->setText(tr(action.text));
            btn->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
            btn->setToolTip(tr(action.text));
            btn->setCursor(Qt::PointingHandCursor);
            btn->setFixedSize(72, 48);
            btn->setProperty("navButton", true);
            btn->setCheckable(true);
            const QString actionName = QString::fromLatin1(action.action);
            connect(btn, &QToolButton::clicked, this, [this, actionName, btn]() {
                for (auto* child : m_toolRail->findChildren<QToolButton*>()) {
                    if (child->isCheckable() && child != btn)
                        child->setChecked(false);
                }
                btn->setChecked(true);
                handleRailAction(actionName);
            });
            railLay->addWidget(btn, 0, Qt::AlignHCenter);
        }
        railLay->addStretch(1);
    }
    bodyLay->addWidget(m_toolRail, 0);

    m_analysisPanel = buildAnalysisPanel();
    bodyLay->addWidget(m_analysisPanel, 0);
    setAnalysisPanelCollapsed(m_analysisPanelCollapsed);

    // ---- Preview (left, expanding) ------------------------------------------
    // Two layered widgets:
    //   m_previewLabel — PreviewWidget; handles zoom, pan, fit-mode, double-
    //                    click toggle. Stores its own copies of the original
    //                    and edited preview images so Spacebar before/after
    //                    is just a flag flip, no re-render needed.
    //   m_emptyState   — overlay widget parented to the preview, raised
    //                    above its content. Paints the welcome screen when
    //                    no image is loaded; we hide it after a successful
    //                    load and show it again when the image is cleared.
    //
    // Done as an overlay (not a PreviewWidget subclass) so future multi-
    // document workflows can re-parent or share the same overlay across
    // previews without each preview surface needing its own empty-state
    // machinery.
    auto* canvasHost = new QWidget(central);
    canvasHost->setObjectName(QStringLiteral("canvasHost"));
    auto* canvasLay = new QVBoxLayout(canvasHost);
    canvasLay->setContentsMargins(0, 0, 0, 0);
    canvasLay->setSpacing(0);

    m_documentTabs = new QTabBar(canvasHost);
    m_documentTabs->setObjectName(QStringLiteral("documentTabs"));
    m_documentTabs->setDocumentMode(true);
    m_documentTabs->setExpanding(false);
    m_documentTabs->setMovable(false);
    m_documentTabs->setTabsClosable(true);
    m_documentTabs->setVisible(false);
    connect(m_documentTabs, &QTabBar::currentChanged,
            this, [this](int index) {
        if (m_syncingDocumentTabs) return;
        setActiveDocumentIndex(index);
    });
    connect(m_documentTabs, &QTabBar::tabCloseRequested,
            this, [this](int index) {
        closeDocumentAt(index);
    });
    canvasLay->addWidget(m_documentTabs, 0);

    m_previewLabel = new PreviewWidget(canvasHost);
    m_previewLabel->setObjectName(QStringLiteral("previewSurface"));
    canvasLay->addWidget(m_previewLabel, /*stretch=*/1);
    bodyLay->addWidget(canvasHost, /*stretch=*/1);

    // Overlay: child of m_previewLabel, geometry tracked via resize event
    // forwarding inside the overlay class.
    m_emptyState = new EmptyStateOverlay(m_previewLabel);
    m_emptyState->show();   // visible until first successful load

    // Empty-state click → run the standard Open Image flow, dialog and all.
    connect(m_emptyState, &EmptyStateOverlay::openRequested,
            this, &MainWindow::onOpenImage);

    // Drag-and-drop → bypass the dialog, load the dropped path directly.
    // The unsaved-changes prompt runs first so we don't silently discard
    // work in progress.
    connect(m_emptyState, &EmptyStateOverlay::imageFileDropped, this,
            [this](const QString& droppedPath) {
        loadImageFromPath(droppedPath);
    });

    // Right-click context menu on the preview. Uses Qt::CustomContextMenu
    // so we don't have to subclass PreviewWidget — the signal arrives in
    // widget-local coords, which we map to global for menu positioning.
    m_previewLabel->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_previewLabel, &QWidget::customContextMenuRequested,
            this, &MainWindow::onPreviewContextMenu);

    // Color sampling — Targeted Color Adjustment tool. PreviewWidget emits
    // colorSampled(QColor, QPoint) when sampling mode is active and the
    // user left-clicks. We map the color to an HSL channel and select it.
    connect(m_previewLabel, &PreviewWidget::colorSampled,
            this, &MainWindow::onColorSampled);

    // Mask handle drag — start (push undo) and live geometry change
    // (refresh dependent UI + kick render debounce). Same one-snapshot-
    // per-drag pattern as sliders.
    connect(m_previewLabel, &PreviewWidget::maskHandleDragStarted,
            this, [this]() {
        m_nextHistoryLabel = tr("Mask changed");
        pushUndoSnapshot();
    });
    connect(m_previewLabel, &PreviewWidget::maskGeometryChanged,
            this, &MainWindow::onMaskGeometryChangedFromPreview);
    connect(m_previewLabel, &PreviewWidget::maskBrushSettingsChanged,
            this, [this]() {
        refreshMaskWidgets();
        markDirty();
    });
    connect(m_previewLabel, &PreviewWidget::cropEditStarted,
            this, [this]() {
        m_nextHistoryLabel = tr("Crop changed");
        pushUndoSnapshot();
    });
    connect(m_previewLabel, &PreviewWidget::cropRectChanged,
            this, [this](const QRectF& cropRect) {
        m_look.transform.cropRect = cropRect;
        if (m_debounce) m_debounce->start();
    });
    connect(m_previewLabel, &PreviewWidget::cropEditCommitted,
            this, [this]() {
        if (m_cropToolBtn) {
            QSignalBlocker block(m_cropToolBtn);
            m_cropToolBtn->setChecked(false);
        }
        if (m_previewLabel) m_previewLabel->setCropOverlayActive(false);
        if (m_debounce) m_debounce->start();
    });
    connect(m_previewLabel, &PreviewWidget::cropEditCanceled,
            this, [this](const QRectF& cropRect) {
        m_look.transform.cropRect = cropRect;
        if (m_cropToolBtn) {
            QSignalBlocker block(m_cropToolBtn);
            m_cropToolBtn->setChecked(false);
        }
        if (m_previewLabel) m_previewLabel->setCropOverlayActive(false);
        refreshTransformWidgets();
        if (m_debounce) m_debounce->start();
    });

    // ---- Controls sidebar (right) -------------------------------------------
    // The sidebar is a QStackedLayout container with two children:
    //   m_sidebarFull — the full controls panel (~320 px wide). Returned by
    //                   buildControlPanel; widget tree stays alive across
    //                   collapse/expand cycles, preserving slider values
    //                   and per-section state.
    //   m_sidebarMini — a narrow icon-only strip that lets the user expand
    //                   back to the full view.
    // QStackedLayout shows exactly one child at a time but keeps both alive.
    m_sidebarFull = buildControlPanel();
    m_sidebarFull->setObjectName(QStringLiteral("sidebarFull"));
    m_sidebarFull->setMinimumWidth(320);

    m_sidebarMini = new QWidget(central);
    m_sidebarMini->setObjectName(QStringLiteral("sidebarMini"));
    {
        auto* miniLay = new QVBoxLayout(m_sidebarMini);
        miniLay->setContentsMargins(2, 8, 2, 8);
        miniLay->setSpacing(6);

        // Expand button — single chevron-style toggle. Clicking it returns
        // to the full sidebar. Section labels below are also expand actions
        // (clicking any of them brings the full panel back); this gives
        // users the spatial cue that the panels are still there.
        auto* expandBtn = new QToolButton(m_sidebarMini);
        expandBtn->setText(QStringLiteral("‹"));
        expandBtn->setToolTip(tr("Expand controls panel"));
        expandBtn->setCursor(Qt::PointingHandCursor);
        expandBtn->setFixedSize(32, 32);
        expandBtn->setProperty("navButton", true);
        connect(expandBtn, &QToolButton::clicked,
                this, &MainWindow::onToggleSidebar);
        miniLay->addWidget(expandBtn, 0, Qt::AlignHCenter);

        miniLay->addSpacing(8);

        // Section icons. SVGs use stroke="currentColor" so a stylesheet
        // can recolor them via the QToolButton's text color — inactive
        // #7A7A7A, hover #CCFF00 (the brand accent). Clicking any icon
        // re-expands the sidebar; future work could scroll-to the
        // corresponding section after expanding.
        // Each row: icon path, tooltip, and a per-row action enum.
        // Most rows just toggle the sidebar (the user wanted to see the
        // section but the strip is collapsed); the Nodes row instead
        // opens the Node Graph dock.
        enum class StripAction { ToggleSidebar, OpenNodeGraph };
        struct StripSection {
            const char* iconPath;
            const char* tip;
            StripAction action;
        };
        const StripSection kSections[] = {
            { ":/icons/sidebar/histogram.svg", "Histogram",     StripAction::ToggleSidebar },
            { ":/icons/sidebar/tone.svg",      "Tone",          StripAction::ToggleSidebar },
            { ":/icons/sidebar/color.svg",     "Color",         StripAction::ToggleSidebar },
            { ":/icons/sidebar/hsl.svg",       "HSL",           StripAction::ToggleSidebar },
            { ":/icons/sidebar/curves.svg",    "Curves",        StripAction::ToggleSidebar },
            { ":/icons/sidebar/grading.svg",   "Color Grading", StripAction::ToggleSidebar },
            { ":/icons/sidebar/lens.svg",      "Lens Correction", StripAction::ToggleSidebar },
            { ":/icons/sidebar/mask.svg",      "Masks",         StripAction::ToggleSidebar },
            { ":/icons/sidebar/nodes.svg",     "Node Graph",    StripAction::OpenNodeGraph },
            { ":/icons/sidebar/presets.svg",   "Presets",       StripAction::ToggleSidebar },
            { ":/icons/sidebar/plugins.svg",   "Plugins",       StripAction::ToggleSidebar },
        };
        // Stylesheet recipe: SVGs are loaded as QIcon; the surrounding
        // QToolButton text color drives currentColor in the rendered
        // SVG via the icon engine's color binding only when the icon
        // is rendered with a tint. Since QIcon's default rendering
        // doesn't apply text color to SVG strokes, we instead style
        // the button background/border for state feedback. The icon
        // itself stays its authored color (which is currentColor →
        // resolved by Qt's SVG renderer to the default text color).
        const QString miniBtnQss =
            "QToolButton { background: transparent; border: 1px solid transparent;"
            "              border-radius: 8px; padding: 4px;"
            "              color: #7A7A7A; }"
            "QToolButton:hover { background: #1E2026; color: #B0B0B0;"
            "                    border-color: #2A2D35; }"
            "QToolButton:checked { background: rgba(204, 255, 0, 31);"
            "                       color: #CCFF00; border-color: #CCFF00; }";
        for (const auto& s : kSections) {
            auto* btn = new QToolButton(m_sidebarMini);
            btn->setIcon(QIcon(QString::fromLatin1(s.iconPath)));
            btn->setIconSize(QSize(20, 20));
            btn->setToolTip(tr(s.tip));
            btn->setCursor(Qt::PointingHandCursor);
            btn->setFixedSize(32, 32);
            btn->setProperty("navButton", true);
            btn->setStyleSheet(miniBtnQss);
            if (s.action == StripAction::OpenNodeGraph)
                btn->setCheckable(true);
            switch (s.action) {
            case StripAction::ToggleSidebar:
                connect(btn, &QToolButton::clicked,
                        this, &MainWindow::onToggleSidebar);
                break;
            case StripAction::OpenNodeGraph:
                connect(btn, &QToolButton::clicked,
                        this, &MainWindow::onShowNodeGraph);
                break;
            }
            miniLay->addWidget(btn, 0, Qt::AlignHCenter);
        }
        miniLay->addStretch(1);
    }

    m_sidebarHost  = new QWidget(central);
    m_sidebarHost->setObjectName(QStringLiteral("sidebarHost"));
    m_sidebarStack = new QStackedLayout(m_sidebarHost);
    m_sidebarStack->setContentsMargins(0, 0, 0, 0);
    m_sidebarStack->addWidget(m_sidebarFull);
    m_sidebarStack->addWidget(m_sidebarMini);
    m_sidebarStack->setCurrentWidget(m_sidebarFull);   // start expanded
    bodyLay->addWidget(m_sidebarHost, /*stretch=*/0);

    root->addWidget(body, /*stretch=*/1);

    m_workspaceStack->addWidget(central);
    setCentralWidget(m_workspaceStack);

    // ---- Bottom dock panel -------------------------------------------------
    m_nodeGraph = new NodeGraphWidget(this);
    m_nodeGraph->setMinimumHeight(180);

    m_bottomWorkspaceContainer = new QWidget(this);
    m_bottomWorkspaceContainer->setObjectName(QStringLiteral("bottomWorkspaceContainer"));
    auto* bottomLay = new QVBoxLayout(m_bottomWorkspaceContainer);
    bottomLay->setContentsMargins(0, 0, 0, 0);
    bottomLay->setSpacing(0);

    auto* bottomHeader = new QFrame(m_bottomWorkspaceContainer);
    bottomHeader->setObjectName(QStringLiteral("bottomWorkspaceHeader"));
    auto* bottomHeaderLay = new QHBoxLayout(bottomHeader);
    bottomHeaderLay->setContentsMargins(10, 6, 10, 6);
    bottomHeaderLay->setSpacing(8);

    auto* bottomTitle = new QLabel(tr("Bottom Workspace"), bottomHeader);
    bottomTitle->setStyleSheet("color: #E7E9EE; font-weight: 700;");
    bottomHeaderLay->addWidget(bottomTitle);
    bottomHeaderLay->addStretch(1);

    m_bottomWorkspaceCollapseBtn = new QToolButton(bottomHeader);
    m_bottomWorkspaceCollapseBtn->setText(QStringLiteral("v"));
    m_bottomWorkspaceCollapseBtn->setToolTip(tr("Collapse bottom workspace"));
    m_bottomWorkspaceCollapseBtn->setCursor(Qt::PointingHandCursor);
    m_bottomWorkspaceCollapseBtn->setFixedSize(28, 24);
    m_bottomWorkspaceCollapseBtn->setProperty("navButton", true);
    connect(m_bottomWorkspaceCollapseBtn, &QToolButton::clicked, this, [this]() {
        setBottomWorkspaceCollapsed(!m_bottomWorkspaceCollapsed);
    });
    bottomHeaderLay->addWidget(m_bottomWorkspaceCollapseBtn);
    bottomLay->addWidget(bottomHeader, 0);

    m_bottomPanelTabs = new QTabWidget(this);
    m_bottomPanelTabs->setObjectName(QStringLiteral("bottomPanelTabs"));
    m_bottomPanelTabs->addTab(m_nodeGraph, tr("Nodes"));

    auto* layersTab = new QWidget(m_bottomPanelTabs);
    auto* layersLay = new QVBoxLayout(layersTab);
    layersLay->setContentsMargins(14, 12, 14, 12);
    auto* layersLabel = new QLabel(tr("Layers panel placeholder"), layersTab);
    layersLabel->setAlignment(Qt::AlignCenter);
    layersLabel->setStyleSheet("color: #7A7A7A;");
    layersLay->addWidget(layersLabel, 1);
    m_bottomPanelTabs->addTab(layersTab, tr("Layers"));

    auto* historyTab = new QWidget(m_bottomPanelTabs);
    auto* historyLay = new QVBoxLayout(historyTab);
    historyLay->setContentsMargins(14, 12, 14, 12);
    historyLay->setSpacing(8);

    m_historyList = new QListWidget(historyTab);
    m_historyList->setObjectName(QStringLiteral("historyList"));
    m_historyList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_historyList->setUniformItemSizes(true);
    m_historyList->setStyleSheet(
        "QListWidget { background-color: #111318;"
        " border: 1px solid #2A2D35; border-radius: 8px; padding: 6px; }"
        "QListWidget::item { padding: 6px 8px; border-radius: 5px; }"
        "QListWidget::item:selected { background: rgba(204, 255, 0, 36);"
        " color: #CCFF00; }");
    connect(m_historyList, &QListWidget::itemClicked, this,
            [this](QListWidgetItem*) {
        if (m_syncingHistorySelection) return;
        refreshHistoryList();
    });
    historyLay->addWidget(m_historyList, 1);
    m_bottomPanelTabs->addTab(historyTab, tr("History"));
    bottomLay->addWidget(m_bottomPanelTabs, 1);

    m_nodeGraphDock = new QDockWidget(tr("Workspace"), this);
    m_nodeGraphDock->setObjectName(QStringLiteral("bottomWorkspaceDock"));
    m_nodeGraphDock->setAllowedAreas(Qt::BottomDockWidgetArea);
    m_nodeGraphDock->setFeatures(
        QDockWidget::DockWidgetMovable
      | QDockWidget::DockWidgetFloatable
      | QDockWidget::DockWidgetClosable);
    m_nodeGraphDock->setWidget(m_bottomWorkspaceContainer);
    addDockWidget(Qt::BottomDockWidgetArea, m_nodeGraphDock);
    connect(m_nodeGraphDock, &QDockWidget::visibilityChanged,
            this, [this](bool visible) {
        if (m_syncingBottomWorkspaceVisibility) return;
        if (!m_editorWorkspaceActive) return;
        m_bottomWorkspaceEnabled = visible;
        if (m_settings) m_settings->setBottomWorkspaceVisible(visible);
        if (m_actShowBottomWorkspace) {
            QSignalBlocker block(m_actShowBottomWorkspace);
            m_actShowBottomWorkspace->setChecked(visible);
        }
    });

    // Initial title — no project, no image yet.
    updateWindowTitle();
    setBottomWorkspaceCollapsed(m_bottomWorkspaceCollapsed);
    if (!m_documents.empty() && m_settings && !m_settings->showWelcomeOnStartup())
        showEditorWorkspace();
    else
        showWelcomeScreen();
}

void MainWindow::showWelcomeScreen()
{
    m_editorWorkspaceActive = false;
    if (m_workspaceStack)
        m_workspaceStack->setCurrentIndex(0);
    updateBottomWorkspaceVisibility();
}

void MainWindow::showEditorWorkspace()
{
    m_editorWorkspaceActive = true;
    if (m_workspaceStack)
        m_workspaceStack->setCurrentIndex(1);
    updateBottomWorkspaceVisibility();
}

MainWindow::ImageDocument MainWindow::makeDocumentFromCurrentState() const
{
    ImageDocument document;
    document.imagePath = m_currentImagePath;
    document.projectPath = m_currentProjectPath;
    document.projectCreatedDate = m_projectCreatedDate;
    document.projectModifiedDate = m_projectModifiedDate;
    document.originalFullRes = m_originalFullRes;
    document.previewSource = m_previewSource;
    document.processed = m_processed;
    document.look = m_look;
    document.dirty = m_projectDirty;
    document.undoStack = m_undoStack;
    document.redoStack = m_redoStack;
    document.historyEntries = m_historyEntries;
    document.historyCurrentIndex = m_historyCurrentIndex;
    document.nextHistoryLabel = m_nextHistoryLabel;
    document.selectedMaskIndex = m_selectedMaskIndex;
    document.selectedLayerIndex = m_selectedLayerIndex;
    return document;
}

MainWindow::ImageDocument* MainWindow::activeDocument()
{
    if (m_activeDocumentIndex < 0 ||
        m_activeDocumentIndex >= static_cast<int>(m_documents.size())) {
        return nullptr;
    }
    return &m_documents[static_cast<size_t>(m_activeDocumentIndex)];
}

const MainWindow::ImageDocument* MainWindow::activeDocument() const
{
    if (m_activeDocumentIndex < 0 ||
        m_activeDocumentIndex >= static_cast<int>(m_documents.size())) {
        return nullptr;
    }
    return &m_documents[static_cast<size_t>(m_activeDocumentIndex)];
}

QString MainWindow::documentTitle(const ImageDocument& document) const
{
    if (!document.projectPath.isEmpty())
        return QFileInfo(document.projectPath).fileName();
    if (!document.imagePath.isEmpty())
        return QFileInfo(document.imagePath).fileName();
    return tr("Untitled");
}

void MainWindow::saveActiveDocumentState()
{
    if (m_restoringDocument || m_isLoadingProject)
        return;
    if (m_debounce && m_debounce->isActive() && !m_isLoadingProject) {
        m_projectDirty = true;
        if (m_historyCurrentIndex >= 0 &&
            m_historyCurrentIndex < static_cast<int>(m_historyEntries.size())) {
            m_historyEntries[static_cast<size_t>(m_historyCurrentIndex)].snapshot = m_look;
        }
        scheduleAutosave();
    }
    if (auto* document = activeDocument())
        *document = makeDocumentFromCurrentState();
}

int MainWindow::appendCurrentStateAsDocument()
{
    m_documents.push_back(makeDocumentFromCurrentState());
    m_activeDocumentIndex = static_cast<int>(m_documents.size()) - 1;
    updateDocumentTabs();
    return m_activeDocumentIndex;
}

bool MainWindow::setActiveDocumentIndex(int index)
{
    if (index < 0 || index >= static_cast<int>(m_documents.size()))
        return false;
    if (index == m_activeDocumentIndex) {
        updateDocumentTabs();
        return true;
    }

    saveActiveDocumentState();
    m_activeDocumentIndex = index;

    const ImageDocument& document = m_documents[static_cast<size_t>(index)];
    ++m_generation;
    m_pendingRender = false;
    m_showOriginal = false;

    m_restoringDocument = true;
    m_isLoadingProject = true;

    m_currentImagePath = document.imagePath;
    m_currentProjectPath = document.projectPath;
    m_projectCreatedDate = document.projectCreatedDate;
    m_projectModifiedDate = document.projectModifiedDate;
    m_originalFullRes = document.originalFullRes;
    m_previewSource = document.previewSource;
    m_processed = document.processed;
    m_look = document.look;
    m_projectDirty = document.dirty;
    m_undoStack = document.undoStack;
    m_redoStack = document.redoStack;
    m_historyEntries = document.historyEntries;
    m_historyCurrentIndex = document.historyCurrentIndex;
    m_nextHistoryLabel = document.nextHistoryLabel;
    m_selectedMaskIndex = document.selectedMaskIndex;
    m_selectedLayerIndex = document.selectedLayerIndex;

    if (m_previewLabel) {
        m_previewLabel->setCropOverlayActive(false);
        m_previewLabel->setShowOriginal(false);
    }
    if (m_cropToolBtn) {
        QSignalBlocker block(m_cropToolBtn);
        m_cropToolBtn->setChecked(false);
    }

    applyLookToUi();
    if (m_debounce) m_debounce->stop();

    m_isLoadingProject = false;
    m_restoringDocument = false;

    refreshUndoRedoActions();
    refreshHistoryList();
    updateMetadataPanel();
    updateNavigatorPreview();

    if (m_originalFullRes.isNull()) {
        if (m_emptyState) m_emptyState->show();
        if (m_histogramWidget) m_histogramWidget->setImage(QImage());
        if (m_secondaryViewer) m_secondaryViewer->setImage(QImage());
    } else {
        if (m_emptyState) m_emptyState->hide();
        if (m_histogramWidget)
            m_histogramWidget->setImage(!m_processed.isNull() ? m_processed : m_previewSource);
        if (m_secondaryViewer) m_secondaryViewer->setImage(m_processed);
    }

    refreshPreviewLabel();
    if (m_processed.isNull() && !m_previewSource.isNull())
        requestRender();

    updateDocumentTabs();
    updateWindowTitle();
    showEditorWorkspace();
    return true;
}

bool MainWindow::closeDocumentAt(int index)
{
    if (index < 0 || index >= static_cast<int>(m_documents.size()))
        return false;

    if (index == m_activeDocumentIndex)
        saveActiveDocumentState();
    if (!maybePromptSaveDocument(index))
        return false;

    int nextIndex = m_activeDocumentIndex;
    if (index == m_activeDocumentIndex) {
        nextIndex = index;
    } else if (index < m_activeDocumentIndex) {
        --nextIndex;
    }

    m_documents.erase(m_documents.begin() + index);
    if (m_documents.empty()) {
        m_activeDocumentIndex = -1;
        clearEditorStateForNoDocuments();
        return true;
    }

    if (nextIndex >= static_cast<int>(m_documents.size()))
        nextIndex = static_cast<int>(m_documents.size()) - 1;
    m_activeDocumentIndex = -1;
    setActiveDocumentIndex(nextIndex);
    return true;
}

void MainWindow::updateDocumentTabs()
{
    if (!m_documentTabs)
        return;

    m_syncingDocumentTabs = true;
    while (m_documentTabs->count() > 0)
        m_documentTabs->removeTab(0);

    for (int i = 0; i < static_cast<int>(m_documents.size()); ++i) {
        const ImageDocument& document = m_documents[static_cast<size_t>(i)];
        QString title = documentTitle(document);
        if (document.dirty)
            title += QStringLiteral(" *");
        m_documentTabs->addTab(title);
        m_documentTabs->setTabToolTip(i, document.projectPath.isEmpty()
            ? document.imagePath
            : document.projectPath);
    }

    m_documentTabs->setVisible(!m_documents.empty());
    if (m_activeDocumentIndex >= 0 &&
        m_activeDocumentIndex < m_documentTabs->count()) {
        m_documentTabs->setCurrentIndex(m_activeDocumentIndex);
    }
    m_syncingDocumentTabs = false;
}

void MainWindow::clearEditorStateForNoDocuments()
{
    ++m_generation;
    m_pendingRender = false;
    m_showOriginal = false;
    m_currentImagePath.clear();
    m_currentProjectPath.clear();
    m_projectCreatedDate = QDateTime();
    m_projectModifiedDate = QDateTime();
    m_originalFullRes = QImage();
    m_previewSource = QImage();
    m_processed = QImage();
    m_look = lps::Look{};
    m_projectDirty = false;
    m_undoStack.clear();
    m_redoStack.clear();
    m_historyEntries.clear();
    m_historyCurrentIndex = -1;
    m_nextHistoryLabel.clear();
    m_selectedMaskIndex = -1;
    m_selectedLayerIndex = -1;

    m_isLoadingProject = true;
    applyLookToUi();
    if (m_debounce) m_debounce->stop();
    m_isLoadingProject = false;

    refreshUndoRedoActions();
    refreshHistoryList();
    updateMetadataPanel();
    updateNavigatorPreview();
    if (m_histogramWidget) m_histogramWidget->setImage(QImage());
    if (m_previewLabel) {
        m_previewLabel->setCropOverlayActive(false);
        m_previewLabel->setOriginalImage(QImage());
        m_previewLabel->setEditedImage(QImage());
        m_previewLabel->setShowOriginal(false);
        m_previewLabel->zoomToFit();
    }
    if (m_secondaryViewer) m_secondaryViewer->setImage(QImage());
    if (m_emptyState) m_emptyState->show();
    updateDocumentTabs();
    updateWindowTitle();
    showWelcomeScreen();
}

void MainWindow::updateBottomWorkspaceVisibility()
{
    if (!m_nodeGraphDock) return;

    m_syncingBottomWorkspaceVisibility = true;
    m_nodeGraphDock->setVisible(m_editorWorkspaceActive && m_bottomWorkspaceEnabled);
    m_syncingBottomWorkspaceVisibility = false;

    if (m_actShowBottomWorkspace) {
        QSignalBlocker block(m_actShowBottomWorkspace);
        m_actShowBottomWorkspace->setChecked(m_bottomWorkspaceEnabled);
    }
}

void MainWindow::setBottomWorkspaceCollapsed(bool collapsed)
{
    m_bottomWorkspaceCollapsed = collapsed;
    if (m_settings)
        m_settings->setBottomWorkspaceCollapsed(collapsed);
    if (m_bottomPanelTabs)
        m_bottomPanelTabs->setVisible(!collapsed);

    if (m_bottomWorkspaceCollapseBtn) {
        m_bottomWorkspaceCollapseBtn->setText(collapsed
            ? QStringLiteral("^")
            : QStringLiteral("v"));
        m_bottomWorkspaceCollapseBtn->setToolTip(collapsed
            ? tr("Expand bottom workspace")
            : tr("Collapse bottom workspace"));
    }

    if (m_nodeGraphDock) {
        m_nodeGraphDock->setMinimumHeight(collapsed ? 56 : 180);
        m_nodeGraphDock->setMaximumHeight(collapsed ? 76 : QWIDGETSIZE_MAX);
    }
}

void MainWindow::refreshWelcomeRecentFiles()
{
    if (!m_welcomeScreen || !m_settings) return;
    m_welcomeScreen->setRecentItems(m_settings->recentImages(),
                                    m_settings->recentProjects());
}

QWidget* MainWindow::buildAnalysisPanel()
{
    auto* outer = new QWidget(this);
    outer->setObjectName(QStringLiteral("analysisPanel"));
    outer->setMinimumWidth(230);
    outer->setMaximumWidth(260);

    auto* outerLay = new QVBoxLayout(outer);
    outerLay->setContentsMargins(0, 0, 0, 0);
    outerLay->setSpacing(0);

    auto* header = new QWidget(outer);
    header->setObjectName(QStringLiteral("analysisHeader"));
    auto* headerLay = new QHBoxLayout(header);
    headerLay->setContentsMargins(10, 7, 8, 7);
    headerLay->setSpacing(6);

    auto* title = new QLabel(tr("Analysis"), header);
    QFont titleFont = title->font();
    titleFont.setBold(true);
    title->setFont(titleFont);
    title->setStyleSheet("color: #E7E9EE;");
    headerLay->addWidget(title);
    headerLay->addStretch(1);

    m_analysisCollapseBtn = new QToolButton(header);
    m_analysisCollapseBtn->setText(QStringLiteral("<"));
    m_analysisCollapseBtn->setToolTip(tr("Collapse analysis panel"));
    m_analysisCollapseBtn->setCursor(Qt::PointingHandCursor);
    m_analysisCollapseBtn->setFixedSize(28, 24);
    m_analysisCollapseBtn->setProperty("navButton", true);
    connect(m_analysisCollapseBtn, &QToolButton::clicked, this,
            [this]() { setAnalysisPanelCollapsed(true); });
    headerLay->addWidget(m_analysisCollapseBtn);
    outerLay->addWidget(header);

    m_analysisScroll = new QScrollArea(outer);
    m_analysisScroll->setObjectName(QStringLiteral("analysisScroll"));
    m_analysisScroll->setWidgetResizable(true);
    m_analysisScroll->setFrameShape(QFrame::NoFrame);

    auto* panel = new QWidget(m_analysisScroll);
    panel->setObjectName(QStringLiteral("analysisBody"));
    auto* col = new QVBoxLayout(panel);
    col->setContentsMargins(10, 10, 10, 12);
    col->setSpacing(10);

    auto makeCard = [panel]() {
        auto* card = new QFrame(panel);
        card->setObjectName(QStringLiteral("analysisCard"));
        auto* lay = new QVBoxLayout(card);
        lay->setContentsMargins(10, 9, 10, 10);
        lay->setSpacing(7);
        return std::pair<QFrame*, QVBoxLayout*>(card, lay);
    };

    auto makeCardTitle = [](const QString& text, QWidget* parent) {
        auto* row = new QWidget(parent);
        auto* lay = new QHBoxLayout(row);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(6);
        auto* label = new QLabel(text, row);
        QFont font = label->font();
        font.setBold(true);
        label->setFont(font);
        label->setStyleSheet("color: #E7E9EE;");
        lay->addWidget(label);
        lay->addStretch(1);
        auto* dots = new QLabel(QStringLiteral("..."), row);
        dots->setStyleSheet("color: #9EA4AE;");
        lay->addWidget(dots);
        return row;
    };

    {
        auto [card, lay] = makeCard();
        lay->addWidget(makeCardTitle(tr("Histogram"), card));
        m_histogramWidget = new HistogramWidget(card);
        m_histogramWidget->setMinimumHeight(110);
        lay->addWidget(m_histogramWidget);
        col->addWidget(card);
    }

    {
        auto [card, lay] = makeCard();
        lay->addWidget(makeCardTitle(tr("Navigator"), card));

        m_navigatorPreview = new QLabel(card);
        m_navigatorPreview->setObjectName(QStringLiteral("navigatorPreview"));
        m_navigatorPreview->setAlignment(Qt::AlignCenter);
        m_navigatorPreview->setMinimumHeight(120);
        m_navigatorPreview->setText(tr("No image"));
        m_navigatorPreview->setStyleSheet("color: #7A7A7A;");
        lay->addWidget(m_navigatorPreview);

        auto* zoomRow = new QWidget(card);
        auto* zoomLay = new QHBoxLayout(zoomRow);
        zoomLay->setContentsMargins(0, 0, 0, 0);
        zoomLay->setSpacing(4);
        auto addZoomButton = [this, zoomLay, zoomRow](const QString& text,
                                                       void (MainWindow::*slot)()) {
            auto* btn = new QPushButton(text, zoomRow);
            btn->setCursor(Qt::PointingHandCursor);
            btn->setMinimumWidth(40);
            connect(btn, &QPushButton::clicked, this, slot);
            zoomLay->addWidget(btn);
        };
        addZoomButton(tr("Fit"),  &MainWindow::onZoomFit);
        addZoomButton(tr("Fill"), &MainWindow::onZoomFit);
        addZoomButton(tr("1:1"),  &MainWindow::onZoom100);
        addZoomButton(tr("100%"), &MainWindow::onZoom100);
        lay->addWidget(zoomRow);

        col->addWidget(card);
    }

    {
        auto [card, lay] = makeCard();
        lay->addWidget(makeCardTitle(tr("Metadata"), card));

        auto addMetadataRow = [lay, card](const QString& name,
                                          QLabel*& valueLabel) {
            auto* row = new QWidget(card);
            auto* rowLay = new QHBoxLayout(row);
            rowLay->setContentsMargins(0, 0, 0, 0);
            rowLay->setSpacing(8);

            auto* nameLabel = new QLabel(name, row);
            nameLabel->setObjectName(QStringLiteral("metadataName"));
            nameLabel->setMinimumWidth(82);
            rowLay->addWidget(nameLabel, 0);

            valueLabel = new QLabel(QStringLiteral("--"), row);
            valueLabel->setObjectName(QStringLiteral("metadataValue"));
            valueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            valueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
            rowLay->addWidget(valueLabel, 1);

            lay->addWidget(row);
        };

        addMetadataRow(tr("File Name"),      m_metaFileName);
        addMetadataRow(tr("Dimensions"),     m_metaDimensions);
        addMetadataRow(tr("Date Time"),      m_metaDateTime);
        addMetadataRow(tr("ISO"),            m_metaIso);
        addMetadataRow(tr("Focal Length"),   m_metaFocalLength);
        addMetadataRow(tr("Aperture"),       m_metaAperture);
        addMetadataRow(tr("Shutter Speed"),  m_metaShutterSpeed);
        addMetadataRow(tr("Camera Model"),   m_metaCameraModel);
        addMetadataRow(tr("Lens Model"),     m_metaLensModel);

        col->addWidget(card);
    }

    col->addStretch(1);
    m_analysisScroll->setWidget(panel);
    outerLay->addWidget(m_analysisScroll, 1);

    updateMetadataPanel();
    updateNavigatorPreview();
    return outer;
}

QWidget* MainWindow::buildControlPanel()
{
    // Outer wrapper: a fixed-height header row (collapse button) on top of
    // a scrollable region with all the actual controls. Wrapping like this
    // keeps the collapse toggle pinned at the top regardless of how far
    // the user has scrolled inside the controls.
    auto* outer = new QWidget(this);
    outer->setObjectName(QStringLiteral("controlPanelCard"));
    auto* outerLay = new QVBoxLayout(outer);
    outerLay->setContentsMargins(0, 0, 0, 0);
    outerLay->setSpacing(0);

    // ---- Header row with collapse toggle ----------------------------------
    {
        auto* header = new QWidget(outer);
        header->setObjectName(QStringLiteral("controlHeader"));
        auto* headerLay = new QHBoxLayout(header);
        headerLay->setContentsMargins(8, 7, 8, 7);
        headerLay->addStretch(1);

        auto* collapseBtn = new QToolButton(header);
        collapseBtn->setText(QStringLiteral("›"));
        collapseBtn->setToolTip(tr("Collapse controls panel"));
        collapseBtn->setCursor(Qt::PointingHandCursor);
        collapseBtn->setFixedSize(28, 24);
        collapseBtn->setProperty("navButton", true);
        connect(collapseBtn, &QToolButton::clicked,
                this, &MainWindow::onToggleSidebar);
        headerLay->addWidget(collapseBtn);
        outerLay->addWidget(header);
    }

    // ---- Scrollable content area ------------------------------------------
    // Scrollable so the controls don't get clipped on short windows.
    auto* scroll = new QScrollArea(outer);
    scroll->setObjectName(QStringLiteral("controlScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    m_controlScroll = scroll;

    auto* panel = new QWidget(scroll);
    panel->setObjectName(QStringLiteral("controlPanelBody"));
    auto* col = new QVBoxLayout(panel);
    col->setContentsMargins(12, 10, 12, 12);
    col->setSpacing(8);

    // ---- View mode readout ------------------------------------------------
    // Shows "Edited" by default, "Original" while Spacebar is held down.
    // Purely informational — the press-and-hold gesture is the interaction.
    // (File operations now live in the File menu — see buildUi().)
    m_viewModeLabel = new QLabel(tr("Edited"), panel);
    m_viewModeLabel->setAlignment(Qt::AlignCenter);
    m_viewModeLabel->setStyleSheet("color: #8a8a90; padding: 2px 0;");
    col->addWidget(m_viewModeLabel);

    // Separator
    auto* sep = new QFrame(panel);
    sep->setFrameShape(QFrame::HLine);
    sep->setFrameShadow(QFrame::Sunken);
    col->addWidget(sep);

    // Bold font used for every section header (TONE, COLOR, HSL, CURVES,
    // etc.). Hoisted to the top so we can reuse it across sections
    // without repeating the QFont setup.
    QFont hf = panel->font();
    hf.setBold(true);

    // ---- Section header ----------------------------------------------------
    auto* toneHeader = new QLabel(tr("TONE"), panel);
    m_toneSection = toneHeader;
    toneHeader->setFont(hf);
    toneHeader->setStyleSheet("color: #9A9AA0; padding-top: 4px;");
    col->addWidget(toneHeader);

    // ---- Sliders -----------------------------------------------------------
    // Exposure is in stops, slider range ±500 → ±5.0 stops (scale factor 100).
    // The rest are -100..+100 scalars mapped 1:1 onto lps::Look fields.

    col->addWidget(buildSliderRow(tr("Exposure"),
                                  -500, +500, 0,
                                  m_exposureSlider, m_exposureValue));

    col->addWidget(buildSliderRow(tr("Contrast"),
                                  -100, +100, 0,
                                  m_contrastSlider, m_contrastValue));

    col->addWidget(buildSliderRow(tr("Highlights"),
                                  -100, +100, 0,
                                  m_highlightsSlider, m_highlightsValue));

    col->addWidget(buildSliderRow(tr("Shadows"),
                                  -100, +100, 0,
                                  m_shadowsSlider, m_shadowsValue));

    col->addWidget(buildSliderRow(tr("Whites"),
                                  -100, +100, 0,
                                  m_whitesSlider, m_whitesValue));

    col->addWidget(buildSliderRow(tr("Blacks"),
                                  -100, +100, 0,
                                  m_blacksSlider, m_blacksValue));

    col->addWidget(buildSliderRow(tr("Brightness"),
                                  -100, +100, 0,
                                  m_brightnessSlider, m_brightnessValue));

    // ---- Section header: COLOR --------------------------------------------
    auto* colorHeader = new QLabel(tr("COLOR"), panel);
    m_colorSection = colorHeader;
    colorHeader->setFont(hf);   // reuse the bold font set up for TONE
    colorHeader->setStyleSheet("color: #9A9AA0; padding-top: 4px;");
    col->addWidget(colorHeader);

    // ---- Color sliders ----------------------------------------------------
    // All four are -100..+100 integer sliders mapped 1:1 onto the matching
    // lps::Look.color fields.

    col->addWidget(buildSliderRow(tr("Temperature"),
                                  -100, +100, 0,
                                  m_temperatureSlider, m_temperatureValue));

    col->addWidget(buildSliderRow(tr("Tint"),
                                  -100, +100, 0,
                                  m_tintSlider, m_tintValue));

    col->addWidget(buildSliderRow(tr("Vibrance"),
                                  -100, +100, 0,
                                  m_vibranceSlider, m_vibranceValue));

    col->addWidget(buildSliderRow(tr("Saturation"),
                                  -100, +100, 0,
                                  m_saturationSlider, m_saturationValue));

    // ---- Section header: HSL ----------------------------------------------
    // Lightroom-inspired selective color: click a channel button to focus
    // the three sliders on that channel's hue/saturation/luminance fields.
    // Eight buttons replace what used to be 24 sliders; the three sliders
    // are shared across channels and re-populated on selection change.
    auto* hslHeader = new QLabel(tr("HSL"), panel);
    m_hslSection = hslHeader;
    hslHeader->setFont(hf);
    hslHeader->setStyleSheet("color: #9A9AA0; padding-top: 4px;");
    col->addWidget(hslHeader);

    // ---- Channel selector: 4×2 grid of toggle buttons ---------------------
    auto* buttonGrid = new QWidget(panel);
    auto* grid = new QGridLayout(buttonGrid);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(4);

    // Common stylesheet applied to every channel button. `:checked` picks
    // up the selected state set via setChecked() or by QButtonGroup's
    // exclusive-toggle behavior. The per-button background color is set
    // individually just below; this sheet handles the shared chrome.
    const QString btnSheet = QStringLiteral(
        "QPushButton { color: #d0d0d4; border: 1px solid #3a3a3f;"
        "              padding: 4px 6px; border-radius: 3px; }"
        "QPushButton:hover   { border-color: #6a6a70; }"
        "QPushButton:checked { border: 1px solid #d0d0d4;"
        "                      font-weight: bold; }"
    );

    for (int i = 0; i < kHslChannelCount; ++i) {
        auto* btn = new QPushButton(tr(kHslChannels[i].displayName), buttonGrid);
        btn->setCheckable(true);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setStyleSheet(btnSheet +
            QString("QPushButton { background-color: %1; }")
                .arg(QLatin1String(kHslChannels[i].tintHex)));
        m_hslChannelButtons[i] = btn;

        // 4 columns × 2 rows. Red at (0,0), Magenta at (1,3).
        grid->addWidget(btn, i / 4, i % 4);

        // Capture `i` by value so each button knows its own channel index.
        connect(btn, &QPushButton::clicked, this, [this, i]() {
            selectHslChannel(i);
        });
    }
    col->addWidget(buttonGrid);

    // ---- Three shared sliders --------------------------------------------
    col->addWidget(buildSliderRow(tr("Hue"),
                                  -100, +100, 0,
                                  m_hslHueSlider, m_hslHueValue));
    col->addWidget(buildSliderRow(tr("Saturation"),
                                  -100, +100, 0,
                                  m_hslSaturationSlider, m_hslSaturationValue));
    col->addWidget(buildSliderRow(tr("Luminance"),
                                  -100, +100, 0,
                                  m_hslLuminanceSlider, m_hslLuminanceValue));

    // ---- Targeted Color Adjustment Tool (eyedropper) ----------------------
    // Toggle button below the sliders. When checked, PreviewWidget enters
    // sampling mode (crosshair cursor, left-click samples a pixel and
    // emits colorSampled). The status label gives feedback before and
    // after a sample.
    {
        m_hslTargetButton = new QPushButton(tr("⊙ Target Color"), panel);
        m_hslTargetButton->setCheckable(true);
        m_hslTargetButton->setCursor(Qt::PointingHandCursor);
        m_hslTargetButton->setToolTip(
            tr("Click a color in the image to select the matching HSL channel"));
        // Subtle accent when checked so users see the tool is armed.
        m_hslTargetButton->setStyleSheet(
            "QPushButton { padding: 4px 8px; }"
            "QPushButton:checked { background-color: #3a5577;"
            "                      color: #ffffff; font-weight: bold; }");
        col->addWidget(m_hslTargetButton);

        m_hslTargetStatus = new QLabel(tr("Click color in image"), panel);
        m_hslTargetStatus->setAlignment(Qt::AlignCenter);
        m_hslTargetStatus->setStyleSheet("color: #8a8a90; font-style: italic;");
        col->addWidget(m_hslTargetStatus);

        connect(m_hslTargetButton, &QPushButton::toggled, this,
                [this](bool on) {
            if (m_previewLabel) m_previewLabel->setColorSamplingActive(on);
            if (m_hslTargetStatus) {
                m_hslTargetStatus->setText(on
                    ? tr("Click color in image")
                    : tr("Tool inactive"));
            }
        });
    }

    // ---- Section header: CURVES -------------------------------------------
    // Tab-style channel selector over an interactive curve editor. Four
    // buttons switch which CurvePoints the editor is mutating; one widget
    // handles all four channels.
    auto* curvesHeader = new QLabel(tr("CURVES"), panel);
    m_curvesSection = curvesHeader;
    curvesHeader->setFont(hf);
    curvesHeader->setStyleSheet("color: #9A9AA0; padding-top: 4px;");
    col->addWidget(curvesHeader);

    // Channel tabs: four-button row. Same styling pattern as HSL channel
    // selector — per-channel tint, checked-state border, manual toggle.
    auto* curveTabs = new QWidget(panel);
    auto* tabRow = new QHBoxLayout(curveTabs);
    tabRow->setContentsMargins(0, 0, 0, 0);
    tabRow->setSpacing(4);

    const QString tabSheet = QStringLiteral(
        "QPushButton { color: #d0d0d4; border: 1px solid #3a3a3f;"
        "              padding: 4px 6px; border-radius: 3px; }"
        "QPushButton:hover   { border-color: #6a6a70; }"
        "QPushButton:checked { border: 1px solid #d0d0d4;"
        "                      font-weight: bold; }"
    );

    for (int i = 0; i < kCurveChannelCount; ++i) {
        auto* btn = new QPushButton(tr(kCurveChannels[i].displayName), curveTabs);
        btn->setCheckable(true);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setStyleSheet(tabSheet +
            QString("QPushButton { background-color: %1; }")
                .arg(QLatin1String(kCurveChannels[i].tintHex)));
        m_curveChannelButtons[i] = btn;
        tabRow->addWidget(btn);

        connect(btn, &QPushButton::clicked, this, [this, i]() {
            selectCurveChannel(i);
        });
    }
    col->addWidget(curveTabs);

    // ---- Curve editor widget ----------------------------------------------
    m_curveEditor = new CurveEditorWidget(panel);
    col->addWidget(m_curveEditor);

    // Every curve edit writes through the pointer the editor holds to the
    // matching CurvePoints in m_look, then re-kicks the preview debounce.
    // The writes happen inside the editor's mouse handlers before the
    // signal fires, so by the time we reach here, m_look already reflects
    // the change — we just need to re-render.
    connect(m_curveEditor, &CurveEditorWidget::curveChanged, this, [this]() {
        m_debounce->start();
    });

    // Undo boundary: one snapshot per curve edit operation (drag / add /
    // delete), regardless of how many curveChanged pulses fire during it.
    // m_curveDragUndoCaptured guards against re-entry if editStarted fires
    // twice (shouldn't happen, but defensively).
    connect(m_curveEditor, &CurveEditorWidget::editStarted, this, [this]() {
        if (m_curveDragUndoCaptured) return;
        m_nextHistoryLabel = tr("Curve changed");
        pushUndoSnapshot();
        m_curveDragUndoCaptured = true;
    });
    connect(m_curveEditor, &CurveEditorWidget::editFinished, this, [this]() {
        m_curveDragUndoCaptured = false;
    });

    // ---- Section header: COLOR GRADING ------------------------------------
    // LUT-based cinematic color grading. The engine's ColorGrading stage
    // already supports loading a .cube LUT and blending it with an opacity
    // multiplier; this section just exposes those two parameters in the
    // UI plus Load/Clear buttons.
    auto* gradingHeader = new QLabel(tr("COLOR GRADING"), panel);
    m_gradingSection = gradingHeader;
    gradingHeader->setFont(hf);
    gradingHeader->setStyleSheet("color: #9A9AA0; padding-top: 4px;");
    col->addWidget(gradingHeader);

    // LUT name + buttons row
    {
        m_lutNameLabel = new QLabel(tr("(none)"), panel);
        m_lutNameLabel->setStyleSheet("color: #8a8a90; font-style: italic;");
        m_lutNameLabel->setWordWrap(false);
        m_lutNameLabel->setToolTip(tr("Currently loaded LUT"));
        col->addWidget(m_lutNameLabel);

        auto* btnRow = new QHBoxLayout();
        btnRow->setContentsMargins(0, 0, 0, 0);
        btnRow->setSpacing(4);

        m_lutLoadBtn = new QPushButton(tr("Load LUT…"), panel);
        m_lutLoadBtn->setCursor(Qt::PointingHandCursor);
        connect(m_lutLoadBtn, &QPushButton::clicked,
                this, &MainWindow::onLoadLut);
        btnRow->addWidget(m_lutLoadBtn);

        m_lutClearBtn = new QPushButton(tr("Clear"), panel);
        m_lutClearBtn->setCursor(Qt::PointingHandCursor);
        connect(m_lutClearBtn, &QPushButton::clicked,
                this, &MainWindow::onClearLut);
        btnRow->addWidget(m_lutClearBtn);

        col->addLayout(btnRow);
    }

    // Opacity slider (0..100 → 0..1.0). Reuses buildSliderRow for layout
    // consistency with the tone/color sliders.
    col->addWidget(buildSliderRow(tr("Opacity"),
                                  0, 100, 100,
                                  m_lutOpacitySlider, m_lutOpacityValue));

    // Wire the opacity slider to grading.lutOpacity. Same drag-undo +
    // debounce pattern as the tone/color sliders.
    connect(m_lutOpacitySlider, &QSlider::sliderPressed,
            this, &MainWindow::pushUndoSnapshot);
    connect(m_lutOpacitySlider, &QSlider::valueChanged, this, [this](int v) {
        m_look.grading.lutOpacity = static_cast<float>(v) / 100.0f;
        m_lutOpacityValue->setText(QString::number(v));
        m_debounce->start();
    });

    // LUT Enabled checkbox — master on/off that preserves the opacity
    // value. When unchecked, ColorGrading::apply skips the LUT branch
    // (driven by GradingParams::isIdentity's lutActive check).
    {
        m_lutEnabledCheck = new QCheckBox(tr("LUT enabled"), panel);
        m_lutEnabledCheck->setChecked(true);
        m_lutEnabledCheck->setEnabled(false);   // until a LUT is loaded
        m_lutEnabledCheck->setCursor(Qt::PointingHandCursor);
        connect(m_lutEnabledCheck, &QCheckBox::toggled, this, [this](bool on) {
            if (m_look.grading.lutEnabled == on) return;
            m_nextHistoryLabel = tr("LUT enabled changed");
            pushUndoSnapshot();
            m_look.grading.lutEnabled = on;
            refreshUndoRedoActions();
            markDirty();
            if (m_debounce) m_debounce->start();
        });
        col->addWidget(m_lutEnabledCheck);
    }

    // ---- 3-way color grading wheels ----------------------------------------
    // Four wheels (Shadows / Midtones / Highlights / Global) each with
    // hue/saturation/strength. Plus Balance and Blending sliders.
    //
    // Each wheel is built as a collapsible block — the header is a
    // toggle button that hides/shows the three sliders. Saves a lot of
    // vertical space when only one or two wheels are in active use.
    {
        // Sub-section divider so the wheels read as a group separate from
        // the LUT controls above.
        auto* divider = new QFrame(panel);
        divider->setFrameShape(QFrame::HLine);
        divider->setFrameShadow(QFrame::Sunken);
        divider->setStyleSheet("color: #2a2a2e;");
        col->addWidget(divider);

        auto* wheelsHeader = new QLabel(tr("3-WAY GRADING"), panel);
        wheelsHeader->setFont(hf);
        wheelsHeader->setStyleSheet("color: #9A9AA0; padding-top: 4px;");
        col->addWidget(wheelsHeader);

        // Build the four wheels. Order matches the Lightroom convention
        // (Shadows → Midtones → Highlights → Global).
        buildGradingWheel(panel, col, 0, tr("Shadows"));
        buildGradingWheel(panel, col, 1, tr("Midtones"));
        buildGradingWheel(panel, col, 2, tr("Highlights"));
        buildGradingWheel(panel, col, 3, tr("Global"));

        // Balance: -100..+100 (slider is 0..200 internally, mapped at use).
        // Center of the slider = 0 = no shift in tonal pivots.
        col->addWidget(buildSliderRow(tr("Balance"),
                                      -100, 100, 0,
                                      m_balanceSlider, m_balanceValue));
        connect(m_balanceSlider, &QSlider::sliderPressed,
                this, &MainWindow::pushUndoSnapshot);
        connect(m_balanceSlider, &QSlider::valueChanged, this, [this](int v) {
            m_look.grading.balance = static_cast<float>(v);
            m_balanceValue->setText(QString::number(v));
            m_debounce->start();
        });

        // Blending: 0..100, default 50 (medium softness).
        col->addWidget(buildSliderRow(tr("Blending"),
                                      0, 100, 50,
                                      m_blendingSlider, m_blendingValue));
        connect(m_blendingSlider, &QSlider::sliderPressed,
                this, &MainWindow::pushUndoSnapshot);
        connect(m_blendingSlider, &QSlider::valueChanged, this, [this](int v) {
            m_look.grading.blending = static_cast<float>(v);
            m_blendingValue->setText(QString::number(v));
            m_debounce->start();
        });

        // ---- Advanced grading (DaVinci-style) — collapsible -----------------
        // Lift / Gamma / Gain / Offset. V1 placeholders — UI present, data
        // round-trips, engine math is a follow-up.
        {
            auto* advHeader = new QToolButton(panel);
            advHeader->setText(QString::fromUtf8("▸ ") + tr("Advanced Grading"));
            advHeader->setToolButtonStyle(Qt::ToolButtonTextOnly);
            advHeader->setAutoRaise(true);
            advHeader->setCursor(Qt::PointingHandCursor);
            advHeader->setStyleSheet(
                "QToolButton { color: #b0b0b6; text-align: left; padding: 2px 4px; }"
                "QToolButton:hover { color: #d0d0d4; }");
            col->addWidget(advHeader);

            auto* advBox = new QWidget(panel);
            auto* advLay = new QVBoxLayout(advBox);
            advLay->setContentsMargins(8, 0, 0, 0);
            advLay->setSpacing(2);
            advBox->setVisible(false);   // collapsed by default

            connect(advHeader, &QToolButton::clicked, this, [advHeader, advBox]() {
                const bool wasVisible = advBox->isVisible();
                advBox->setVisible(!wasVisible);
                advHeader->setText((!wasVisible ? QString::fromUtf8("▾ ")
                                                : QString::fromUtf8("▸ "))
                                   + tr("Advanced Grading"));
            });

            advLay->addWidget(buildSliderRow(tr("Lift"),
                                             -100, 100, 0,
                                             m_liftSlider, m_liftValue));
            advLay->addWidget(buildSliderRow(tr("Gamma"),
                                             -100, 100, 0,
                                             m_gammaSlider, m_gammaValue));
            advLay->addWidget(buildSliderRow(tr("Gain"),
                                             -100, 100, 0,
                                             m_gainSlider, m_gainValue));
            advLay->addWidget(buildSliderRow(tr("Offset"),
                                             -100, 100, 0,
                                             m_offsetSlider, m_offsetValue));
            col->addWidget(advBox);

            // Wire each — same pattern: undo on press, write on change.
            auto wireAdv = [this](QSlider* slider, QLabel* lbl,
                                  float lps::GradingParams::* field) {
                connect(slider, &QSlider::sliderPressed,
                        this, &MainWindow::pushUndoSnapshot);
                connect(slider, &QSlider::valueChanged, this,
                        [this, lbl, field](int v) {
                    m_look.grading.*field = static_cast<float>(v);
                    if (lbl) lbl->setText(QString::number(v));
                    if (m_debounce) m_debounce->start();
                });
            };
            wireAdv(m_liftSlider,   m_liftValue,   &lps::GradingParams::lift);
            wireAdv(m_gammaSlider,  m_gammaValue,  &lps::GradingParams::gamma);
            wireAdv(m_gainSlider,   m_gainValue,   &lps::GradingParams::gain);
            wireAdv(m_offsetSlider, m_offsetValue, &lps::GradingParams::offset);
        }

        // ---- Filmic look — collapsible --------------------------------------
        // Filmic Contrast / Highlight Rolloff / Shadow Lift / Fade Blacks /
        // Color Separation. V1 placeholders.
        {
            auto* filmHeader = new QToolButton(panel);
            filmHeader->setText(QString::fromUtf8("▸ ") + tr("Filmic Look"));
            filmHeader->setToolButtonStyle(Qt::ToolButtonTextOnly);
            filmHeader->setAutoRaise(true);
            filmHeader->setCursor(Qt::PointingHandCursor);
            filmHeader->setStyleSheet(
                "QToolButton { color: #b0b0b6; text-align: left; padding: 2px 4px; }"
                "QToolButton:hover { color: #d0d0d4; }");
            col->addWidget(filmHeader);

            auto* filmBox = new QWidget(panel);
            auto* filmLay = new QVBoxLayout(filmBox);
            filmLay->setContentsMargins(8, 0, 0, 0);
            filmLay->setSpacing(2);
            filmBox->setVisible(false);

            connect(filmHeader, &QToolButton::clicked, this,
                    [filmHeader, filmBox]() {
                const bool wasVisible = filmBox->isVisible();
                filmBox->setVisible(!wasVisible);
                filmHeader->setText((!wasVisible ? QString::fromUtf8("▾ ")
                                                 : QString::fromUtf8("▸ "))
                                    + tr("Filmic Look"));
            });

            filmLay->addWidget(buildSliderRow(tr("Filmic Contrast"),
                                              -100, 100, 0,
                                              m_filmicContrastSlider,
                                              m_filmicContrastValue));
            filmLay->addWidget(buildSliderRow(tr("Highlight Rolloff"),
                                              -100, 100, 0,
                                              m_highlightRolloffSlider,
                                              m_highlightRolloffValue));
            filmLay->addWidget(buildSliderRow(tr("Shadow Lift"),
                                              -100, 100, 0,
                                              m_shadowLiftSlider,
                                              m_shadowLiftValue));
            filmLay->addWidget(buildSliderRow(tr("Fade Blacks"),
                                              -100, 100, 0,
                                              m_fadeBlacksSlider,
                                              m_fadeBlacksValue));
            filmLay->addWidget(buildSliderRow(tr("Color Separation"),
                                              -100, 100, 0,
                                              m_colorSeparationSlider,
                                              m_colorSeparationValue));
            col->addWidget(filmBox);

            auto wireFilm = [this](QSlider* slider, QLabel* lbl,
                                   float lps::GradingParams::* field) {
                connect(slider, &QSlider::sliderPressed,
                        this, &MainWindow::pushUndoSnapshot);
                connect(slider, &QSlider::valueChanged, this,
                        [this, lbl, field](int v) {
                    m_look.grading.*field = static_cast<float>(v);
                    if (lbl) lbl->setText(QString::number(v));
                    if (m_debounce) m_debounce->start();
                });
            };
            wireFilm(m_filmicContrastSlider,   m_filmicContrastValue,
                     &lps::GradingParams::filmicContrast);
            wireFilm(m_highlightRolloffSlider, m_highlightRolloffValue,
                     &lps::GradingParams::highlightRolloff);
            wireFilm(m_shadowLiftSlider,       m_shadowLiftValue,
                     &lps::GradingParams::shadowLift);
            wireFilm(m_fadeBlacksSlider,       m_fadeBlacksValue,
                     &lps::GradingParams::fadeBlacks);
            wireFilm(m_colorSeparationSlider,  m_colorSeparationValue,
                     &lps::GradingParams::colorSeparation);
        }
    }

    // ---- Section header: PRESETS ------------------------------------------
    // Save / Load buttons that route to the same slots as the File menu's
    // preset items — single source of truth for preset I/O behavior. The
    // small label below shows the most recently loaded preset filename,
    // for at-a-glance recall of "which preset is on this image right now."
    auto* presetsHeader = new QLabel(tr("PRESETS"), panel);
    presetsHeader->setFont(hf);
    presetsHeader->setStyleSheet("color: #9A9AA0; padding-top: 4px;");
    col->addWidget(presetsHeader);

    m_presetNameLabel = new QLabel(tr("(no preset loaded)"), panel);
    m_presetNameLabel->setStyleSheet("color: #8a8a90; font-style: italic;");
    m_presetNameLabel->setWordWrap(false);
    col->addWidget(m_presetNameLabel);

    {
        auto* presetRow = new QHBoxLayout();
        presetRow->setContentsMargins(0, 0, 0, 0);
        presetRow->setSpacing(4);

        auto* saveBtn = new QPushButton(tr("Save Preset"), panel);
        saveBtn->setCursor(Qt::PointingHandCursor);
        connect(saveBtn, &QPushButton::clicked,
                this, &MainWindow::onSavePreset);
        presetRow->addWidget(saveBtn);

        auto* loadBtn = new QPushButton(tr("Load Preset"), panel);
        loadBtn->setCursor(Qt::PointingHandCursor);
        connect(loadBtn, &QPushButton::clicked,
                this, &MainWindow::onLoadPreset);
        presetRow->addWidget(loadBtn);

        col->addLayout(presetRow);
    }

    // Initialize LUT widgets from the (default-empty) Look.
    refreshLutWidgets();

    // ---- Section header: TRANSFORM ---------------------------------------
    {
        auto* transformHeader = new QLabel(tr("TRANSFORM"), panel);
        m_transformSection = transformHeader;
        transformHeader->setFont(hf);
        transformHeader->setStyleSheet("color: #9A9AA0; padding-top: 4px;");
        col->addWidget(transformHeader);

        auto makeButton = [panel](const QString& text) {
            auto* btn = new QPushButton(text, panel);
            btn->setCursor(Qt::PointingHandCursor);
            return btn;
        };

        m_cropToolBtn = makeButton(tr("Crop Tool"));
        m_cropToolBtn->setCheckable(true);
        col->addWidget(m_cropToolBtn);

        auto* cropOptionsRow = new QWidget(panel);
        auto* cropOptionsLay = new QHBoxLayout(cropOptionsRow);
        cropOptionsLay->setContentsMargins(0, 0, 0, 0);
        cropOptionsLay->setSpacing(6);

        m_cropAspectCombo = new QComboBox(panel);
        m_cropAspectCombo->addItems(QStringList{
            tr("Free"),
            tr("Original"),
            tr("1:1"),
            tr("4:5"),
            tr("5:4"),
            tr("3:2"),
            tr("2:3"),
            tr("16:9"),
            tr("9:16"),
        });
        cropOptionsLay->addWidget(m_cropAspectCombo, 1);

        m_cropLockAspectCheck = new QCheckBox(tr("Lock"), panel);
        m_cropLockAspectCheck->setCursor(Qt::PointingHandCursor);
        cropOptionsLay->addWidget(m_cropLockAspectCheck);
        col->addWidget(cropOptionsRow);

        auto* resetCropBtn = makeButton(tr("Reset Crop"));
        col->addWidget(resetCropBtn);

        auto* rotateRow = new QWidget(panel);
        auto* rotateLay = new QHBoxLayout(rotateRow);
        rotateLay->setContentsMargins(0, 0, 0, 0);
        rotateLay->setSpacing(6);

        auto* rotateLeftBtn = makeButton(tr("Rotate Left"));
        auto* rotateRightBtn = makeButton(tr("Rotate Right"));
        rotateLay->addWidget(rotateLeftBtn);
        rotateLay->addWidget(rotateRightBtn);
        col->addWidget(rotateRow);

        auto* flipRow = new QWidget(panel);
        auto* flipLay = new QHBoxLayout(flipRow);
        flipLay->setContentsMargins(0, 0, 0, 0);
        flipLay->setSpacing(6);

        m_transformFlipHorizontalBtn = makeButton(tr("Flip H"));
        m_transformFlipHorizontalBtn->setCheckable(true);
        m_transformFlipVerticalBtn = makeButton(tr("Flip V"));
        m_transformFlipVerticalBtn->setCheckable(true);
        flipLay->addWidget(m_transformFlipHorizontalBtn);
        flipLay->addWidget(m_transformFlipVerticalBtn);
        col->addWidget(flipRow);

        col->addWidget(buildSliderRow(tr("Straighten"),
                                      -100, 100, 0,
                                      m_straightenSlider,
                                      m_straightenValue));

        auto* resetTransformBtn = makeButton(tr("Reset Transform"));
        col->addWidget(resetTransformBtn);

        connect(rotateLeftBtn, &QPushButton::clicked,
                this, &MainWindow::onRotateLeft);
        connect(rotateRightBtn, &QPushButton::clicked,
                this, &MainWindow::onRotateRight);
        connect(m_transformFlipHorizontalBtn, &QPushButton::clicked,
                this, &MainWindow::onFlipHorizontal);
        connect(m_transformFlipVerticalBtn, &QPushButton::clicked,
                this, &MainWindow::onFlipVertical);
        connect(resetTransformBtn, &QPushButton::clicked,
                this, &MainWindow::onResetTransform);
        connect(m_cropToolBtn, &QPushButton::toggled,
                this, [this](bool on) {
            if (!m_previewLabel) return;
            m_previewLabel->setCropRect(m_look.transform.cropRect);
            updateCropAspectConstraint();
            m_previewLabel->setCropOverlayActive(on);
        });
        connect(m_cropAspectCombo,
                qOverload<int>(&QComboBox::currentIndexChanged),
                this, [this](int) { updateCropAspectConstraint(); });
        connect(m_cropLockAspectCheck, &QCheckBox::toggled,
                this, [this](bool) { updateCropAspectConstraint(); });
        connect(resetCropBtn, &QPushButton::clicked,
                this, [this]() {
            const QRectF identity(0.0, 0.0, 1.0, 1.0);
            if (m_look.transform.cropRect == identity) return;
            m_nextHistoryLabel = tr("Crop changed");
            pushUndoSnapshot();
            m_look.transform.cropRect = identity;
            if (m_previewLabel)
                m_previewLabel->setCropRect(m_look.transform.cropRect);
            refreshTransformWidgets();
            if (m_debounce) m_debounce->start();
        });

        connect(m_straightenSlider, &QSlider::sliderPressed,
                this, &MainWindow::pushUndoSnapshot);
        connect(m_straightenSlider, &QSlider::valueChanged,
                this, [this](int v) {
            const float angle = static_cast<float>(v) / 10.0f;
            m_look.transform.straightenAngle = angle;
            if (m_straightenValue)
                m_straightenValue->setText(QString::number(angle, 'f', 1));
            if (m_debounce) m_debounce->start();
        });

        refreshTransformWidgets();
    }

    // ---- Section header: HDR TONE MAPPING --------------------------------
    {
        auto* hdrHeader = new QLabel(tr("HDR"), panel);
        hdrHeader->setFont(hf);
        hdrHeader->setStyleSheet("color: #9A9AA0; padding-top: 4px;");
        col->addWidget(hdrHeader);

        m_hdrEnabledCheck = new QCheckBox(tr("Enable HDR Tone Mapping"), panel);
        m_hdrEnabledCheck->setCursor(Qt::PointingHandCursor);
        connect(m_hdrEnabledCheck, &QCheckBox::toggled, this, [this](bool on) {
            if (m_look.hdr.enabled == on) return;
            m_nextHistoryLabel = tr("HDR tone mapping changed");
            pushUndoSnapshot();
            m_look.hdr.enabled = on;
            refreshHdrWidgets();
            refreshUndoRedoActions();
            markDirty();
            if (m_debounce) m_debounce->start();
        });
        col->addWidget(m_hdrEnabledCheck);

        col->addWidget(buildSliderRow(tr("Exposure Bias"),
                                      -500, 500, 0,
                                      m_hdrExposureBiasSlider,
                                      m_hdrExposureBiasValue));
        col->addWidget(buildSliderRow(tr("Highlight Compression"),
                                      0, 100, 50,
                                      m_hdrHighlightCompressionSlider,
                                      m_hdrHighlightCompressionValue));
        col->addWidget(buildSliderRow(tr("Shoulder Strength"),
                                      0, 100, 50,
                                      m_hdrShoulderStrengthSlider,
                                      m_hdrShoulderStrengthValue));
        col->addWidget(buildSliderRow(tr("Midtone Pivot"),
                                      5, 100, 18,
                                      m_hdrMidtonePivotSlider,
                                      m_hdrMidtonePivotValue));
        col->addWidget(buildSliderRow(tr("Saturation Preserve"),
                                      0, 100, 85,
                                      m_hdrSaturationPreserveSlider,
                                      m_hdrSaturationPreserveValue));

        auto wireHdr = [this](QSlider* slider, QLabel* lbl,
                              float lps::HDRParams::* field,
                              float scale, int decimals) {
            connect(slider, &QSlider::sliderPressed,
                    this, &MainWindow::pushUndoSnapshot);
            connect(slider, &QSlider::valueChanged, this,
                    [this, lbl, field, scale, decimals](int v) {
                const float value = static_cast<float>(v) / scale;
                m_look.hdr.*field = value;
                if (lbl) lbl->setText(QString::number(value, 'f', decimals));
                if (m_debounce) m_debounce->start();
            });
        };

        wireHdr(m_hdrExposureBiasSlider, m_hdrExposureBiasValue,
                &lps::HDRParams::exposureBias, 100.0f, 2);
        wireHdr(m_hdrHighlightCompressionSlider, m_hdrHighlightCompressionValue,
                &lps::HDRParams::highlightCompression, 1.0f, 0);
        wireHdr(m_hdrShoulderStrengthSlider, m_hdrShoulderStrengthValue,
                &lps::HDRParams::shoulderStrength, 1.0f, 0);
        wireHdr(m_hdrMidtonePivotSlider, m_hdrMidtonePivotValue,
                &lps::HDRParams::midtonePivot, 100.0f, 2);
        wireHdr(m_hdrSaturationPreserveSlider, m_hdrSaturationPreserveValue,
                &lps::HDRParams::saturationPreserve, 1.0f, 0);

        refreshHdrWidgets();
    }

    // ---- Section header: LENS CORRECTION ---------------------------------
    // Master enable + per-control sliders. Vignetting is engine-active
    // in V1; distortion / CA / fringe are placeholders (UI persists,
    // engine ignores them — they round-trip through save/load).
    {
        auto* lensHeader = new QLabel(tr("LENS CORRECTION"), panel);
        m_lensSection = lensHeader;
        lensHeader->setFont(hf);
        lensHeader->setStyleSheet("color: #9A9AA0; padding-top: 4px;");
        col->addWidget(lensHeader);

        m_lensEnabledCheck = new QCheckBox(tr("Enable Lens Corrections"), panel);
        m_lensEnabledCheck->setCursor(Qt::PointingHandCursor);
        connect(m_lensEnabledCheck, &QCheckBox::toggled, this, [this](bool on) {
            if (m_look.lens.enabled == on) return;
            m_nextHistoryLabel = tr("Lens correction changed");
            pushUndoSnapshot();
            m_look.lens.enabled = on;
            refreshLensWidgets();
            refreshUndoRedoActions();
            markDirty();
            if (m_debounce) m_debounce->start();
        });
        col->addWidget(m_lensEnabledCheck);

        m_lensRemoveCaCheck = new QCheckBox(tr("Remove Chromatic Aberration"), panel);
        m_lensRemoveCaCheck->setCursor(Qt::PointingHandCursor);
        connect(m_lensRemoveCaCheck, &QCheckBox::toggled, this, [this](bool on) {
            if (m_look.lens.removeChromaticAberration == on) return;
            m_nextHistoryLabel = tr("Lens correction changed");
            pushUndoSnapshot();
            m_look.lens.removeChromaticAberration = on;
            refreshUndoRedoActions();
            markDirty();
            // CA is a placeholder in V1 — toggling it doesn't change pixels,
            // but kicking the debounce keeps the dirty/undo machinery
            // consistent with other lens controls.
            if (m_debounce) m_debounce->start();
        });
        col->addWidget(m_lensRemoveCaCheck);

        col->addWidget(buildSliderRow(tr("Distortion"),
                                      -100, 100, 0,
                                      m_lensDistortionSlider,
                                      m_lensDistortionValue));
        col->addWidget(buildSliderRow(tr("Vignetting"),
                                      -100, 100, 0,
                                      m_lensVignettingSlider,
                                      m_lensVignettingValue));
        col->addWidget(buildSliderRow(tr("Purple Fringe"),
                                      0, 100, 0,
                                      m_lensPurpleFringeSlider,
                                      m_lensPurpleFringeValue));
        col->addWidget(buildSliderRow(tr("Green Fringe"),
                                      0, 100, 0,
                                      m_lensGreenFringeSlider,
                                      m_lensGreenFringeValue));

        // Wire each slider — same pattern as global tone sliders. One
        // undo snapshot per drag (sliderPressed), debounce on each
        // valueChanged.
        auto wireLens = [this](QSlider* slider, QLabel* lbl,
                               float lps::LensParams::* field) {
            connect(slider, &QSlider::sliderPressed,
                    this, &MainWindow::pushUndoSnapshot);
            connect(slider, &QSlider::valueChanged, this,
                    [this, lbl, field](int v) {
                m_look.lens.*field = static_cast<float>(v);
                if (lbl) lbl->setText(QString::number(v));
                if (m_debounce) m_debounce->start();
            });
        };
        wireLens(m_lensDistortionSlider,   m_lensDistortionValue,
                 &lps::LensParams::distortion);
        wireLens(m_lensVignettingSlider,   m_lensVignettingValue,
                 &lps::LensParams::vignetting);
        wireLens(m_lensPurpleFringeSlider, m_lensPurpleFringeValue,
                 &lps::LensParams::purpleFringe);
        wireLens(m_lensGreenFringeSlider,  m_lensGreenFringeValue,
                 &lps::LensParams::greenFringe);

        // Initial state — all sliders disabled until master enable is on.
        refreshLensWidgets();
    }

    // ---- Section header: DETAILS -----------------------------------------
    // Lightroom-style sharpening plus luminance/chroma noise reduction.
    {
        auto* detailsHeader = new QLabel(tr("DETAILS"), panel);
        m_detailsSection = detailsHeader;
        detailsHeader->setFont(hf);
        detailsHeader->setStyleSheet("color: #9A9AA0; padding-top: 4px;");
        col->addWidget(detailsHeader);

        auto* sharpHeader = new QLabel(tr("Sharpening"), panel);
        sharpHeader->setStyleSheet("color: #b0b0b6; padding-top: 2px;");
        col->addWidget(sharpHeader);

        col->addWidget(buildSliderRow(tr("Amount"),
                                      0, 150, 0,
                                      m_sharpeningAmountSlider,
                                      m_sharpeningAmountValue));
        col->addWidget(buildSliderRow(tr("Radius"),
                                      5, 30, 10,
                                      m_sharpeningRadiusSlider,
                                      m_sharpeningRadiusValue));
        col->addWidget(buildSliderRow(tr("Detail"),
                                      0, 100, 0,
                                      m_sharpeningDetailSlider,
                                      m_sharpeningDetailValue));
        col->addWidget(buildSliderRow(tr("Masking"),
                                      0, 100, 0,
                                      m_sharpeningMaskingSlider,
                                      m_sharpeningMaskingValue));

        auto* nrHeader = new QLabel(tr("Noise Reduction"), panel);
        nrHeader->setStyleSheet("color: #b0b0b6; padding-top: 2px;");
        col->addWidget(nrHeader);

        col->addWidget(buildSliderRow(tr("Luminance"),
                                      0, 100, 0,
                                      m_luminanceNrSlider,
                                      m_luminanceNrValue));
        col->addWidget(buildSliderRow(tr("Lum Detail"),
                                      0, 100, 0,
                                      m_luminanceDetailSlider,
                                      m_luminanceDetailValue));
        col->addWidget(buildSliderRow(tr("Color NR"),
                                      0, 100, 0,
                                      m_colorNrSlider,
                                      m_colorNrValue));
        col->addWidget(buildSliderRow(tr("Color Detail"),
                                      0, 100, 0,
                                      m_colorDetailSlider,
                                      m_colorDetailValue));

        auto wireDetails = [this](QSlider* slider, QLabel* lbl,
                                  float lps::DetailsParams::* field,
                                  float scale, int decimals) {
            connect(slider, &QSlider::sliderPressed,
                    this, &MainWindow::pushUndoSnapshot);
            connect(slider, &QSlider::valueChanged, this,
                    [this, lbl, field, scale, decimals](int v) {
                const float value = static_cast<float>(v) / scale;
                m_look.details.*field = value;
                if (lbl) lbl->setText(QString::number(value, 'f', decimals));
                if (m_debounce) m_debounce->start();
            });
        };

        wireDetails(m_sharpeningAmountSlider,  m_sharpeningAmountValue,
                    &lps::DetailsParams::sharpeningAmount, 1.0f, 0);
        wireDetails(m_sharpeningRadiusSlider,  m_sharpeningRadiusValue,
                    &lps::DetailsParams::sharpeningRadius, 10.0f, 1);
        wireDetails(m_sharpeningDetailSlider,  m_sharpeningDetailValue,
                    &lps::DetailsParams::sharpeningDetail, 1.0f, 0);
        wireDetails(m_sharpeningMaskingSlider, m_sharpeningMaskingValue,
                    &lps::DetailsParams::sharpeningMasking, 1.0f, 0);
        wireDetails(m_luminanceNrSlider,       m_luminanceNrValue,
                    &lps::DetailsParams::luminanceNR, 1.0f, 0);
        wireDetails(m_luminanceDetailSlider,   m_luminanceDetailValue,
                    &lps::DetailsParams::luminanceDetail, 1.0f, 0);
        wireDetails(m_colorNrSlider,           m_colorNrValue,
                    &lps::DetailsParams::colorNR, 1.0f, 0);
        wireDetails(m_colorDetailSlider,       m_colorDetailValue,
                    &lps::DetailsParams::colorDetail, 1.0f, 0);

        refreshDetailsWidgets();
    }

    // ---- Section header: MASKS --------------------------------------------
    // Lightroom-style local adjustments: linear gradient, radial gradient,
    // and real brush masks. Each mask has six adjustment sliders that
    // operate only where the mask weight > 0 (LocalAdjustmentEngine).
    //
    // Layout: three "Add" buttons → list of masks (with per-row checkbox)
    //       → delete button → six sliders for the selected mask.
    {
        auto* maskHeader = new QLabel(tr("MASKS"), panel);
        m_masksSection = maskHeader;
        maskHeader->setFont(hf);
        maskHeader->setStyleSheet("color: #9A9AA0; padding-top: 4px;");
        col->addWidget(maskHeader);

        // Add-mask button row.
        auto* addRow = new QHBoxLayout();
        addRow->setContentsMargins(0, 0, 0, 0);
        addRow->setSpacing(4);
        m_maskAddLinearBtn = new QPushButton(tr("+ Linear"), panel);
        m_maskAddLinearBtn->setCursor(Qt::PointingHandCursor);
        m_maskAddLinearBtn->setToolTip(tr("Add a linear gradient mask"));
        connect(m_maskAddLinearBtn, &QPushButton::clicked,
                this, &MainWindow::onAddLinearMask);
        addRow->addWidget(m_maskAddLinearBtn);

        m_maskAddRadialBtn = new QPushButton(tr("+ Radial"), panel);
        m_maskAddRadialBtn->setCursor(Qt::PointingHandCursor);
        m_maskAddRadialBtn->setToolTip(tr("Add a radial gradient mask"));
        connect(m_maskAddRadialBtn, &QPushButton::clicked,
                this, &MainWindow::onAddRadialMask);
        addRow->addWidget(m_maskAddRadialBtn);

        m_maskAddBrushBtn = new QPushButton(tr("+ Brush"), panel);
        m_maskAddBrushBtn->setCursor(Qt::PointingHandCursor);
        m_maskAddBrushBtn->setToolTip(tr("Add a brush mask"));
        connect(m_maskAddBrushBtn, &QPushButton::clicked,
                this, &MainWindow::onAddBrushMask);
        addRow->addWidget(m_maskAddBrushBtn);
        col->addLayout(addRow);

        // Mask list. Per-row checkbox via Qt::ItemIsUserCheckable. Selection
        // drives which mask the sliders below edit. List takes a moderate
        // fixed height to keep the rest of the panel reachable.
        m_maskList = new QListWidget(panel);
        m_maskList->setSelectionMode(QAbstractItemView::SingleSelection);
        m_maskList->setMinimumHeight(80);
        m_maskList->setMaximumHeight(140);
        m_maskList->setStyleSheet(
            "QListWidget { background-color: #111318;"
            "              border: 1px solid #2A2D35;"
            "              border-radius: 8px; padding: 4px; }"
            "QListWidget::item { padding: 5px 6px; border-radius: 5px; }"
            "QListWidget::item:selected { background: rgba(204, 255, 0, 36);"
            "                              color: #FFFFFF; }");
        connect(m_maskList, &QListWidget::itemSelectionChanged,
                this, &MainWindow::onMaskListSelectionChanged);
        connect(m_maskList, &QListWidget::itemChanged,
                this, &MainWindow::onMaskItemChanged);
        col->addWidget(m_maskList);

        // Delete + status row.
        auto* delRow = new QHBoxLayout();
        delRow->setContentsMargins(0, 0, 0, 0);
        m_maskDeleteBtn = new QPushButton(tr("Delete"), panel);
        m_maskDeleteBtn->setCursor(Qt::PointingHandCursor);
        m_maskDeleteBtn->setEnabled(false);
        connect(m_maskDeleteBtn, &QPushButton::clicked,
                this, &MainWindow::onDeleteSelectedMask);
        delRow->addWidget(m_maskDeleteBtn);

        m_maskStatusLabel = new QLabel(tr("No mask selected"), panel);
        m_maskStatusLabel->setStyleSheet("color: #8a8a90; font-style: italic;");
        delRow->addWidget(m_maskStatusLabel, /*stretch=*/1);
        col->addLayout(delRow);

        // Per-mask sliders. Same six fields as in LocalAdjustment. Disabled
        // when no mask is selected.
        col->addWidget(buildSliderRow(tr("Exposure"),
                                      -500, +500, 0,
                                      m_maskExposureSlider, m_maskExposureValue));
        col->addWidget(buildSliderRow(tr("Brightness"),
                                      -100, +100, 0,
                                      m_maskBrightnessSlider, m_maskBrightnessValue));
        col->addWidget(buildSliderRow(tr("Contrast"),
                                      -100, +100, 0,
                                      m_maskContrastSlider, m_maskContrastValue));
        col->addWidget(buildSliderRow(tr("Saturation"),
                                      -100, +100, 0,
                                      m_maskSaturationSlider, m_maskSaturationValue));
        col->addWidget(buildSliderRow(tr("Temperature"),
                                      -100, +100, 0,
                                      m_maskTemperatureSlider, m_maskTemperatureValue));
        col->addWidget(buildSliderRow(tr("Tint"),
                                      -100, +100, 0,
                                      m_maskTintSlider, m_maskTintValue));

        // Wire mask sliders. Each slider:
        //  - sliderPressed → pushUndoSnapshot (one snapshot per drag)
        //  - valueChanged → write into the selected mask, kick debounce
        // Mapping: exposure slider is in 1/100 stops (range -500..+500 = ±5
        // stops, matching the global Exposure slider). Others are -100..+100.
        auto wireMaskSlider = [this](QSlider* slider, QLabel* valueLabel,
                                     float lps::LocalAdjustment::* field,
                                     bool isExposureScale = false) {
            connect(slider, &QSlider::sliderPressed,
                    this, &MainWindow::pushUndoSnapshot);
            connect(slider, &QSlider::valueChanged, this,
                    [this, valueLabel, field, isExposureScale](int v) {
                if (m_selectedMaskIndex < 0 ||
                    m_selectedMaskIndex >= static_cast<int>(m_look.localAdjustments.size())) {
                    return;   // no selection — slider should be disabled,
                              // but defensive against race conditions
                }
                const float value = isExposureScale
                    ? (static_cast<float>(v) / 100.0f)
                    : static_cast<float>(v);
                m_look.localAdjustments[m_selectedMaskIndex].*field = value;
                if (valueLabel) valueLabel->setText(QString::number(v));
                if (m_debounce) m_debounce->start();
            });
        };
        wireMaskSlider(m_maskExposureSlider,    m_maskExposureValue,
                       &lps::LocalAdjustment::exposure, /*exposure scale*/ true);
        wireMaskSlider(m_maskBrightnessSlider,  m_maskBrightnessValue,
                       &lps::LocalAdjustment::brightness);
        wireMaskSlider(m_maskContrastSlider,    m_maskContrastValue,
                       &lps::LocalAdjustment::contrast);
        wireMaskSlider(m_maskSaturationSlider,  m_maskSaturationValue,
                       &lps::LocalAdjustment::saturation);
        wireMaskSlider(m_maskTemperatureSlider, m_maskTemperatureValue,
                       &lps::LocalAdjustment::temperature);
        wireMaskSlider(m_maskTintSlider,        m_maskTintValue,
                       &lps::LocalAdjustment::tint);

        // ---- Geometry / structural mask controls ----------------------------
        // These edit WHERE the mask hits — name, enable-via-checkbox-row-above,
        // invert, feather (geometry softness), density (overall strength),
        // flow (brush painting rate), reset geometry button.
        {
            auto* row = new QHBoxLayout();
            row->setContentsMargins(0, 4, 0, 0);
            auto* lbl = new QLabel(tr("Name"), panel);
            lbl->setMinimumWidth(60);
            row->addWidget(lbl);
            m_maskNameEdit = new QLineEdit(panel);
            m_maskNameEdit->setPlaceholderText(tr("Mask name"));
            // Commit on editing-finished (focus loss / Enter) — mid-typing
            // shouldn't push undo entries per character. One snapshot
            // before the edit, the post-commit value gets stored.
            connect(m_maskNameEdit, &QLineEdit::editingFinished, this, [this]() {
                if (m_selectedMaskIndex < 0 ||
                    m_selectedMaskIndex >= static_cast<int>(m_look.localAdjustments.size())) {
                    return;
                }
                auto& mask = m_look.localAdjustments[m_selectedMaskIndex];
                const QString newName = m_maskNameEdit->text();
                if (mask.name == newName) return;
                m_nextHistoryLabel = tr("Mask renamed");
                pushUndoSnapshot();
                mask.name = newName;
                refreshUndoRedoActions();
                refreshMaskWidgets();   // updates the list-row label
                markDirty();
            });
            row->addWidget(m_maskNameEdit, /*stretch=*/1);
            col->addLayout(row);
        }

        m_maskInvertCheck = new QCheckBox(tr("Invert mask"), panel);
        m_maskInvertCheck->setCursor(Qt::PointingHandCursor);
        m_maskInvertCheck->setEnabled(false);
        connect(m_maskInvertCheck, &QCheckBox::toggled, this, [this](bool on) {
            if (m_selectedMaskIndex < 0 ||
                m_selectedMaskIndex >= static_cast<int>(m_look.localAdjustments.size())) {
                return;
            }
            auto& mask = m_look.localAdjustments[m_selectedMaskIndex];
            if (mask.invert == on) return;
            m_nextHistoryLabel = tr("Mask changed");
            pushUndoSnapshot();
            mask.invert = on;
            refreshUndoRedoActions();
            // Geometry-equivalent change: invalidate overlay cache + render.
            if (m_previewLabel) m_previewLabel->setActiveMask(&mask);   // forces cache rebuild
            markDirty();
            if (m_debounce) m_debounce->start();
        });
        col->addWidget(m_maskInvertCheck);

        // Feather / Density / Flow sliders. Feather is shared with
        // LocalAdjustment::feather (engine reads it). Density scales overall
        // mask strength. Flow controls brush stroke buildup.
        col->addWidget(buildSliderRow(tr("Feather"),
                                      0, 100, 50,
                                      m_maskFeatherSlider, m_maskFeatherValue));
        col->addWidget(buildSliderRow(tr("Density"),
                                      0, 100, 100,
                                      m_maskDensitySlider, m_maskDensityValue));
        col->addWidget(buildSliderRow(tr("Flow"),
                                      0, 100, 100,
                                      m_maskFlowSlider, m_maskFlowValue));
        col->addWidget(buildSliderRow(tr("Brush Size"),
                                      1, 200, 80,
                                      m_maskBrushSizeSlider, m_maskBrushSizeValue));

        auto wireMaskGeoSlider = [this](QSlider* slider, QLabel* lbl,
                                         float lps::LocalAdjustment::* field,
                                         float scaleDiv) {
            connect(slider, &QSlider::sliderPressed,
                    this, &MainWindow::pushUndoSnapshot);
            connect(slider, &QSlider::valueChanged, this,
                    [this, lbl, field, scaleDiv](int v) {
                if (m_selectedMaskIndex < 0 ||
                    m_selectedMaskIndex >= static_cast<int>(m_look.localAdjustments.size())) {
                    return;
                }
                auto& mask = m_look.localAdjustments[m_selectedMaskIndex];
                mask.*field = static_cast<float>(v) / scaleDiv;
                if (lbl) lbl->setText(QString::number(v));
                // Geometry-changed: invalidate the overlay cache.
                if (m_previewLabel) m_previewLabel->setActiveMask(&mask);
                if (m_debounce) m_debounce->start();
            });
        };
        wireMaskGeoSlider(m_maskFeatherSlider, m_maskFeatherValue,
                          &lps::LocalAdjustment::feather, 100.0f);
        wireMaskGeoSlider(m_maskDensitySlider, m_maskDensityValue,
                          &lps::LocalAdjustment::density, 100.0f);
        wireMaskGeoSlider(m_maskFlowSlider, m_maskFlowValue,
                          &lps::LocalAdjustment::flow, 100.0f);

        connect(m_maskBrushSizeSlider, &QSlider::sliderPressed,
                this, &MainWindow::pushUndoSnapshot);
        connect(m_maskBrushSizeSlider, &QSlider::valueChanged,
                this, [this](int v) {
            if (m_selectedMaskIndex < 0 ||
                m_selectedMaskIndex >= static_cast<int>(m_look.localAdjustments.size())) {
                return;
            }
            auto& mask = m_look.localAdjustments[m_selectedMaskIndex];
            if (mask.type != lps::MaskType::Brush) return;
            mask.brushSize = static_cast<float>(v) / 1000.0f;
            if (m_maskBrushSizeValue)
                m_maskBrushSizeValue->setText(QString::number(v));
            if (m_previewLabel) m_previewLabel->setActiveMask(&mask);
            markDirty();
        });

        m_maskBrushEraseCheck = new QCheckBox(tr("Erase brush"), panel);
        m_maskBrushEraseCheck->setCursor(Qt::PointingHandCursor);
        m_maskBrushEraseCheck->setEnabled(false);
        connect(m_maskBrushEraseCheck, &QCheckBox::toggled,
                this, [this](bool on) {
            if (m_selectedMaskIndex < 0 ||
                m_selectedMaskIndex >= static_cast<int>(m_look.localAdjustments.size())) {
                return;
            }
            auto& mask = m_look.localAdjustments[m_selectedMaskIndex];
            if (mask.type != lps::MaskType::Brush || mask.brushEraseMode == on) return;
            m_nextHistoryLabel = tr("Brush erase mode changed");
            pushUndoSnapshot();
            mask.brushEraseMode = on;
            if (m_previewLabel) m_previewLabel->setActiveMask(&mask);
            refreshUndoRedoActions();
            markDirty();
        });
        col->addWidget(m_maskBrushEraseCheck);

        m_maskResetBrushBtn = new QPushButton(tr("Reset Brush Mask"), panel);
        m_maskResetBrushBtn->setCursor(Qt::PointingHandCursor);
        m_maskResetBrushBtn->setEnabled(false);
        connect(m_maskResetBrushBtn, &QPushButton::clicked,
                this, &MainWindow::onResetBrushMask);
        col->addWidget(m_maskResetBrushBtn);

        m_maskResetGeoBtn = new QPushButton(tr("Reset Mask Geometry"), panel);
        m_maskResetGeoBtn->setCursor(Qt::PointingHandCursor);
        m_maskResetGeoBtn->setEnabled(false);
        connect(m_maskResetGeoBtn, &QPushButton::clicked,
                this, &MainWindow::onResetMaskGeometry);
        col->addWidget(m_maskResetGeoBtn);

        // ---- Overlay controls -----------------------------------------------
        // Show/hide overlay, opacity slider, view-mode dropdown. These
        // drive PreviewWidget directly — no Look state involved (overlay
        // is a UI-only preference, not part of the rendered image).
        m_maskShowOverlayCheck = new QCheckBox(tr("Show Mask Overlay"), panel);
        m_maskShowOverlayCheck->setChecked(true);
        m_maskShowOverlayCheck->setCursor(Qt::PointingHandCursor);
        connect(m_maskShowOverlayCheck, &QCheckBox::toggled, this, [this](bool on) {
            if (m_previewLabel) m_previewLabel->setShowMaskOverlay(on);
        });
        col->addWidget(m_maskShowOverlayCheck);

        col->addWidget(buildSliderRow(tr("Overlay Opacity"),
                                      0, 100, 35,
                                      m_maskOverlayOpacitySlider,
                                      m_maskOverlayOpacityValue));
        connect(m_maskOverlayOpacitySlider, &QSlider::valueChanged,
                this, [this](int v) {
            if (m_maskOverlayOpacityValue)
                m_maskOverlayOpacityValue->setText(QString::number(v));
            if (m_previewLabel)
                m_previewLabel->setMaskOverlayOpacity(static_cast<float>(v) / 100.0f);
        });

        {
            auto* row = new QHBoxLayout();
            row->setContentsMargins(0, 0, 0, 0);
            auto* lbl = new QLabel(tr("View Mode"), panel);
            lbl->setMinimumWidth(80);
            row->addWidget(lbl);
            m_maskViewModeCombo = new QComboBox(panel);
            m_maskViewModeCombo->addItem(tr("Overlay"));
            m_maskViewModeCombo->addItem(tr("Black & White"));
            m_maskViewModeCombo->addItem(tr("Marching Ants"));
            m_maskViewModeCombo->addItem(tr("Off"));
            connect(m_maskViewModeCombo,
                    QOverload<int>::of(&QComboBox::currentIndexChanged),
                    this, [this](int idx) {
                if (!m_previewLabel) return;
                m_previewLabel->setMaskViewMode(
                    static_cast<PreviewWidget::MaskViewMode>(idx));
                // BlackAndWhite affects the cached overlay's color, so
                // we need a cache rebuild. The setActiveMask path forces
                // that rebuild.
                if (m_selectedMaskIndex >= 0 &&
                    m_selectedMaskIndex < static_cast<int>(m_look.localAdjustments.size())) {
                    m_previewLabel->setActiveMask(
                        &m_look.localAdjustments[m_selectedMaskIndex]);
                }
            });
            row->addWidget(m_maskViewModeCombo, /*stretch=*/1);
            col->addLayout(row);
        }

        // Initial state — no masks, sliders disabled.
        refreshMaskWidgets();
    }

    // ---- Section header: LAYERS -------------------------------------------
    // Stackable adjustment layers (Photoshop/Lightroom-style). V1 is data
    // + UI plumbing only — the pipeline doesn't yet composite layers on
    // top of the base Look. The list, opacity, and blend mode all round-
    // trip through save/load and undo/redo so projects authored now will
    // pick up rendering once the compositor lands.
    {
        auto* layerHeader = new QLabel(tr("LAYERS"), panel);
        m_layersSection = layerHeader;
        layerHeader->setFont(hf);
        layerHeader->setStyleSheet("color: #9A9AA0; padding-top: 4px;");
        col->addWidget(layerHeader);

        // Add / Duplicate / Delete row.
        auto* btnRow = new QHBoxLayout();
        btnRow->setContentsMargins(0, 0, 0, 0);
        btnRow->setSpacing(4);
        m_layerAddBtn = new QPushButton(tr("+ Layer"), panel);
        m_layerAddBtn->setCursor(Qt::PointingHandCursor);
        m_layerAddBtn->setToolTip(tr("Add a new adjustment layer"));
        connect(m_layerAddBtn, &QPushButton::clicked,
                this, &MainWindow::onAddLayer);
        btnRow->addWidget(m_layerAddBtn);

        m_layerDuplicateBtn = new QPushButton(tr("Duplicate"), panel);
        m_layerDuplicateBtn->setCursor(Qt::PointingHandCursor);
        m_layerDuplicateBtn->setToolTip(tr("Duplicate the selected layer"));
        m_layerDuplicateBtn->setEnabled(false);
        connect(m_layerDuplicateBtn, &QPushButton::clicked,
                this, &MainWindow::onDuplicateLayer);
        btnRow->addWidget(m_layerDuplicateBtn);

        m_layerDeleteBtn = new QPushButton(tr("Delete"), panel);
        m_layerDeleteBtn->setCursor(Qt::PointingHandCursor);
        m_layerDeleteBtn->setEnabled(false);
        connect(m_layerDeleteBtn, &QPushButton::clicked,
                this, &MainWindow::onDeleteSelectedLayer);
        btnRow->addWidget(m_layerDeleteBtn);
        col->addLayout(btnRow);

        // Layer list — same widget pattern as masks. Per-row checkbox
        // for enabled-state, selection drives the opacity/blend controls.
        m_layerList = new QListWidget(panel);
        m_layerList->setSelectionMode(QAbstractItemView::SingleSelection);
        m_layerList->setMinimumHeight(80);
        m_layerList->setMaximumHeight(140);
        m_layerList->setStyleSheet(
            "QListWidget { background-color: #111318;"
            "              border: 1px solid #2A2D35;"
            "              border-radius: 8px; padding: 4px; }"
            "QListWidget::item { padding: 5px 6px; border-radius: 5px; }"
            "QListWidget::item:selected { background: rgba(204, 255, 0, 36);"
            "                              color: #FFFFFF; }");
        connect(m_layerList, &QListWidget::itemSelectionChanged,
                this, &MainWindow::onLayerListSelectionChanged);
        connect(m_layerList, &QListWidget::itemChanged,
                this, &MainWindow::onLayerItemChanged);
        col->addWidget(m_layerList);

        // Status label.
        m_layerStatusLabel = new QLabel(tr("No layers"), panel);
        m_layerStatusLabel->setStyleSheet("color: #8a8a90; font-style: italic;");
        col->addWidget(m_layerStatusLabel);

        // Opacity slider — 0..100% mapped to layer.opacity ∈ [0, 1].
        col->addWidget(buildSliderRow(tr("Opacity"),
                                      0, 100, 100,
                                      m_layerOpacitySlider, m_layerOpacityValue));
        connect(m_layerOpacitySlider, &QSlider::sliderPressed,
                this, &MainWindow::pushUndoSnapshot);
        connect(m_layerOpacitySlider, &QSlider::valueChanged,
                this, &MainWindow::onLayerOpacityChanged);

        // Blend mode dropdown. The integer index matches the BlendMode
        // enum value — keep this list in sync with the enum.
        auto* blendRow = new QHBoxLayout();
        blendRow->setContentsMargins(0, 0, 0, 0);
        auto* blendLabel = new QLabel(tr("Blend"), panel);
        blendLabel->setMinimumWidth(60);
        blendRow->addWidget(blendLabel);
        m_layerBlendModeCombo = new QComboBox(panel);
        m_layerBlendModeCombo->addItem(tr("Normal"));
        m_layerBlendModeCombo->addItem(tr("Multiply"));
        m_layerBlendModeCombo->addItem(tr("Screen"));
        m_layerBlendModeCombo->addItem(tr("Overlay"));
        m_layerBlendModeCombo->addItem(tr("Soft Light"));
        m_layerBlendModeCombo->addItem(tr("Hard Light"));
        m_layerBlendModeCombo->addItem(tr("Color Dodge"));
        m_layerBlendModeCombo->addItem(tr("Color Burn"));
        m_layerBlendModeCombo->addItem(tr("Darken"));
        m_layerBlendModeCombo->addItem(tr("Lighten"));
        m_layerBlendModeCombo->addItem(tr("Difference"));
        m_layerBlendModeCombo->setEnabled(false);
        connect(m_layerBlendModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &MainWindow::onLayerBlendModeChanged);
        blendRow->addWidget(m_layerBlendModeCombo, /*stretch=*/1);
        col->addLayout(blendRow);

        // Initial state — no layers, controls disabled.
        refreshLayerWidgets();
    }

    col->addStretch(1);

    // ---- Wire each slider → Look field + debounce --------------------------
    // Captures are to member pointers, so they see live lookups through `this`.
    //
    // Undo boundaries: sliderPressed (drag start) pushes a snapshot so
    // undo walks back to the pre-drag state regardless of how many
    // valueChanged ticks fire during the drag. One gesture = one undo step.
    auto hookSliderUndo = [this](QSlider* slider) {
        connect(slider, &QSlider::sliderPressed,
                this, &MainWindow::pushUndoSnapshot);
    };
    hookSliderUndo(m_exposureSlider);
    hookSliderUndo(m_contrastSlider);
    hookSliderUndo(m_highlightsSlider);
    hookSliderUndo(m_shadowsSlider);
    hookSliderUndo(m_whitesSlider);
    hookSliderUndo(m_blacksSlider);
    hookSliderUndo(m_brightnessSlider);
    hookSliderUndo(m_temperatureSlider);
    hookSliderUndo(m_tintSlider);
    hookSliderUndo(m_vibranceSlider);
    hookSliderUndo(m_saturationSlider);
    hookSliderUndo(m_hslHueSlider);
    hookSliderUndo(m_hslSaturationSlider);
    hookSliderUndo(m_hslLuminanceSlider);

    connect(m_exposureSlider, &QSlider::valueChanged, this, [this](int v) {
        m_look.tone.exposure = static_cast<float>(v) / kExposureScale;
        m_exposureValue->setText(QString::number(m_look.tone.exposure, 'f', 2));
        m_debounce->start();
    });

    connect(m_contrastSlider, &QSlider::valueChanged, this, [this](int v) {
        m_look.tone.contrast = static_cast<float>(v);
        m_contrastValue->setText(QString::number(v));
        m_debounce->start();
    });

    connect(m_highlightsSlider, &QSlider::valueChanged, this, [this](int v) {
        m_look.tone.highlights = static_cast<float>(v);
        m_highlightsValue->setText(QString::number(v));
        m_debounce->start();
    });

    connect(m_shadowsSlider, &QSlider::valueChanged, this, [this](int v) {
        m_look.tone.shadows = static_cast<float>(v);
        m_shadowsValue->setText(QString::number(v));
        m_debounce->start();
    });

    connect(m_whitesSlider, &QSlider::valueChanged, this, [this](int v) {
        m_look.tone.whites = static_cast<float>(v);
        m_whitesValue->setText(QString::number(v));
        m_debounce->start();
    });

    connect(m_blacksSlider, &QSlider::valueChanged, this, [this](int v) {
        m_look.tone.blacks = static_cast<float>(v);
        m_blacksValue->setText(QString::number(v));
        m_debounce->start();
    });

    connect(m_brightnessSlider, &QSlider::valueChanged, this, [this](int v) {
        m_look.tone.brightness = static_cast<float>(v);
        m_brightnessValue->setText(QString::number(v));
        m_debounce->start();
    });

    // ---- COLOR slider wiring ----------------------------------------------
    // Same debounce/update pattern as the tone sliders: slider drag mutates
    // the Look field, updates the readout label, restarts the debounce
    // timer. Actual render dispatch happens when the timer fires.

    connect(m_temperatureSlider, &QSlider::valueChanged, this, [this](int v) {
        m_look.color.whiteBalance.temperature = static_cast<float>(v);
        m_temperatureValue->setText(QString::number(v));
        m_debounce->start();
    });

    connect(m_tintSlider, &QSlider::valueChanged, this, [this](int v) {
        m_look.color.whiteBalance.tint = static_cast<float>(v);
        m_tintValue->setText(QString::number(v));
        m_debounce->start();
    });

    connect(m_vibranceSlider, &QSlider::valueChanged, this, [this](int v) {
        m_look.color.vibrance = static_cast<float>(v);
        m_vibranceValue->setText(QString::number(v));
        m_debounce->start();
    });

    connect(m_saturationSlider, &QSlider::valueChanged, this, [this](int v) {
        m_look.color.saturation = static_cast<float>(v);
        m_saturationValue->setText(QString::number(v));
        m_debounce->start();
    });

    // ---- HSL slider wiring ------------------------------------------------
    // Three sliders, dispatched to whichever channel is currently selected.
    // The lambda reads m_selectedHslChannel at fire time (not at connect
    // time), so a single connect per slider handles all eight channels.
    //
    // When the user switches channels via a button click, selectHslChannel()
    // reprograms the sliders with QSignalBlocker — so this lambda only runs
    // from genuine user drags, never from the switch itself.
    //
    // Control order in kHslControls MUST match the slider declaration order
    // below (hue / saturation / luminance).
    auto wireHslSlider = [this](QSlider* slider, QLabel* value, int controlIndex) {
        connect(slider, &QSlider::valueChanged, this,
                [this, value, controlIndex](int v) {
            // Two pointer-to-member dereferences: first select the channel
            // sub-struct inside HSLParams, then select the field inside
            // that HSLChannel.
            (m_look.color.hsl.*kHslChannels[m_selectedHslChannel].field)
                .*kHslControls[controlIndex] = static_cast<float>(v);
            value->setText(QString::number(v));
            m_debounce->start();
        });
    };

    wireHslSlider(m_hslHueSlider,        m_hslHueValue,        0);
    wireHslSlider(m_hslSaturationSlider, m_hslSaturationValue, 1);
    wireHslSlider(m_hslLuminanceSlider,  m_hslLuminanceValue,  2);

    // ---- Curve editor initialization --------------------------------------
    // Point the editor at the default channel (Master) and check its button.
    // Done after widget construction and connects so that setCurve() + the
    // resulting repaint reach the fully-configured widget.
    selectCurveChannel(m_selectedCurveChannel);

    // Initialize HSL UI to the default channel (Red). This both checks the
    // right button and repopulates the three sliders from m_look — crucial
    // when a future preset-load path assigns a non-zero Look before showing
    // the window. Signals are blocked inside selectHslChannel, so this call
    // doesn't kick a debounce at startup.
    selectHslChannel(m_selectedHslChannel);

    scroll->setWidget(panel);
    outerLay->addWidget(scroll, /*stretch=*/1);
    return outer;
}

QWidget* MainWindow::buildSliderRow(const QString& label,
                                    int minValue, int maxValue, int initialValue,
                                    QSlider*& outSlider,
                                    QLabel*& outValueLabel)
{
    auto* row = new QWidget(this);
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(6);

    auto* name = new QLabel(label, row);
    name->setMinimumWidth(74);

    auto* slider = new NoWheelSlider(Qt::Horizontal, row);
    slider->setRange(minValue, maxValue);
    slider->setValue(initialValue);
    slider->setTracking(true);   // valueChanged fires during drag

    auto* val = new QLabel(QString::number(initialValue), row);
    val->setMinimumWidth(42);
    val->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    QFont vf = val->font();
    vf.setStyleHint(QFont::Monospace);
    val->setFont(vf);

    h->addWidget(name, 0);
    h->addWidget(slider, 1);
    h->addWidget(val, 0);

    outSlider = slider;
    outValueLabel = val;
    return row;
}

void MainWindow::setAnalysisPanelCollapsed(bool collapsed)
{
    m_analysisPanelCollapsed = collapsed;
    if (m_settings)
        m_settings->setAnalysisPanelCollapsed(collapsed);

    if (m_analysisPanel) {
        if (collapsed) {
            m_analysisPanel->setMinimumWidth(0);
            m_analysisPanel->setMaximumWidth(0);
            m_analysisPanel->setVisible(false);
        } else {
            m_analysisPanel->setVisible(true);
            m_analysisPanel->setMinimumWidth(230);
            m_analysisPanel->setMaximumWidth(260);
        }
    }

    if (m_analysisCollapseBtn) {
        m_analysisCollapseBtn->setText(collapsed
            ? QStringLiteral(">")
            : QStringLiteral("<"));
        m_analysisCollapseBtn->setToolTip(collapsed
            ? tr("Expand analysis panel")
            : tr("Collapse analysis panel"));
    }
}

void MainWindow::scrollInspectorTo(QWidget* section)
{
    if (m_sidebarCollapsed)
        onToggleSidebar();

    if (m_sidebarHost) {
        m_sidebarHost->setVisible(true);
        m_sidebarHost->setMaximumWidth(QWIDGETSIZE_MAX);
    }
    if (m_sidebarStack && m_sidebarFull)
        m_sidebarStack->setCurrentWidget(m_sidebarFull);

    if (m_controlScroll && section)
        m_controlScroll->ensureWidgetVisible(section, 0, 8);
}

void MainWindow::handleRailAction(const QString& action)
{
    const QString key = action.toLower();

    if (key == QStringLiteral("library")) {
        QMessageBox::information(this, tr("Library"),
                                 tr("This feature isn't implemented yet."));
    } else if (key == QStringLiteral("histogram")) {
        setAnalysisPanelCollapsed(false);
        if (m_histogramWidget) m_histogramWidget->setVisible(true);
        if (m_actShowHistogram) {
            QSignalBlocker block(m_actShowHistogram);
            m_actShowHistogram->setChecked(true);
        }
        if (m_analysisScroll && m_histogramWidget)
            m_analysisScroll->ensureWidgetVisible(m_histogramWidget, 0, 8);
    } else if (key == QStringLiteral("tone")) {
        scrollInspectorTo(m_toneSection);
    } else if (key == QStringLiteral("color")) {
        scrollInspectorTo(m_colorSection);
    } else if (key == QStringLiteral("hsl")) {
        scrollInspectorTo(m_hslSection);
    } else if (key == QStringLiteral("curves")) {
        scrollInspectorTo(m_curvesSection);
    } else if (key == QStringLiteral("grading")) {
        scrollInspectorTo(m_gradingSection);
    } else if (key == QStringLiteral("lens")) {
        scrollInspectorTo(m_lensSection);
    } else if (key == QStringLiteral("details")) {
        scrollInspectorTo(m_detailsSection);
    } else if (key == QStringLiteral("masks")) {
        scrollInspectorTo(m_masksSection);
    } else if (key == QStringLiteral("layers")) {
        if (m_bottomPanelTabs) m_bottomPanelTabs->setCurrentIndex(1);
        m_bottomWorkspaceEnabled = true;
        if (m_settings) m_settings->setBottomWorkspaceVisible(true);
        setBottomWorkspaceCollapsed(false);
        updateBottomWorkspaceVisibility();
        scrollInspectorTo(m_layersSection);
    } else if (key == QStringLiteral("nodes")) {
        onShowNodeGraph();
    } else if (key == QStringLiteral("export")) {
        onExportImage();
    }
}

void MainWindow::updateNavigatorPreview()
{
    if (!m_navigatorPreview) return;

    const QImage image = !m_processed.isNull() ? m_processed : m_previewSource;
    if (image.isNull()) {
        m_navigatorPreview->clear();
        m_navigatorPreview->setText(tr("No image"));
        return;
    }

    QSize target = m_navigatorPreview->contentsRect().size();
    if (target.width() < 16 || target.height() < 16)
        target = QSize(200, 120);

    m_navigatorPreview->setText(QString());
    m_navigatorPreview->setPixmap(QPixmap::fromImage(image).scaled(
        target, Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

void MainWindow::updateMetadataPanel()
{
    const QString missing = QString::fromUtf8("\xE2\x80\x94");
    auto setValue = [](QLabel* label, const QString& value) {
        if (label) label->setText(value);
    };
    auto display = [&missing](const QString& value) {
        return value.trimmed().isEmpty() ? missing : value.trimmed();
    };

    if (m_originalFullRes.isNull()) {
        setValue(m_metaFileName, missing);
        setValue(m_metaDimensions, missing);
        setValue(m_metaDateTime, missing);
        setValue(m_metaIso, missing);
        setValue(m_metaFocalLength, missing);
        setValue(m_metaAperture, missing);
        setValue(m_metaShutterSpeed, missing);
        setValue(m_metaCameraModel, missing);
        setValue(m_metaLensModel, missing);
        return;
    }

    const QFileInfo fileInfo(m_currentImagePath);
    const lps::ImageMetadata metadata =
        lps::ImageMetadataReader::read(m_currentImagePath);

    setValue(m_metaFileName, display(fileInfo.fileName()));
    setValue(m_metaDimensions,
             tr("%1 x %2").arg(m_originalFullRes.width()).arg(m_originalFullRes.height()));
    setValue(m_metaDateTime, display(metadata.captureDateTime));
    setValue(m_metaIso, display(metadata.iso));
    setValue(m_metaFocalLength, display(metadata.focalLength));
    setValue(m_metaAperture, display(metadata.aperture));
    setValue(m_metaShutterSpeed, display(metadata.shutterSpeed));
    setValue(m_metaCameraModel, display(metadata.cameraModel));
    setValue(m_metaLensModel, display(metadata.lensModel));
}

// ==============================================================================
// HSL channel selection
//
// Called when the user clicks a channel button in the HSL section, and once
// at construction to initialize the default selection. Three jobs:
//
//   1. Update m_selectedHslChannel so the slider lambdas dispatch to the
//      right sub-struct inside m_look.color.hsl.
//   2. Repopulate the three sliders with the selected channel's current
//      hue/saturation/luminance values, so the visible slider positions
//      reflect the stored state. Uses QSignalBlocker so these programmatic
//      setValue() calls don't trigger the valueChanged lambdas — otherwise
//      switching channels would spam phantom writes and restart the
//      debounce unnecessarily.
//   3. Keep the channel button group in sync — only the selected button
//      should be in the checked state. Also done under signal-block to
//      avoid re-entering this function via the button's own clicked signal.
//
// Slider float values in m_look are stored as float in [-100, +100] so
// static_cast<int> round-trips cleanly for our integer-only sliders.
// ==============================================================================
void MainWindow::selectHslChannel(int channelIndex)
{
    static_assert(kHslChannels.size() == kHslChannelCount,
                  "kHslChannels (MainWindow.cpp) and kHslChannelCount "
                  "(MainWindow.h) must agree on the number of channels");

    if (channelIndex < 0 || channelIndex >= kHslChannelCount) return;
    m_selectedHslChannel = channelIndex;

    // Pull the three stored values for the selected channel.
    const lps::HSLChannel& ch =
        m_look.color.hsl.*kHslChannels[channelIndex].field;
    const int hueVal = static_cast<int>(ch.hue);
    const int satVal = static_cast<int>(ch.saturation);
    const int lumVal = static_cast<int>(ch.luminance);

    // Reposition the three sliders + readouts without firing valueChanged.
    if (m_hslHueSlider) {
        QSignalBlocker block(m_hslHueSlider);
        m_hslHueSlider->setValue(hueVal);
    }
    if (m_hslHueValue) m_hslHueValue->setText(QString::number(hueVal));

    if (m_hslSaturationSlider) {
        QSignalBlocker block(m_hslSaturationSlider);
        m_hslSaturationSlider->setValue(satVal);
    }
    if (m_hslSaturationValue) m_hslSaturationValue->setText(QString::number(satVal));

    if (m_hslLuminanceSlider) {
        QSignalBlocker block(m_hslLuminanceSlider);
        m_hslLuminanceSlider->setValue(lumVal);
    }
    if (m_hslLuminanceValue) m_hslLuminanceValue->setText(QString::number(lumVal));

    // Sync button check states. We're not using QButtonGroup exclusive mode
    // because we want the selection to persist even if the user clicks the
    // already-selected button (no-op rather than deselect-to-nothing).
    // Manual toggle is simpler and gives us the right behavior directly.
    for (int i = 0; i < kHslChannelCount; ++i) {
        if (!m_hslChannelButtons[i]) continue;
        QSignalBlocker block(m_hslChannelButtons[i]);
        m_hslChannelButtons[i]->setChecked(i == channelIndex);
    }
}

// ==============================================================================
// Targeted Color Adjustment Tool (HSL eyedropper)
//
// PreviewWidget hands us a QColor sampled from m_processed when the user
// clicks while sampling mode is active. We convert RGB→HSV, find the
// nearest Lightroom-style HSL channel by hue distance (with proper
// angular wrap-around), then call selectHslChannel to make it the
// active channel in the panel.
//
// Design notes / future-compatibility:
//   - The mapping table below (channel → center hue) is the standard
//     Lightroom layout. Centers at 0/30/60/120/180/240/270/300 give
//     each channel a band of roughly 30–60° on the wheel.
//   - This code path does NOT trigger a render. Per spec: "Only trigger
//     render when an HSL slider value changes." selectHslChannel just
//     repopulates the slider widgets from m_look (signal-blocked), no
//     render kick.
//   - The same QColor → channel function would serve future tools
//     (color-grading wheel sampling, mask range selection). If/when
//     more callers appear, lift it into a helper namespace.
// ==============================================================================
namespace {

// Lightroom convention. Index here matches kHslChannelCount index in
// MainWindow (Red=0 ... Magenta=7).
constexpr struct { const char* displayName; double centerHue; } kHslChannelHues[8] = {
    { "Red",     0.0   },
    { "Orange",  30.0  },
    { "Yellow",  60.0  },
    { "Green",   120.0 },
    { "Aqua",    180.0 },
    { "Blue",    240.0 },
    { "Purple",  270.0 },
    { "Magenta", 300.0 },
};

// Angular distance on the [0, 360) hue wheel. Returns the shorter
// arc length between a and b — always in [0, 180].
inline double hueDistance(double a, double b)
{
    double d = std::fabs(a - b);
    if (d > 180.0) d = 360.0 - d;
    return d;
}

// RGB → HSV using QColor (saves us from re-deriving the math). Returns
// hue in degrees [0, 360); saturation and value in [0, 1]. For pure
// grayscale, QColor::hueF() returns -1; we map that to 0 hue with sat=0
// so callers can still find a "nearest" channel for display purposes.
struct Hsv { double h, s, v; };
inline Hsv toHsv(const QColor& c)
{
    QColor hsv = c.toHsv();
    double h = hsv.hueF();
    if (h < 0.0) h = 0.0;            // grayscale → hue undefined
    return { h * 360.0, hsv.saturationF(), hsv.valueF() };
}

// Pick the channel whose center hue is closest to the sampled hue.
// Returns the index in [0, 7].
int nearestHslChannel(double hueDeg)
{
    int bestIdx = 0;
    double bestDist = 1e9;
    for (int i = 0; i < 8; ++i) {
        const double d = hueDistance(hueDeg, kHslChannelHues[i].centerHue);
        if (d < bestDist) {
            bestDist = d;
            bestIdx  = i;
        }
    }
    return bestIdx;
}

} // namespace

void MainWindow::onColorSampled(QColor color, QPoint imagePos)
{
    Q_UNUSED(imagePos);   // not used today; reserved for future tools
    static_assert(sizeof(kHslChannelHues) / sizeof(kHslChannelHues[0]) == kHslChannelCount,
                  "kHslChannelHues and kHslChannelCount must agree");

    if (!color.isValid()) return;

    const Hsv hsv = toHsv(color);
    const int channelIdx = nearestHslChannel(hsv.h);

    // Drive the existing channel-selection path. selectHslChannel updates
    // the buttons' checked state, repoints the three sliders to the new
    // channel's fields, and signal-blocks the slider updates so this
    // doesn't kick a render. m_selectedHslChannel is also updated inside.
    selectHslChannel(channelIdx);

    if (m_hslTargetStatus) {
        // Show channel name + sampled hue/saturation. The numeric readout
        // helps users see when their click landed on a near-gray pixel
        // (low saturation → channel choice may be unstable on slight
        // changes) — same information surface DaVinci's qualifier
        // dropdown shows when sampling.
        const QString name = QString::fromLatin1(
            kHslChannelHues[channelIdx].displayName);
        const int huePct = static_cast<int>(std::lround(hsv.h));
        const int satPct = static_cast<int>(std::lround(hsv.s * 100.0));
        m_hslTargetStatus->setText(
            tr("Selected: %1  (%2°, %3%)").arg(name)
                                            .arg(huePct)
                                            .arg(satPct));
    }
}

// ==============================================================================
// Curve channel selection
//
// Swaps which CurvePoints the editor widget is mutating, updates the line
// color to match the channel, and keeps the channel buttons in sync. The
// actual curve data lives inside m_look; we just point the editor at the
// right field via kCurveChannels[i].field.
// ==============================================================================
void MainWindow::selectCurveChannel(int channelIndex)
{
    static_assert(kCurveChannels.size() == kCurveChannelCount,
                  "kCurveChannels (MainWindow.cpp) and kCurveChannelCount "
                  "(MainWindow.h) must agree on the number of channels");

    if (channelIndex < 0 || channelIndex >= kCurveChannelCount) return;
    m_selectedCurveChannel = channelIndex;

    if (m_curveEditor) {
        const auto& spec = kCurveChannels[channelIndex];
        m_curveEditor->setCurveColor(spec.lineColor);
        m_curveEditor->setCurve(&(m_look.curves.*spec.field));
    }

    // Sync button check states — manual toggle so clicking the already-
    // selected tab is a no-op (can't deselect to nothing). Same pattern as
    // the HSL channel selector.
    for (int i = 0; i < kCurveChannelCount; ++i) {
        if (!m_curveChannelButtons[i]) continue;
        QSignalBlocker block(m_curveChannelButtons[i]);
        m_curveChannelButtons[i]->setChecked(i == channelIndex);
    }
}

QString MainWindow::historyLabelForSender(const QObject* senderObj) const
{
    if (!senderObj) return QString();

    if (senderObj == m_exposureSlider)   return tr("Exposure changed");
    if (senderObj == m_contrastSlider)   return tr("Contrast changed");
    if (senderObj == m_highlightsSlider) return tr("Highlights changed");
    if (senderObj == m_shadowsSlider)    return tr("Shadows changed");
    if (senderObj == m_whitesSlider)     return tr("Whites changed");
    if (senderObj == m_blacksSlider)     return tr("Blacks changed");
    if (senderObj == m_brightnessSlider) return tr("Brightness changed");

    if (senderObj == m_temperatureSlider) return tr("Temperature changed");
    if (senderObj == m_tintSlider)        return tr("Tint changed");
    if (senderObj == m_vibranceSlider)    return tr("Vibrance changed");
    if (senderObj == m_saturationSlider)  return tr("Saturation changed");
    if (senderObj == m_hslHueSlider ||
        senderObj == m_hslSaturationSlider ||
        senderObj == m_hslLuminanceSlider) {
        return tr("HSL changed");
    }

    if (senderObj == m_curveEditor) return tr("Curve changed");

    if (senderObj == m_lutOpacitySlider) return tr("LUT opacity changed");
    if (senderObj == m_lutEnabledCheck)  return tr("LUT enabled changed");

    if (senderObj == m_hdrEnabledCheck ||
        senderObj == m_hdrExposureBiasSlider ||
        senderObj == m_hdrHighlightCompressionSlider ||
        senderObj == m_hdrShoulderStrengthSlider ||
        senderObj == m_hdrMidtonePivotSlider ||
        senderObj == m_hdrSaturationPreserveSlider) {
        return tr("HDR tone mapping changed");
    }

    if (senderObj == m_lensEnabledCheck ||
        senderObj == m_lensRemoveCaCheck ||
        senderObj == m_lensDistortionSlider ||
        senderObj == m_lensVignettingSlider ||
        senderObj == m_lensPurpleFringeSlider ||
        senderObj == m_lensGreenFringeSlider) {
        return tr("Lens correction changed");
    }

    if (senderObj == m_straightenSlider ||
        senderObj == m_transformFlipHorizontalBtn ||
        senderObj == m_transformFlipVerticalBtn) {
        return tr("Transform changed");
    }

    if (senderObj == m_sharpeningAmountSlider ||
        senderObj == m_sharpeningRadiusSlider ||
        senderObj == m_sharpeningDetailSlider ||
        senderObj == m_sharpeningMaskingSlider) {
        return tr("Sharpening changed");
    }
    if (senderObj == m_luminanceNrSlider ||
        senderObj == m_luminanceDetailSlider ||
        senderObj == m_colorNrSlider ||
        senderObj == m_colorDetailSlider) {
        return tr("Noise reduction changed");
    }

    if (senderObj == m_maskList ||
        senderObj == m_maskExposureSlider ||
        senderObj == m_maskBrightnessSlider ||
        senderObj == m_maskContrastSlider ||
        senderObj == m_maskSaturationSlider ||
        senderObj == m_maskTemperatureSlider ||
        senderObj == m_maskTintSlider ||
        senderObj == m_maskFeatherSlider ||
        senderObj == m_maskDensitySlider ||
        senderObj == m_maskFlowSlider ||
        senderObj == m_maskBrushSizeSlider ||
        senderObj == m_maskOverlayOpacitySlider) {
        return tr("Mask changed");
    }

    if (senderObj == m_layerList ||
        senderObj == m_layerOpacitySlider ||
        senderObj == m_layerBlendModeCombo) {
        return tr("Layer changed");
    }

    for (const auto& wheel : m_gradingWheels) {
        if (senderObj == wheel.wheel ||
            senderObj == wheel.str ||
            senderObj == wheel.lum ||
            senderObj == wheel.resetBtn) {
            return tr("Color grading changed");
        }
    }
    if (senderObj == m_balanceSlider ||
        senderObj == m_blendingSlider ||
        senderObj == m_liftSlider ||
        senderObj == m_gammaSlider ||
        senderObj == m_gainSlider ||
        senderObj == m_offsetSlider ||
        senderObj == m_filmicContrastSlider ||
        senderObj == m_highlightRolloffSlider ||
        senderObj == m_shadowLiftSlider ||
        senderObj == m_fadeBlacksSlider ||
        senderObj == m_colorSeparationSlider) {
        return tr("Color grading changed");
    }

    if (const auto* button = qobject_cast<const QPushButton*>(senderObj)) {
        const QString text = button->text();
        if (text.contains(tr("Linear"), Qt::CaseInsensitive) ||
            text.contains(tr("Radial"), Qt::CaseInsensitive) ||
            text.contains(tr("Brush"), Qt::CaseInsensitive)) {
            return tr("Mask added");
        }
        if (text.contains(tr("Rotate"), Qt::CaseInsensitive) ||
            text.contains(tr("Flip"), Qt::CaseInsensitive) ||
            text.contains(tr("Transform"), Qt::CaseInsensitive) ||
            text.contains(tr("Crop"), Qt::CaseInsensitive)) {
            return tr("Transform changed");
        }
    }

    if (const auto* action = qobject_cast<const QAction*>(senderObj)) {
        QString text = action->text();
        text.remove(QLatin1Char('&'));
        if (text.contains(tr("Reset Edits"), Qt::CaseInsensitive))
            return tr("Reset edits");
        if (text.contains(tr("Rotate"), Qt::CaseInsensitive) ||
            text.contains(tr("Flip"), Qt::CaseInsensitive))
            return tr("Transform changed");
    }

    return QString();
}

void MainWindow::recordHistoryStep(QString label)
{
    label = label.trimmed();
    if (label.isEmpty()) label = tr("Edit");

    if (m_historyCurrentIndex >= 0 &&
        m_historyCurrentIndex + 1 < static_cast<int>(m_historyEntries.size())) {
        m_historyEntries.erase(m_historyEntries.begin() + m_historyCurrentIndex + 1,
                               m_historyEntries.end());
    }

    m_historyEntries.push_back(HistoryEntry{ label, m_look });
    while (m_historyEntries.size() > kMaxUndoDepth + 1) {
        m_historyEntries.erase(m_historyEntries.begin());
    }

    m_historyCurrentIndex = static_cast<int>(m_historyEntries.size()) - 1;
    refreshHistoryList();
    saveActiveDocumentState();
}

void MainWindow::clearHistory(const QString& baselineLabel)
{
    m_historyEntries.clear();
    m_historyCurrentIndex = -1;
    m_nextHistoryLabel.clear();

    const QString label = baselineLabel.trimmed();
    if (!label.isEmpty()) {
        m_historyEntries.push_back(HistoryEntry{ label, m_look });
        m_historyCurrentIndex = 0;
    }

    refreshHistoryList();
    saveActiveDocumentState();
}

void MainWindow::refreshHistoryList()
{
    if (!m_historyList) return;

    m_syncingHistorySelection = true;
    QSignalBlocker block(m_historyList);
    m_historyList->clear();

    if (m_historyEntries.empty()) {
        auto* item = new QListWidgetItem(tr("No history yet"), m_historyList);
        item->setFlags(Qt::NoItemFlags);
        item->setForeground(QColor(0x7A, 0x7A, 0x7A));
        m_historyCurrentIndex = -1;
        m_syncingHistorySelection = false;
        return;
    }

    for (int i = 0; i < static_cast<int>(m_historyEntries.size()); ++i) {
        const HistoryEntry& entry = m_historyEntries[static_cast<size_t>(i)];
        auto* item = new QListWidgetItem(entry.label, m_historyList);
        item->setToolTip(tr("History state stored. Click-to-restore is a future step."));
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    }

    if (m_historyCurrentIndex >= 0 &&
        m_historyCurrentIndex < static_cast<int>(m_historyEntries.size())) {
        m_historyList->setCurrentRow(m_historyCurrentIndex);
    }
    m_syncingHistorySelection = false;
}

void MainWindow::updateCurrentHistorySnapshot()
{
    if (m_historyCurrentIndex < 0 ||
        m_historyCurrentIndex >= static_cast<int>(m_historyEntries.size())) {
        return;
    }
    m_historyEntries[static_cast<size_t>(m_historyCurrentIndex)].snapshot = m_look;
    saveActiveDocumentState();
}

// ==============================================================================
// Undo / Redo
//
// The undo/redo model snapshots the entire lps::Look as a value type. A Look
// is a small (~hundreds of bytes) aggregate of POD fields plus a few
// std::vector<QPointF> for curve points — cheap to copy and trivially
// round-trippable. 100 snapshots cap memory below ~100 KB.
//
// Boundaries are set at user-initiated operation starts:
//   - Slider sliderPressed (not valueChanged) → one snapshot per drag gesture
//   - Curve editor editStarted              → one snapshot per edit operation
//
// This is the "one gesture = one undo step" invariant. Continuously pushing
// on valueChanged/curveChanged would flood the stack with microscopic deltas
// and make Ctrl+Z walk back one mouse-tick at a time. Not useful.
//
// The m_isApplyingLookToUi flag is a defensive guard. It's technically not
// strictly needed given that we push on sliderPressed (which setValue never
// emits) rather than valueChanged — but it costs nothing and protects
// against future changes where someone adds a pushUndoSnapshot() to a path
// that applyLookToUi() might trigger.
// ==============================================================================
void MainWindow::pushUndoSnapshot()
{
    // Re-entry guard. applyLookToUi sets this true; any pushUndoSnapshot
    // call that somehow fires during programmatic widget updates gets
    // swallowed here.
    if (m_isApplyingLookToUi) {
        m_nextHistoryLabel.clear();
        return;
    }

    QString historyLabel = m_nextHistoryLabel.trimmed();
    m_nextHistoryLabel.clear();
    if (historyLabel.isEmpty())
        historyLabel = historyLabelForSender(sender());

    m_undoStack.push_back(m_look);
    recordHistoryStep(historyLabel);

    // A new edit invalidates any future-branch that was being held in redo.
    // This is the standard linear-history model: branching would require
    // a tree, which is beyond the spec.
    m_redoStack.clear();

    // Cap the undo depth. Erasing from the front is O(n) on vector but n
    // is at most 100 and this runs at drag-start frequency (human timescale),
    // not per frame — negligible cost.
    while (m_undoStack.size() > kMaxUndoDepth) {
        m_undoStack.erase(m_undoStack.begin());
    }

    refreshUndoRedoActions();
    saveActiveDocumentState();
}

void MainWindow::undo()
{
    if (m_undoStack.empty()) return;

    // Current state → redo stack; previous state → m_look.
    m_redoStack.push_back(m_look);
    m_look = m_undoStack.back();
    m_undoStack.pop_back();

    if (m_historyCurrentIndex > 0)
        --m_historyCurrentIndex;
    applyLookToUi();
    refreshUndoRedoActions();
    refreshHistoryList();
    saveActiveDocumentState();
}

void MainWindow::redo()
{
    if (m_redoStack.empty()) return;

    // Current state → undo stack; next state → m_look.
    m_undoStack.push_back(m_look);
    m_look = m_redoStack.back();
    m_redoStack.pop_back();
    if (m_historyCurrentIndex + 1 < static_cast<int>(m_historyEntries.size()))
        ++m_historyCurrentIndex;

    // Note: we do NOT cap the undo stack here. The user can't possibly have
    // redone more than they undid (that'd require the redo stack to have
    // more entries than the undo stack's capacity), so the natural growth
    // is bounded by the cap enforced in pushUndoSnapshot().

    applyLookToUi();
    refreshUndoRedoActions();
    refreshHistoryList();
    saveActiveDocumentState();
}

void MainWindow::applyLookToUi()
{
    // Scope guard: every programmatic widget update below should NOT produce
    // a new undo snapshot. We set the flag on entry and clear on exit.
    // Using a RAII helper rather than a manual flag flip means early returns
    // or exceptions (unlikely here, but defensively) still clean up.
    struct ScopedFlag {
        bool& flag;
        explicit ScopedFlag(bool& f) : flag(f) { flag = true; }
        ~ScopedFlag() { flag = false; }
    } guard(m_isApplyingLookToUi);

    // ---- Tone sliders -----------------------------------------------------
    // Exposure uses kExposureScale (stored as stops; slider int = stops×100).
    // The rest map 1:1 between slider int and stored float.
    auto setIntSliderAndLabel = [](QSlider* slider, QLabel* label, int value) {
        if (slider) {
            QSignalBlocker block(slider);
            slider->setValue(value);
        }
        if (label) label->setText(QString::number(value));
    };

    if (m_exposureSlider) {
        QSignalBlocker block(m_exposureSlider);
        m_exposureSlider->setValue(
            static_cast<int>(m_look.tone.exposure * kExposureScale));
    }
    if (m_exposureValue) {
        m_exposureValue->setText(
            QString::number(m_look.tone.exposure, 'f', 2));
    }

    setIntSliderAndLabel(m_contrastSlider,   m_contrastValue,
                         static_cast<int>(m_look.tone.contrast));
    setIntSliderAndLabel(m_highlightsSlider, m_highlightsValue,
                         static_cast<int>(m_look.tone.highlights));
    setIntSliderAndLabel(m_shadowsSlider,    m_shadowsValue,
                         static_cast<int>(m_look.tone.shadows));
    setIntSliderAndLabel(m_whitesSlider,     m_whitesValue,
                         static_cast<int>(m_look.tone.whites));
    setIntSliderAndLabel(m_blacksSlider,     m_blacksValue,
                         static_cast<int>(m_look.tone.blacks));
    setIntSliderAndLabel(m_brightnessSlider, m_brightnessValue,
                         static_cast<int>(m_look.tone.brightness));

    // ---- Color sliders ----------------------------------------------------
    setIntSliderAndLabel(m_temperatureSlider, m_temperatureValue,
                         static_cast<int>(m_look.color.whiteBalance.temperature));
    setIntSliderAndLabel(m_tintSlider,        m_tintValue,
                         static_cast<int>(m_look.color.whiteBalance.tint));
    setIntSliderAndLabel(m_vibranceSlider,    m_vibranceValue,
                         static_cast<int>(m_look.color.vibrance));
    setIntSliderAndLabel(m_saturationSlider,  m_saturationValue,
                         static_cast<int>(m_look.color.saturation));

    // ---- HSL section ------------------------------------------------------
    // selectHslChannel() already handles its own QSignalBlocker pattern for
    // the three shared sliders, their readouts, AND the eight button check
    // states. Reusing it here avoids duplicating that logic and keeps the
    // HSL side as the single source of truth for "refresh the HSL UI from
    // m_look for the current selected channel."
    selectHslChannel(m_selectedHslChannel);

    // ---- Curve editor -----------------------------------------------------
    // Same reasoning: selectCurveChannel re-points the editor at the right
    // CurvePoints field inside the (now-restored) m_look. The field address
    // is stable across the m_look = snapshot assignment because m_look is
    // a value member — we assigned INTO it, not replaced it. The editor's
    // existing pointer into m_look.curves.* remains valid, but we call
    // setCurve() again anyway because it also triggers a repaint, and
    // the content it's pointing at has changed.
    selectCurveChannel(m_selectedCurveChannel);

    // ---- COLOR GRADING (LUT) ----------------------------------------------
    // Mirrors the HSL/curve refresh above: undo/redo or preset load may have
    // changed grading.lutPath or grading.lutOpacity, so the widgets need to
    // catch up. refreshLutWidgets blocks the slider's signals internally,
    // so this doesn't kick the debounce twice.
    refreshLutWidgets();

    // ---- 3-way color grading wheels ---------------------------------------
    // Sliders for the 12 wheel parameters plus balance/blending. All
    // updates are signal-blocked internally to avoid kicking the debounce
    // a second time.
    refreshGradingWidgets();

    // ---- HDR tone mapping ------------------------------------------------
    refreshHdrWidgets();

    // ---- Lens correction --------------------------------------------------
    // Master-enable + four sliders + CA checkbox. Same lifecycle as the
    // other refresh functions — signal-blocked, no debounce kick.
    refreshLensWidgets();

    // ---- Transform --------------------------------------------------------
    refreshTransformWidgets();

    // ---- Details ----------------------------------------------------------
    refreshDetailsWidgets();

    // ---- Local masks ------------------------------------------------------
    // Rebuild the mask list and re-sync the per-mask sliders. Handles undo/
    // redo replacing m_look entirely, project/preset load with new masks,
    // and selection-index clamping when the list shrinks.
    refreshMaskWidgets();

    // ---- Adjustment layers ------------------------------------------------
    // Same lifecycle handling as masks — list rebuild + selection clamp +
    // per-layer control sync.
    refreshLayerWidgets();

    // ---- Kick a render ----------------------------------------------------
    // One debounce restart, not one per setter. The debounce will coalesce
    // with any pending renders and dispatch a single threaded re-render.
    if (m_debounce) m_debounce->start();

    // m_curveDragUndoCaptured: reset defensively. An undo/redo should not
    // happen mid-drag (shortcuts route through the app, and Qt delivers
    // release events reliably after a press), but if some weird path did
    // leave this flag true, clearing it here means the NEXT curve edit
    // will correctly open a new undo boundary.
    m_curveDragUndoCaptured = false;
}

// ==============================================================================
// File I/O
// ==============================================================================
namespace {

bool isSupportedImagePath(const QString& path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    static const QStringList standard = {
        QStringLiteral("png"),
        QStringLiteral("jpg"),
        QStringLiteral("jpeg"),
        QStringLiteral("bmp"),
        QStringLiteral("tif"),
        QStringLiteral("tiff"),
        QStringLiteral("webp")
    };
    return standard.contains(suffix) || lps::RawImageLoader::isRawExtension(path);
}

QString firstSupportedDroppedImage(const QMimeData* mime)
{
    if (!mime || !mime->hasUrls())
        return QString();
    for (const QUrl& url : mime->urls()) {
        if (!url.isLocalFile())
            continue;
        const QString path = url.toLocalFile();
        if (QFileInfo(path).isFile() && isSupportedImagePath(path))
            return path;
    }
    return QString();
}

QImage loadImageForEditor(const QString& path, QString* error)
{
    if (error) error->clear();

    if (lps::RawImageLoader::isRawExtension(path)) {
        return lps::RawImageLoader::load(path, error);
    }

    QImage img;
    if (!img.load(path) && error)
        *error = QObject::tr("Could not load: %1").arg(path);
    return img;
}

} // namespace

void MainWindow::onOpenImage()
{
    // Behavior: opening a new image gives a clean slate — all edits reset
    // to default. (Previous version preserved the Look across image opens
    // for "apply consistent edits to a series" workflows; users found the
    // hidden carry-over surprising, so we now reset.) If you want to
    // share edits across images, use Save Project / Open Project instead.
    //
    const QString picturesDir = m_settings
        ? m_settings->lastOpenFolder()
        : QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    const QString path = QFileDialog::getOpenFileName(
        this,
        tr("Open Image"),
        picturesDir,
        tr("Images (*.png *.jpg *.jpeg *.bmp *.tif *.tiff *.webp "
           "*.cr2 *.cr3 *.nef *.arw *.dng *.raf *.orf *.rw2);;"
           "RAW Images (*.cr2 *.cr3 *.nef *.arw *.dng *.raf *.orf *.rw2);;"
           "Standard Images (*.png *.jpg *.jpeg *.bmp *.tif *.tiff *.webp)"));
    if (path.isEmpty()) return;

    loadImageFromPath(path);
}

bool MainWindow::loadImageFromPath(const QString& path)
{
    QString loadError;
    QImage img = loadImageForEditor(path, &loadError);
    if (img.isNull()) {
        QMessageBox::warning(this, tr("Open Image"),
                             loadError.isEmpty()
                                 ? tr("Could not load: %1").arg(path)
                                 : loadError);
        return false;
    }

    saveActiveDocumentState();
    m_isLoadingProject = true;

    // ---- Commit the new image + reset all editor state -------------------
    m_originalFullRes = img;
    m_currentImagePath = path;
    m_currentProjectPath.clear();   // new image = no project yet
    m_projectCreatedDate = QDateTime();
    m_projectModifiedDate = QDateTime();
    if (m_settings) {
        m_settings->setLastOpenFolder(QFileInfo(path).absolutePath());
        m_settings->addRecentImage(path);
        refreshWelcomeRecentFiles();
    }

    const int longest = std::max(img.width(), img.height());
    if (longest <= kPreviewMaxEdge) {
        m_previewSource = img;
    } else {
        m_previewSource = img.scaled(kPreviewMaxEdge, kPreviewMaxEdge,
                                     Qt::KeepAspectRatio,
                                     Qt::SmoothTransformation);
    }
    m_processed = QImage();
    if (m_histogramWidget) m_histogramWidget->setImage(m_previewSource);
    updateMetadataPanel();
    updateNavigatorPreview();

    // Reset the edit state. lps::Look is a default-constructible aggregate
    // whose member defaults all correspond to identity — sliders at 0,
    // curves at the {(0,0),(1,1)} two-point identity, etc. Plain assignment
    // is the canonical reset.
    m_look = lps::Look{};
    if (m_previewLabel) m_previewLabel->setCropOverlayActive(false);
    if (m_cropToolBtn) {
        QSignalBlocker block(m_cropToolBtn);
        m_cropToolBtn->setChecked(false);
    }

    // Clear undo history. The new image is its own clean baseline; letting
    // the user undo into the previous image's edit chain would be very
    // confusing (and the snapshots would be applied to a different source).
    m_undoStack.clear();
    m_redoStack.clear();
    clearHistory(tr("Open Image"));
    refreshUndoRedoActions();

    // Push the freshly-default Look into every UI widget. m_isLoadingProject
    // is repurposed here as a generic "next render isn't a user edit"
    // suppression flag — it gates the dirty-mark inside the debounce
    // handler, so the render that applyLookToUi triggers doesn't make the
    // new untitled project look dirty before the user has touched anything.
    applyLookToUi();
    // applyLookToUi kicked the debounce; cancel it because we're about to
    // render directly. Without this, we'd render twice for the same Look.
    if (m_debounce) m_debounce->stop();
    m_isLoadingProject = false;

    // Clean baseline: not dirty, refresh the title to "Untitled" with no
    // asterisk.
    m_projectDirty = false;
    appendCurrentStateAsDocument();
    updateWindowTitle();

    // Hide the empty-state overlay so the preview widget's image content
    // shows through. Symmetric with showing it again on the no-image
    // branches (see loadProjectFromPath).
    if (m_emptyState) m_emptyState->hide();
    showEditorWorkspace();

    // Reset the preview to Fit mode for the newly-loaded image. Without
    // this, leftover zoom/pan state from a previous image would carry over
    // — typically not what users want when opening a new file.
    if (m_previewLabel) m_previewLabel->zoomToFit();

    // Immediate render so the new image appears now, not after a debounce.
    requestRender();
    return true;
}

void MainWindow::onOpenProject()
{
    const QString picturesDir = m_settings
        ? m_settings->lastOpenFolder()
        : QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    const QString path = QFileDialog::getOpenFileName(
        this,
        tr("Open Project"),
        picturesDir,
        tr("Lumen Projects (*.lps);;All Files (*)"));
    if (path.isEmpty()) return;

    if (!loadProjectFromPath(path)) {
        // loadProjectFromPath shows its own error dialog. Nothing to do here.
        return;
    }
}

void MainWindow::onSaveProject()
{
    // If we already have a project path, save there. Otherwise fall through
    // to Save As. This is the standard "Save / Save As" idiom.
    if (m_currentProjectPath.isEmpty()) {
        onSaveProjectAs();
        return;
    }
    if (!m_currentProjectPath.endsWith(QStringLiteral(".lps"), Qt::CaseInsensitive)) {
        onSaveProjectAs();
        return;
    }
    saveProjectToPath(m_currentProjectPath);
}

void MainWindow::onSaveProjectAs()
{
    const QString picturesDir = m_settings
        ? m_settings->lastOpenFolder()
        : QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    QString defaultName = QStringLiteral("Untitled.lps");
    if (!m_currentImagePath.isEmpty()) {
        // Suggest the source image's basename + .lps.
        const QFileInfo fi(m_currentImagePath);
        defaultName = fi.completeBaseName() + QStringLiteral(".lps");
    }

    const QString path = QFileDialog::getSaveFileName(
        this,
        tr("Save Project As"),
        picturesDir + QLatin1Char('/') + defaultName,
        tr("Lumen Projects (*.lps)"));
    if (path.isEmpty()) return;

    // Force the .lps extension if the user didn't supply one. Some platforms'
    // dialogs don't auto-append based on the filter.
    QString finalPath = path;
    if (!finalPath.endsWith(QStringLiteral(".lps"), Qt::CaseInsensitive)) {
        const QFileInfo typed(finalPath);
        finalPath = typed.suffix().isEmpty()
            ? finalPath + QStringLiteral(".lps")
            : typed.path() + QLatin1Char('/') + typed.completeBaseName()
                + QStringLiteral(".lps");
    }

    saveProjectToPath(finalPath);
}

void MainWindow::onExportImage()
{
    if (m_originalFullRes.isNull()) {
        QMessageBox::information(this, tr("Export Image"),
                                 tr("No image loaded."));
        return;
    }

    const QString picturesDir = m_settings
        ? m_settings->defaultExportFolder()
        : QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);

    // Suggest source basename as the default export name.
    QString defaultName = QStringLiteral("export.png");
    if (!m_currentImagePath.isEmpty()) {
        const QFileInfo fi(m_currentImagePath);
        defaultName = fi.completeBaseName() + QStringLiteral("_export.png");
    }

    ExportDialog dialog(picturesDir, defaultName, m_originalFullRes.size(), this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const ExportDialog::Options exportOptions = dialog.options();
    const QString path = exportOptions.outputPath();

    // Full-resolution render through the same pipeline. Blocks the UI thread —
    // acceptable for a desktop tool at typical photo sizes; a future step
    // could move this onto the existing QFutureWatcher infrastructure for
    // huge files.
    lps::ImagePipeline pipeline;
    const lps::RenderResult r = pipeline.render(m_originalFullRes, m_look);
    QImage exportImage = r.image;
    if (exportImage.isNull()) {
        QMessageBox::warning(this, tr("Export Image"), tr("Render failed."));
        return;
    }

    if (exportOptions.resize) {
        exportImage = exportImage.scaled(
            exportOptions.width,
            exportOptions.height,
            exportOptions.preserveAspectRatio
                ? Qt::KeepAspectRatio
                : Qt::IgnoreAspectRatio,
            Qt::SmoothTransformation);
    }

    if (exportOptions.format == QStringLiteral("JPG") &&
        exportImage.hasAlphaChannel()) {
        exportImage = exportImage.convertToFormat(QImage::Format_RGB888);
    }

    QImageWriter writer(path, exportOptions.imageFormat());
    if (exportOptions.format == QStringLiteral("JPG") ||
        exportOptions.format == QStringLiteral("WEBP")) {
        writer.setQuality(exportOptions.quality);
    }

    if (!writer.write(exportImage)) {
        QMessageBox::warning(
            this, tr("Export Image"),
            tr("Could not write: %1\n\n"
               "%2\n\n"
               "If you exported as WebP or TIFF, your Qt build may not "
               "include support for that format. Try PNG or JPEG.")
                .arg(path, writer.errorString()));
    } else if (m_settings) {
        m_settings->setDefaultExportFolder(QFileInfo(path).absolutePath());
    }
}

// ==============================================================================
// Project I/O — .lps file format
//
// JSON envelope:
//   {
//     "schemaVersion":      1,
//     "projectName":        "...",
//     "sourceImagePath":    "...",
//     "look":               { ... LookSerializer JSON ... }
//   }
//
// The "look" object is exactly what LookSerializer::toJson produces, embedded
// inline (not stringified) so the .lps file is human-readable. This means
// future Look schema changes propagate automatically.
//
// sourceImagePath is stored as an absolute path. Future work: consider
// storing it relative to the .lps file's directory so projects can be
// moved/shared. Out of scope for this step.
// ==============================================================================
bool MainWindow::saveProjectToPath(const QString& path)
{
    lps::ProjectDocument project;
    project.projectName = QFileInfo(path).completeBaseName();
    project.projectPathReference = path;
    project.sourceImagePath = m_currentImagePath;
    project.look = m_look;
    project.exportSettingsReference = QStringLiteral("default");
    project.createdDate = m_projectCreatedDate.isValid()
        ? m_projectCreatedDate
        : QDateTime::currentDateTimeUtc();
    project.modifiedDate = QDateTime::currentDateTimeUtc();

    const lps::ProjectSaveResult result =
        lps::ProjectSerializer::saveToFile(project, path);
    if (!result.ok) {
        QMessageBox::warning(this, tr("Save Project"), result.errorMessage);
        return false;
    }

    m_currentProjectPath = path;
    m_projectCreatedDate = project.createdDate;
    m_projectModifiedDate = project.modifiedDate;
    m_projectDirty = false;
    if (m_debounce && m_debounce->isActive()) {
        m_debounce->stop();
        requestRender();
    }
    saveActiveDocumentState();
    updateDocumentTabs();
    if (m_autosaveManager)
        m_autosaveManager->deleteAllAutosaves();
    if (m_settings) {
        m_settings->setLastOpenFolder(QFileInfo(path).absolutePath());
        m_settings->addRecentProject(path);
        refreshWelcomeRecentFiles();
    }
    updateWindowTitle();
    return true;
}

bool MainWindow::loadProjectFromPath(const QString& path)
{
    const lps::ProjectLoadResult result =
        lps::ProjectSerializer::loadFromFile(path);
    if (!result.ok) {
        QMessageBox::warning(this, tr("Open Project"), result.errorMessage);
        return false;
    }

    const lps::ProjectDocument project = result.document;
    if (project.schemaVersion > lps::ProjectSerializer::kCurrentSchemaVersion) {
        QMessageBox::warning(
            this, tr("Open Project"),
            tr("This project was saved with a newer version of Lumen "
               "(schema %1). Some settings may not load correctly.")
                .arg(project.schemaVersion));
        // Continue loading anyway — the embedded LookSerializer JSON is
        // tolerant of missing fields, so we'll get a best-effort import.
    }

    QString imagePath = project.sourceImagePath;
    // Try to load the referenced source image. If it's missing/unreadable,
    // surface a non-fatal warning and continue — the user can still see
    // the Look settings, and "relink" workflows are common in real editors.
    QImage img;
    QString imageLoadError;
    bool imageLoaded = false;
    bool sourceRelinked = false;
    if (!imagePath.isEmpty()) {
        img = loadImageForEditor(imagePath, &imageLoadError);
        imageLoaded = !img.isNull();
    }
    if (!imageLoaded && !imagePath.isEmpty()) {
        QMessageBox relinkBox(this);
        relinkBox.setWindowTitle(tr("Missing Source Image"));
        relinkBox.setText(tr("Could not load the source image:\n%1").arg(imagePath));
        relinkBox.setInformativeText(imageLoadError.isEmpty()
            ? tr("Relink the image to restore the preview, or continue without it.")
            : imageLoadError);
        auto* relinkButton =
            relinkBox.addButton(tr("Relink Image"), QMessageBox::AcceptRole);
        auto* continueButton =
            relinkBox.addButton(tr("Continue Without Image"), QMessageBox::DestructiveRole);
        relinkBox.addButton(QMessageBox::Cancel);
        relinkBox.exec();

        if (relinkBox.clickedButton() == relinkButton) {
            const QString relinkPath = QFileDialog::getOpenFileName(
                this,
                tr("Relink Source Image"),
                QFileInfo(imagePath).absolutePath(),
                tr("Images (*.png *.jpg *.jpeg *.bmp *.tif *.tiff *.webp "
                   "*.cr2 *.cr3 *.nef *.arw *.dng *.raf *.orf *.rw2);;"
                   "RAW Images (*.cr2 *.cr3 *.nef *.arw *.dng *.raf *.orf *.rw2);;"
                   "Standard Images (*.png *.jpg *.jpeg *.bmp *.tif *.tiff *.webp)"));
            if (relinkPath.isEmpty())
                return false;
            imagePath = relinkPath;
            sourceRelinked = true;
            imageLoadError.clear();
            img = loadImageForEditor(imagePath, &imageLoadError);
            imageLoaded = !img.isNull();
            if (!imageLoaded) {
                QMessageBox::warning(this, tr("Open Project"),
                                     imageLoadError.isEmpty()
                                         ? tr("Could not load: %1").arg(imagePath)
                                         : imageLoadError);
                return false;
            }
        } else if (relinkBox.clickedButton() != continueButton) {
            return false;
        }
    }

    // ---- Commit the load -------------------------------------------------
    saveActiveDocumentState();

    // Suppress dirty-marking while we update m_look + UI. The debounce kick
    // inside applyLookToUi would otherwise call markDirty().
    m_isLoadingProject = true;

    m_look = project.look;
    m_currentImagePath = imagePath;
    m_currentProjectPath = path;
    m_projectCreatedDate = project.createdDate;
    m_projectModifiedDate = project.modifiedDate;
    if (m_previewLabel) m_previewLabel->setCropOverlayActive(false);
    if (m_cropToolBtn) {
        QSignalBlocker block(m_cropToolBtn);
        m_cropToolBtn->setChecked(false);
    }

    if (imageLoaded) {
        m_originalFullRes = img;
        const int longest = std::max(img.width(), img.height());
        m_previewSource = (longest <= kPreviewMaxEdge)
            ? img
            : img.scaled(kPreviewMaxEdge, kPreviewMaxEdge,
                         Qt::KeepAspectRatio, Qt::SmoothTransformation);
        m_processed = QImage();
        if (m_histogramWidget) m_histogramWidget->setImage(m_previewSource);
        if (m_emptyState) m_emptyState->hide();
    } else {
        // No image — clear preview state so the editor doesn't try to render
        // against stale pixels from a previous session.
        m_originalFullRes = QImage();
        m_previewSource = QImage();
        m_processed = QImage();
        // Also clear the histogram so it doesn't keep showing the previous
        // image's distribution.
        if (m_histogramWidget) m_histogramWidget->setImage(QImage());
        updateMetadataPanel();
        updateNavigatorPreview();
        // Wipe the preview widget so the overlay isn't drawn over a stale
        // image, then bring the empty-state overlay back.
        if (m_previewLabel) {
            m_previewLabel->setOriginalImage(QImage());
            m_previewLabel->setEditedImage(QImage());
            m_previewLabel->setShowOriginal(false);
            m_previewLabel->zoomToFit();   // reset zoom for the next image
        }
        if (m_emptyState) m_emptyState->show();
        // Clear the secondary viewer too if it's open — without this it
        // would keep showing the previous image after a project load
        // with a missing source.
        if (m_secondaryViewer) m_secondaryViewer->setImage(QImage());
    }

    // Reset undo history. The newly loaded project is its own clean baseline;
    // letting users undo back into a previous editing session would be
    // confusing.
    m_undoStack.clear();
    m_redoStack.clear();
    clearHistory(tr("Open Project"));
    refreshUndoRedoActions();

    // Push the loaded values into all UI widgets, then trigger a render.
    // applyLookToUi() kicks the debounce; that fires onDebounceFired which
    // will short-circuit the markDirty() because m_isLoadingProject is true.
    applyLookToUi();
    updateMetadataPanel();
    updateNavigatorPreview();

    m_isLoadingProject = false;

    m_projectDirty = sourceRelinked;
    appendCurrentStateAsDocument();

    if (imageLoaded) {
        requestRender();
    } else if (m_previewLabel) {
        m_previewLabel->setOriginalImage(QImage());
        m_previewLabel->setEditedImage(QImage());
    }

    if (sourceRelinked)
        scheduleAutosave();
    if (m_settings) {
        m_settings->setLastOpenFolder(QFileInfo(path).absolutePath());
        m_settings->addRecentProject(path);
        refreshWelcomeRecentFiles();
    }
    updateWindowTitle();
    showEditorWorkspace();
    return true;
}

bool MainWindow::recoverAutosaveFromPath(const QString& path)
{
    const lps::ProjectLoadResult result =
        lps::ProjectSerializer::loadFromFile(path);
    if (!result.ok) {
        QMessageBox::warning(this, tr("Autosave Recovery"), result.errorMessage);
        return false;
    }

    const QString projectReference = result.document.projectPathReference;
    if (!loadProjectFromPath(path))
        return false;

    m_currentProjectPath = projectReference;
    m_projectDirty = true;
    if (m_settings) {
        QStringList projects = m_settings->recentProjects();
        projects.removeAll(QDir::fromNativeSeparators(QFileInfo(path).absoluteFilePath()));
        m_settings->setRecentProjects(projects);
        if (!projectReference.isEmpty() && QFileInfo(projectReference).isFile())
            m_settings->addRecentProject(projectReference);
        refreshWelcomeRecentFiles();
    }
    updateWindowTitle();
    saveActiveDocumentState();
    updateDocumentTabs();
    return true;
}

void MainWindow::checkAutosaveRecovery()
{
    if (!m_autosaveManager || !m_autosaveManager->hasAutosave())
        return;

    const QString autosavePath = m_autosaveManager->latestAutosavePath();
    if (autosavePath.isEmpty())
        return;

    QMessageBox box(this);
    box.setWindowTitle(tr("Autosave Recovery"));
    box.setText(tr("Recover last session?"));
    box.setInformativeText(QFileInfo(autosavePath).fileName());
    auto* recoverButton = box.addButton(tr("Recover"), QMessageBox::AcceptRole);
    auto* discardButton = box.addButton(tr("Discard"), QMessageBox::DestructiveRole);
    box.setDefaultButton(recoverButton);
    box.exec();

    if (box.clickedButton() == recoverButton) {
        if (recoverAutosaveFromPath(autosavePath)) {
            m_autosaveManager->deleteAllAutosaves();
            scheduleAutosave();
        }
        return;
    }

    if (box.clickedButton() == discardButton)
        m_autosaveManager->deleteAllAutosaves();
}

lps::ProjectDocument MainWindow::currentProjectDocumentForAutosave() const
{
    lps::ProjectDocument project;
    project.projectName = m_currentProjectPath.isEmpty()
        ? tr("Recovered Session")
        : QFileInfo(m_currentProjectPath).completeBaseName();
    project.projectPathReference = m_currentProjectPath;
    project.sourceImagePath = m_currentImagePath;
    project.look = m_look;
    project.exportSettingsReference = QStringLiteral("default");
    project.createdDate = m_projectCreatedDate.isValid()
        ? m_projectCreatedDate
        : QDateTime::currentDateTimeUtc();
    project.modifiedDate = QDateTime::currentDateTimeUtc();
    return project;
}

void MainWindow::scheduleAutosave()
{
    if (!m_autosaveManager || m_isLoadingProject)
        return;
    if (m_currentImagePath.isEmpty()
        && m_currentProjectPath.isEmpty()
        && m_previewSource.isNull()) {
        return;
    }
    m_autosaveManager->schedule(currentProjectDocumentForAutosave());
}

// ==============================================================================
// Dirty state + window title
// ==============================================================================
void MainWindow::markDirty()
{
    const bool wasDirty = m_projectDirty;
    m_projectDirty = true;
    scheduleAutosave();
    saveActiveDocumentState();
    updateDocumentTabs();
    if (!wasDirty)
        updateWindowTitle();
}

void MainWindow::updateWindowTitle()
{
    QString projectLabel;
    if (!m_currentProjectPath.isEmpty()) {
        projectLabel = QFileInfo(m_currentProjectPath).fileName();
    } else if (!m_currentImagePath.isEmpty()) {
        projectLabel = QFileInfo(m_currentImagePath).fileName();
    } else {
        projectLabel = tr("Untitled");
    }
    const QString dirtyMark = m_projectDirty ? QStringLiteral(" *") : QString();
    setWindowTitle(tr("Lumen Photo Studio — %1%2").arg(projectLabel, dirtyMark));
}

bool MainWindow::maybePromptUnsavedChanges()
{
    return maybePromptSaveDocument(m_activeDocumentIndex);
}

bool MainWindow::maybePromptSaveDocument(int index)
{
    if (index < 0 || index >= static_cast<int>(m_documents.size()))
        return true;
    if (index == m_activeDocumentIndex)
        saveActiveDocumentState();

    ImageDocument& document = m_documents[static_cast<size_t>(index)];
    if (!document.dirty) return true;
    const QString title = documentTitle(document);

    const auto reply = QMessageBox::question(
        this, tr("Unsaved Changes"),
        tr("\"%1\" has unsaved changes. Save before closing?").arg(title),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Save);

    if (reply == QMessageBox::Save) {
        if (index != m_activeDocumentIndex)
            setActiveDocumentIndex(index);
        if (m_currentProjectPath.isEmpty()) {
            // No path yet — Save As. If the user cancels the file dialog,
            // saveProjectToPath never runs, and m_projectDirty stays true.
            // That's our signal to treat the whole flow as cancelled.
            onSaveProjectAs();
            return !m_projectDirty;
        }
        return saveProjectToPath(m_currentProjectPath);
    }
    if (reply == QMessageBox::Discard) {
        document.dirty = false;
        if (index == m_activeDocumentIndex) {
            if (m_debounce) m_debounce->stop();
            m_projectDirty = false;
            saveActiveDocumentState();
            updateWindowTitle();
        }
        updateDocumentTabs();
        return true;
    }
    return false;   // Cancel
}

bool MainWindow::maybePromptAllUnsavedDocuments()
{
    saveActiveDocumentState();
    for (int i = 0; i < static_cast<int>(m_documents.size()); ++i) {
        if (!m_documents[static_cast<size_t>(i)].dirty)
            continue;
        if (!maybePromptSaveDocument(i))
            return false;
    }
    return true;
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (maybePromptAllUnsavedDocuments()) {
        event->accept();
    } else {
        event->ignore();
    }
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
    if (!firstSupportedDroppedImage(event->mimeData()).isEmpty()) {
        event->acceptProposedAction();
        return;
    }
    QMainWindow::dragEnterEvent(event);
}

void MainWindow::dropEvent(QDropEvent* event)
{
    const QString path = firstSupportedDroppedImage(event->mimeData());
    if (path.isEmpty()) {
        QMainWindow::dropEvent(event);
        return;
    }

    event->acceptProposedAction();
    loadImageFromPath(path);
}

// ==============================================================================
// Debounce + worker render
// ==============================================================================
void MainWindow::onDebounceFired()
{
    // The debounce timer is the central funnel: every interactive edit path
    // (slider drag, curve edit, HSL slider, undo/redo) restarts it. So
    // "debounce just fired" means "the user (or undo/redo) recently
    // mutated m_look." That's the right place to mark dirty — one hook,
    // every edit path covered, no per-control bookkeeping.
    //
    // Suppressed during project load: applyLookToUi runs as part of the
    // load and its render kick would otherwise dirty the freshly-loaded
    // project.
    if (!m_isLoadingProject) {
        markDirty();
        updateCurrentHistorySnapshot();
    }
    requestRender();
}

void MainWindow::requestRender()
{
    if (m_previewSource.isNull()) return;

    ++m_generation;

    // If a worker is already in flight, let it finish and then re-kick. This
    // is what keeps the UI responsive under sustained slider drags — we
    // never queue up multiple pending renders.
    if (m_renderInFlight) {
        m_pendingRender = true;
        return;
    }

    const quint64 thisGen = m_generation;
    const QImage  source  = m_previewSource;   // copy-on-write: O(1)
    const lps::Look look  = m_look;            // by-value snapshot

    m_renderInFlight = true;

    auto* watcher = new QFutureWatcher<QImage>(this);
    connect(watcher, &QFutureWatcher<QImage>::finished, this,
            [this, watcher, thisGen]() {
        const QImage result = watcher->result();
        watcher->deleteLater();
        onRenderFinished(thisGen, result);
    });

    QFuture<QImage> fut = QtConcurrent::run(
        [source, look]() { return renderOnWorker(source, look); });
    watcher->setFuture(fut);
}

QImage MainWindow::renderOnWorker(QImage source, lps::Look look)
{
    // Pure function on the worker thread. No member access, no shared state.
    lps::ImagePipeline pipeline;
    lps::RenderResult r = pipeline.render(source, look);
    return r.image;
}

void MainWindow::onRenderFinished(quint64 generation, QImage result)
{
    m_renderInFlight = false;

    // Stale result? Discard and re-kick if something newer is waiting.
    if (generation != m_generation || m_pendingRender) {
        m_pendingRender = false;
        requestRender();
        return;
    }

    m_processed = std::move(result);
    refreshPreviewLabel();

    // Live histogram update: feed the freshly-rendered preview to the
    // panel widget. Computing 4×256 bins from a subsampled image is cheap
    // (~3 ms at the cap of 250K samples) so doing this synchronously on
    // the UI thread per render is fine.
    if (m_histogramWidget) m_histogramWidget->setImage(m_processed);
    updateNavigatorPreview();

    // Mirror to the secondary viewer if it's open. The viewer holds an
    // implicitly-shared copy of the QImage — no per-pixel work, just a
    // refcount bump. setImage() repaints if the image actually changed.
    if (m_secondaryViewer) m_secondaryViewer->setImage(m_processed);
    saveActiveDocumentState();
}

// ==============================================================================
// Display
// ==============================================================================
void MainWindow::refreshPreviewLabel()
{
    if (!m_previewLabel) return;

    // Push the cached source/processed images into the PreviewWidget.
    // The widget owns its own copies and decides which one to display
    // based on the showOriginal flag. Both setters early-out when the
    // image is unchanged (cacheKey equality), so it's safe to call this
    // every refresh.
    m_previewLabel->setOriginalImage(m_previewSource);
    m_previewLabel->setEditedImage(m_processed);
    m_previewLabel->setShowOriginal(m_showOriginal);
}

void MainWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);
    // The preview widget handles its own resize internally — Fit mode
    // recomputes the fit scale on every paint, free mode clamps the pan
    // offset on its own resizeEvent. Nothing for MainWindow to do here.
}

// ==============================================================================
// Press-and-hold Before/After preview
//
// Spacebar pressed  → show the unedited m_previewSource
// Spacebar released → show the edited m_processed
//
// Auto-repeat events are ignored so holding the key doesn't ping-pong the
// state every repeat. If no image is loaded yet (m_previewSource is null),
// the gesture is a no-op — we return without mutating state so the readout
// doesn't flip to "Original" with nothing to show.
// ==============================================================================
void MainWindow::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        if (m_previewSource.isNull()) {
            // No image loaded — swallow the key but don't change state.
            // Swallowing (accept + return) prevents QMainWindow from doing
            // anything default with Space.
            event->accept();
            return;
        }
        // If the menu toggle has explicitly stuck the view to "original",
        // the press-and-hold gesture has nothing to do — we'd otherwise
        // toggle off on release and defeat the menu's intent.
        if (m_actBeforeAfter && m_actBeforeAfter->isChecked()) {
            event->accept();
            return;
        }
        m_showOriginal = true;
        if (m_viewModeLabel) m_viewModeLabel->setText(tr("Original"));
        refreshPreviewLabel();
        event->accept();
        return;
    }
    QMainWindow::keyPressEvent(event);
}

void MainWindow::keyReleaseEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        if (m_previewSource.isNull()) {
            event->accept();
            return;
        }
        // Mirror the press-handler guard.
        if (m_actBeforeAfter && m_actBeforeAfter->isChecked()) {
            event->accept();
            return;
        }
        m_showOriginal = false;
        if (m_viewModeLabel) m_viewModeLabel->setText(tr("Edited"));
        refreshPreviewLabel();
        event->accept();
        return;
    }
    QMainWindow::keyReleaseEvent(event);
}

// ------------------------------------------------------------------------------
// eventFilter
//
// Sliders are focusable — during drags they hold keyboard focus, so a Space
// press would normally go to the slider (whose default handler does nothing
// useful here) instead of reaching MainWindow::keyPressEvent.
//
// We install this filter on QApplication so EVERY key event in the app is
// inspected first. If it's a Space press/release that isn't an auto-repeat,
// we dispatch it to our own handlers and tell Qt we've consumed it. Other
// key events fall through to their normal targets.
//
// Important: we filter on press AND release. If we only filtered on press,
// a slider with focus during the release would see the Space release as a
// stray event (harmless, but bad hygiene — and the user's key-up wouldn't
// trigger our restore-to-edited behavior).
//
// Note: this filter consumes Space globally. The current UI has no text
// input widgets, so there's no collision. If a QLineEdit or QTextEdit is
// added later (e.g., a preset-name field), this filter should first check
// whether `watched` is such a widget and pass through in that case:
//     if (qobject_cast<QLineEdit*>(watched) || qobject_cast<QTextEdit*>(watched))
//         return QMainWindow::eventFilter(watched, event);
// ------------------------------------------------------------------------------
bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
    const QEvent::Type t = event->type();
    if (t == QEvent::KeyPress || t == QEvent::KeyRelease) {
        auto* keyEv = static_cast<QKeyEvent*>(event);
        if (keyEv->key() == Qt::Key_Space && !keyEv->isAutoRepeat()) {
            if (t == QEvent::KeyPress)
                keyPressEvent(keyEv);
            else
                keyReleaseEvent(keyEv);
            return true;   // event consumed
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

// ==============================================================================
// Menu construction
//
// All top-level menus + their actions in one place. We build QAction
// instances once and connect them to slots. Actions referenced after
// construction (Undo/Redo enable-state, panel toggles, etc.) are stored
// as members; one-shot actions are local.
//
// Convention for placeholders: stub actions trigger a small "Not yet
// implemented" dialog rather than silently doing nothing. Users get a
// clear signal that the action exists but isn't wired up yet.
// ==============================================================================
namespace {

// Central placeholder helper — keeps the dialog text consistent across
// stub actions and makes it easy to replace later.
inline void showPlaceholder(QWidget* parent, const QString& title)
{
    QMessageBox::information(parent, title,
        QObject::tr("This feature isn't implemented yet."));
}

} // namespace

void MainWindow::buildMenus()
{
    // ---- File menu --------------------------------------------------------
    {
        auto* m = menuBar()->addMenu(tr("&File"));

        m_actOpenImage = m->addAction(tr("Open &Image..."));
        m_actOpenImage->setShortcut(QKeySequence(QStringLiteral("Ctrl+I")));
        connect(m_actOpenImage, &QAction::triggered, this, &MainWindow::onOpenImage);

        auto* openProj = m->addAction(tr("&Open Project..."));
        openProj->setShortcut(QKeySequence::Open);
        connect(openProj, &QAction::triggered, this, &MainWindow::onOpenProject);

        auto* saveProj = m->addAction(tr("&Save Project"));
        saveProj->setShortcut(QKeySequence::Save);
        connect(saveProj, &QAction::triggered, this, &MainWindow::onSaveProject);

        auto* saveProjAs = m->addAction(tr("Save Project &As..."));
        saveProjAs->setShortcut(QKeySequence::SaveAs);
        connect(saveProjAs, &QAction::triggered, this, &MainWindow::onSaveProjectAs);

        m->addSeparator();

        m_actExportImage = m->addAction(tr("&Export Image..."));
        m_actExportImage->setShortcut(QKeySequence(QStringLiteral("Ctrl+E")));
        connect(m_actExportImage, &QAction::triggered, this, &MainWindow::onExportImage);

        m->addSeparator();

        auto* savePreset = m->addAction(tr("Save &Preset..."));
        connect(savePreset, &QAction::triggered, this, &MainWindow::onSavePreset);
        auto* loadPreset = m->addAction(tr("&Load Preset..."));
        connect(loadPreset, &QAction::triggered, this, &MainWindow::onLoadPreset);

        m->addSeparator();

        auto* quitAction = m->addAction(tr("E&xit"));
        quitAction->setShortcut(QKeySequence::Quit);
        // close() routes through closeEvent → unsaved-changes prompt.
        connect(quitAction, &QAction::triggered, this, &QWidget::close);
    }

    // ---- Edit menu --------------------------------------------------------
    {
        auto* m = menuBar()->addMenu(tr("&Edit"));

        m_actUndo = m->addAction(tr("&Undo"));
        m_actUndo->setShortcut(QKeySequence::Undo);
        m_actUndo->setEnabled(false);   // empty stack at startup
        connect(m_actUndo, &QAction::triggered, this, &MainWindow::undo);

        m_actRedo = m->addAction(tr("&Redo"));
        m_actRedo->setShortcut(QKeySequence::Redo);
        m_actRedo->setEnabled(false);
        connect(m_actRedo, &QAction::triggered, this, &MainWindow::redo);

        m->addSeparator();

        m_actResetEdits = m->addAction(tr("Reset Edits"));
        connect(m_actResetEdits, &QAction::triggered, this, &MainWindow::onResetEdits);

        m->addSeparator();

        m_actRotateLeft = m->addAction(tr("Rotate &Left"));
        m_actRotateLeft->setShortcut(QKeySequence(QStringLiteral("Ctrl+[")));
        connect(m_actRotateLeft, &QAction::triggered, this, &MainWindow::onRotateLeft);

        m_actRotateRight = m->addAction(tr("Rotate &Right"));
        m_actRotateRight->setShortcut(QKeySequence(QStringLiteral("Ctrl+]")));
        connect(m_actRotateRight, &QAction::triggered, this, &MainWindow::onRotateRight);

        m_actFlipHorizontal = m->addAction(tr("Flip &Horizontal"));
        connect(m_actFlipHorizontal, &QAction::triggered,
                this, &MainWindow::onFlipHorizontal);

        m_actFlipVertical = m->addAction(tr("Flip &Vertical"));
        connect(m_actFlipVertical, &QAction::triggered,
                this, &MainWindow::onFlipVertical);

        m->addSeparator();

        m_actCopyLook = m->addAction(tr("&Copy Look"));
        m_actCopyLook->setShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+C")));
        connect(m_actCopyLook, &QAction::triggered, this, &MainWindow::onCopyLook);

        m_actPasteLook = m->addAction(tr("&Paste Look"));
        m_actPasteLook->setShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+V")));
        connect(m_actPasteLook, &QAction::triggered, this, &MainWindow::onPasteLook);

        m->addSeparator();

        auto* prefs = m->addAction(tr("Pre&ferences..."));
        connect(prefs, &QAction::triggered, this, &MainWindow::onPreferences);
    }

    // ---- View menu --------------------------------------------------------
    {
        auto* m = menuBar()->addMenu(tr("&View"));

        m_actZoomFit = m->addAction(tr("&Fit to Screen"));
        m_actZoomFit->setShortcut(QKeySequence(QStringLiteral("Ctrl+0")));
        connect(m_actZoomFit, &QAction::triggered, this, &MainWindow::onZoomFit);

        m_actZoom100 = m->addAction(tr("&100%"));
        m_actZoom100->setShortcut(QKeySequence(QStringLiteral("Ctrl+1")));
        connect(m_actZoom100, &QAction::triggered, this, &MainWindow::onZoom100);

        m_actZoomIn = m->addAction(tr("Zoom &In"));
        m_actZoomIn->setShortcut(QKeySequence::ZoomIn);
        connect(m_actZoomIn, &QAction::triggered, this, &MainWindow::onZoomIn);

        m_actZoomOut = m->addAction(tr("Zoom &Out"));
        m_actZoomOut->setShortcut(QKeySequence::ZoomOut);
        connect(m_actZoomOut, &QAction::triggered, this, &MainWindow::onZoomOut);

        m->addSeparator();

        // Sticky toggle (distinct from press-and-hold Spacebar). When
        // checked, displays the unedited image until unchecked.
        m_actBeforeAfter = m->addAction(tr("&Before / After"));
        m_actBeforeAfter->setCheckable(true);
        m_actBeforeAfter->setShortcut(QKeySequence(QStringLiteral("Backslash")));
        connect(m_actBeforeAfter, &QAction::triggered,
                this, &MainWindow::onToggleBeforeAfter);

        m->addSeparator();

        // Histogram + Controls toggles. Both are checkable; their state
        // is synced with the actual widget visibility in the slot bodies.
        m_actShowHistogram = m->addAction(tr("Show &Histogram"));
        m_actShowHistogram->setCheckable(true);
        m_actShowHistogram->setChecked(true);
        connect(m_actShowHistogram, &QAction::toggled, this, [this](bool on) {
            if (on) setAnalysisPanelCollapsed(false);
            if (m_histogramWidget) m_histogramWidget->setVisible(on);
        });

        m_actShowControls = m->addAction(tr("Show &Controls Panel"));
        m_actShowControls->setCheckable(true);
        m_actShowControls->setChecked(true);
        connect(m_actShowControls, &QAction::toggled, this, [this](bool on) {
            // Map "show" → expanded, "hide" → collapsed (instead of fully
            // hiding, which would lose the spatial cue that the panel
            // exists). Same toggle mechanic as the chevron buttons.
            const bool wantCollapsed = !on;
            if (wantCollapsed != m_sidebarCollapsed) onToggleSidebar();
        });

        m_actShowBottomWorkspace = m->addAction(tr("Show &Bottom Workspace"));
        m_actShowBottomWorkspace->setCheckable(true);
        m_actShowBottomWorkspace->setChecked(true);
        connect(m_actShowBottomWorkspace, &QAction::toggled, this, [this](bool on) {
            m_bottomWorkspaceEnabled = on;
            if (m_settings) m_settings->setBottomWorkspaceVisible(on);
            updateBottomWorkspaceVisibility();
        });

        m->addSeparator();

        auto* secondary = m->addAction(tr("Secondary Viewer..."));
        connect(secondary, &QAction::triggered, this, &MainWindow::onSecondaryViewer);

        m_actFullscreen = m->addAction(tr("F&ullscreen"));
        m_actFullscreen->setCheckable(true);
        m_actFullscreen->setShortcut(QKeySequence(QStringLiteral("F11")));
        connect(m_actFullscreen, &QAction::triggered,
                this, &MainWindow::onToggleFullscreen);
    }

    // ---- Window menu ------------------------------------------------------
    {
        auto* m = menuBar()->addMenu(tr("&Window"));

        auto* resetWS = m->addAction(tr("&Reset Workspace Layout"));
        connect(resetWS, &QAction::triggered,
                this, &MainWindow::onResetWorkspaceLayout);

        auto* saveWS = m->addAction(tr("&Save Workspace Layout"));
        connect(saveWS, &QAction::triggered,
                this, &MainWindow::onSaveWorkspaceLayout);

        m->addSeparator();

        // Panel-toggle actions. The histogram is real (toggles widget
        // visibility); the others are placeholders since those panels
        // don't exist as separable docks yet.
        auto* histPanel = m->addAction(tr("Histogram Panel"));
        histPanel->setCheckable(true);
        histPanel->setChecked(true);
        connect(histPanel, &QAction::toggled, this, [this](bool on) {
            if (on) setAnalysisPanelCollapsed(false);
            if (m_histogramWidget) m_histogramWidget->setVisible(on);
            if (m_actShowHistogram) m_actShowHistogram->setChecked(on);
        });

        auto* curvesPanel = m->addAction(tr("Curves Panel"));
        connect(curvesPanel, &QAction::triggered,
                this, [this]() { showPlaceholder(this, tr("Curves Panel")); });

        auto* colorPanel = m->addAction(tr("Color Panel"));
        connect(colorPanel, &QAction::triggered,
                this, [this]() { showPlaceholder(this, tr("Color Panel")); });

        auto* presetsPanel = m->addAction(tr("Presets Panel"));
        connect(presetsPanel, &QAction::triggered,
                this, [this]() { showPlaceholder(this, tr("Presets Panel")); });

        auto* nodeGraph = m->addAction(tr("Node Graph..."));
        connect(nodeGraph, &QAction::triggered,
                this, &MainWindow::onShowNodeGraph);
    }

    // ---- Plugins menu -----------------------------------------------------
    {
        auto* m = menuBar()->addMenu(tr("&Plugins"));

        auto* mgr = m->addAction(tr("Plugin &Manager..."));
        connect(mgr, &QAction::triggered, this, &MainWindow::onPluginManager);

        auto* install = m->addAction(tr("&Install Plugin..."));
        connect(install, &QAction::triggered, this, &MainWindow::onInstallPlugin);

        auto* reload = m->addAction(tr("&Reload Plugins"));
        connect(reload, &QAction::triggered, this, &MainWindow::onReloadPlugins);

        m->addSeparator();

        auto* openFolder = m->addAction(tr("&Open Plugins Folder"));
        connect(openFolder, &QAction::triggered,
                this, &MainWindow::onOpenPluginsFolder);
    }

    // ---- Help menu --------------------------------------------------------
    {
        auto* m = menuBar()->addMenu(tr("&Help"));

        auto* shortcuts = m->addAction(tr("&Keyboard Shortcuts"));
        connect(shortcuts, &QAction::triggered,
                this, &MainWindow::onShowKeyboardShortcuts);

        auto* docs = m->addAction(tr("&Documentation"));
        connect(docs, &QAction::triggered,
                this, &MainWindow::onShowDocumentation);

        m->addSeparator();

        auto* about = m->addAction(tr("&About Lumen Photo Studio"));
        connect(about, &QAction::triggered, this, &MainWindow::onAbout);
    }
}

// ==============================================================================
// Undo / Redo enable-state sync
//
// Called after every operation that mutates the stacks. Keeps the menu
// items disabled when there's nothing to do, so users see at a glance
// whether undo/redo is available.
// ==============================================================================
void MainWindow::refreshUndoRedoActions()
{
    if (m_actUndo) m_actUndo->setEnabled(!m_undoStack.empty());
    if (m_actRedo) m_actRedo->setEnabled(!m_redoStack.empty());
}

// ==============================================================================
// Preset I/O
//
// Save Preset writes a JSON file containing just the Look (no source path,
// no project metadata). Useful for sharing edit recipes between projects.
// Load Preset replaces the current Look with the file's Look and applies
// it through the existing applyLookToUi path so the UI updates and a
// render kicks. Marks dirty (since the project differs from disk).
// ==============================================================================
void MainWindow::onSavePreset()
{
    const QString picturesDir =
        QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Save Preset"),
        picturesDir + QStringLiteral("/preset.lxp"),
        tr("Lumen Presets (*.lxp);;JSON (*.json)"));
    if (path.isEmpty()) return;

    // Auto-append .lxp if the user supplied no recognized extension. Some
    // platform file dialogs don't add the filter's extension automatically.
    QString finalPath = path;
    const bool hasKnownExt =
        finalPath.endsWith(QStringLiteral(".lxp"),     Qt::CaseInsensitive) ||
        finalPath.endsWith(QStringLiteral(".lpreset"), Qt::CaseInsensitive) ||
        finalPath.endsWith(QStringLiteral(".json"),    Qt::CaseInsensitive);
    if (!hasKnownExt) finalPath += QStringLiteral(".lxp");

    const QJsonDocument doc(lps::LookSerializer::toJson(m_look));
    QFile f(finalPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, tr("Save Preset"),
                             tr("Could not write: %1").arg(finalPath));
        return;
    }
    f.write(doc.toJson(QJsonDocument::Indented));
    f.close();
}

void MainWindow::onLoadPreset()
{
    const QString picturesDir =
        QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    // Accept both the new .lxp extension and the legacy .lpreset so older
    // files keep working. JSON is also allowed for hand-edited presets.
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Load Preset"), picturesDir,
        tr("Lumen Presets (*.lxp *.lpreset *.json);;All Files (*)"));
    if (path.isEmpty()) return;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("Load Preset"),
                             tr("Could not read: %1").arg(path));
        return;
    }
    const QByteArray bytes = f.readAll();
    f.close();

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &err);
    if (doc.isNull() || !doc.isObject()) {
        QMessageBox::warning(this, tr("Load Preset"),
                             tr("Invalid preset file:\n%1").arg(err.errorString()));
        return;
    }

    lps::Look loaded;
    QString lookErr;
    if (!lps::LookSerializer::fromJson(doc.object(), loaded, &lookErr)) {
        QMessageBox::warning(this, tr("Load Preset"),
                             tr("Could not parse Look:\n%1").arg(lookErr));
        return;
    }

    // Spec rule: presets do NOT change the loaded original image — they
    // only replace the Look. Push undo BEFORE the mutation so the user
    // can revert. The image, project path, and dirty state are not part
    // of a preset; we leave m_currentImagePath / m_currentProjectPath
    // alone here. applyLookToUi triggers the debounce, which renders.
    m_nextHistoryLabel = tr("Preset loaded");
    pushUndoSnapshot();
    m_look = loaded;
    applyLookToUi();
    refreshUndoRedoActions();

    // Update the small status label so users can see which preset is
    // currently applied. This is purely informational — we don't track
    // "the active preset" as a piece of project state.
    if (m_presetNameLabel)
        m_presetNameLabel->setText(QFileInfo(path).fileName());
}

// ==============================================================================
// LUT (color grading) slots
//
// The engine's ColorGrading stage already supports loading a .cube LUT and
// blending it via lutOpacity. These slots only mutate the Look fields and
// kick the standard render path; the engine does the actual cube parsing
// and trilinear interpolation.
//
// Both slots push an undo snapshot before mutating, so users can revert
// LUT load/clear via Ctrl+Z. They mark dirty and start the debounce, the
// same as a slider drag.
// ==============================================================================
void MainWindow::onLoadLut()
{
    // Default the dialog to the user's Pictures dir, the same as Open
    // Image. Many users keep their LUT collection there.
    const QString picturesDir =
        QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Load LUT"), picturesDir,
        tr("LUT Files (*.cube)"));
    if (path.isEmpty()) return;

    m_nextHistoryLabel = tr("LUT loaded");
    pushUndoSnapshot();
    m_look.grading.lutPath = path;
    // Spec rule: bump opacity to 1.0 if it was 0 — newly-loaded LUTs
    // should be visible immediately. Mid-mix values (e.g. 0.5) are
    // preserved so users can keep their working blend ratio.
    if (m_look.grading.lutOpacity <= 0.0f) m_look.grading.lutOpacity = 1.0f;

    refreshLutWidgets();
    refreshUndoRedoActions();
    markDirty();
    if (m_debounce) m_debounce->start();
}

void MainWindow::onClearLut()
{
    if (m_look.grading.lutPath.isEmpty()) return;   // nothing to clear

    m_nextHistoryLabel = tr("LUT cleared");
    pushUndoSnapshot();
    m_look.grading.lutPath.clear();
    m_look.grading.lutOpacity = 1.0f;   // reset to default

    refreshLutWidgets();
    refreshUndoRedoActions();
    markDirty();
    if (m_debounce) m_debounce->start();
}

// Refresh the LUT widgets from m_look.grading. Called from applyLookToUi
// (so undo/redo/preset-load updates the display) and from the LUT slots
// themselves after mutating m_look.
//
// QSignalBlocker on the slider prevents this programmatic update from
// kicking the debounce a second time.
void MainWindow::refreshLutWidgets()
{
    if (!m_lutNameLabel || !m_lutOpacitySlider || !m_lutOpacityValue) return;

    const bool hasLut = !m_look.grading.lutPath.isEmpty();
    m_lutNameLabel->setText(hasLut
        ? QFileInfo(m_look.grading.lutPath).fileName()
        : tr("(none)"));

    const int sliderValue = static_cast<int>(
        std::lround(m_look.grading.lutOpacity * 100.0f));
    {
        QSignalBlocker block(m_lutOpacitySlider);
        m_lutOpacitySlider->setValue(sliderValue);
    }
    m_lutOpacityValue->setText(QString::number(sliderValue));

    // Sync the enabled checkbox. Stays interactive only when a LUT is
    // actually loaded — otherwise toggling it does nothing useful.
    if (m_lutEnabledCheck) {
        QSignalBlocker block(m_lutEnabledCheck);
        m_lutEnabledCheck->setChecked(m_look.grading.lutEnabled);
        m_lutEnabledCheck->setEnabled(hasLut);
    }

    // Disable the opacity slider and Clear button when no LUT is loaded —
    // they have nothing to act on. The Load LUT button stays enabled.
    // When the LUT is loaded but disabled via checkbox, the opacity
    // slider stays editable so users can pre-adjust before re-enabling.
    m_lutOpacitySlider->setEnabled(hasLut);
    if (m_lutClearBtn) m_lutClearBtn->setEnabled(hasLut);
}

// ==============================================================================
// HDR tone mapping - refresh
// ==============================================================================
void MainWindow::refreshHdrWidgets()
{
    const auto& h = m_look.hdr;

    if (m_hdrEnabledCheck) {
        QSignalBlocker b(m_hdrEnabledCheck);
        m_hdrEnabledCheck->setChecked(h.enabled);
    }

    auto setSlider = [&](QSlider* s, QLabel* lbl,
                         float value, float scale, int decimals) {
        if (!s) return;
        const int iv = static_cast<int>(std::lround(value * scale));
        QSignalBlocker b(s);
        s->setValue(iv);
        s->setEnabled(h.enabled);
        if (lbl) lbl->setText(QString::number(value, 'f', decimals));
    };

    setSlider(m_hdrExposureBiasSlider, m_hdrExposureBiasValue,
              h.exposureBias, 100.0f, 2);
    setSlider(m_hdrHighlightCompressionSlider, m_hdrHighlightCompressionValue,
              h.highlightCompression, 1.0f, 0);
    setSlider(m_hdrShoulderStrengthSlider, m_hdrShoulderStrengthValue,
              h.shoulderStrength, 1.0f, 0);
    setSlider(m_hdrMidtonePivotSlider, m_hdrMidtonePivotValue,
              h.midtonePivot, 100.0f, 2);
    setSlider(m_hdrSaturationPreserveSlider, m_hdrSaturationPreserveValue,
              h.saturationPreserve, 1.0f, 0);
}

// ==============================================================================
// Lens correction — refresh
//
// Sync all lens widgets from m_look.lens. Master-enable gates the per-
// control widgets: when off, sliders / CA checkbox are visually disabled
// (their values still persist in the Look — this is a UI cue, not a data
// reset). The master checkbox itself is always editable.
// ==============================================================================
void MainWindow::refreshLensWidgets()
{
    const auto& lp = m_look.lens;

    if (m_lensEnabledCheck) {
        QSignalBlocker b(m_lensEnabledCheck);
        m_lensEnabledCheck->setChecked(lp.enabled);
    }
    if (m_lensRemoveCaCheck) {
        QSignalBlocker b(m_lensRemoveCaCheck);
        m_lensRemoveCaCheck->setChecked(lp.removeChromaticAberration);
        m_lensRemoveCaCheck->setEnabled(lp.enabled);
    }

    auto setSlider = [&](QSlider* s, QLabel* lbl, float v) {
        if (!s) return;
        const int iv = static_cast<int>(std::lround(v));
        QSignalBlocker b(s);
        s->setValue(iv);
        s->setEnabled(lp.enabled);
        if (lbl) lbl->setText(QString::number(iv));
    };
    setSlider(m_lensDistortionSlider,   m_lensDistortionValue,   lp.distortion);
    setSlider(m_lensVignettingSlider,   m_lensVignettingValue,   lp.vignetting);
    setSlider(m_lensPurpleFringeSlider, m_lensPurpleFringeValue, lp.purpleFringe);
    setSlider(m_lensGreenFringeSlider,  m_lensGreenFringeValue,  lp.greenFringe);
}

// ==============================================================================
// Transform refresh
// ==============================================================================
void MainWindow::updateCropAspectConstraint()
{
    if (!m_previewLabel) return;

    double ratio = 0.0;
    const int index = m_cropAspectCombo ? m_cropAspectCombo->currentIndex() : 0;
    switch (index) {
    case 1:
        if (!m_previewSource.isNull() && m_previewSource.height() > 0) {
            ratio = static_cast<double>(m_previewSource.width())
                  / static_cast<double>(m_previewSource.height());
        }
        break;
    case 2: ratio = 1.0; break;
    case 3: ratio = 4.0 / 5.0; break;
    case 4: ratio = 5.0 / 4.0; break;
    case 5: ratio = 3.0 / 2.0; break;
    case 6: ratio = 2.0 / 3.0; break;
    case 7: ratio = 16.0 / 9.0; break;
    case 8: ratio = 9.0 / 16.0; break;
    default: break;
    }

    const bool locked = m_cropLockAspectCheck && m_cropLockAspectCheck->isChecked();
    m_previewLabel->setCropAspectRatio(ratio);
    m_previewLabel->setCropAspectRatioLocked(locked && ratio > 0.0);
}

void MainWindow::refreshTransformWidgets()
{
    const auto& tp = m_look.transform;

    if (m_previewLabel) {
        m_previewLabel->setCropRect(tp.cropRect);
        updateCropAspectConstraint();
    }
    if (m_cropToolBtn) {
        QSignalBlocker b(m_cropToolBtn);
        m_cropToolBtn->setChecked(m_previewLabel && m_previewLabel->isCropOverlayActive());
    }
    if (m_transformFlipHorizontalBtn) {
        QSignalBlocker b(m_transformFlipHorizontalBtn);
        m_transformFlipHorizontalBtn->setChecked(tp.flipHorizontal);
    }
    if (m_transformFlipVerticalBtn) {
        QSignalBlocker b(m_transformFlipVerticalBtn);
        m_transformFlipVerticalBtn->setChecked(tp.flipVertical);
    }
    if (m_straightenSlider) {
        QSignalBlocker b(m_straightenSlider);
        m_straightenSlider->setValue(
            static_cast<int>(std::lround(tp.straightenAngle * 10.0f)));
    }
    if (m_straightenValue)
        m_straightenValue->setText(QString::number(tp.straightenAngle, 'f', 1));
}

// ==============================================================================
// Details — refresh
// ==============================================================================
void MainWindow::refreshDetailsWidgets()
{
    const auto& d = m_look.details;

    auto setSlider = [](QSlider* s, QLabel* lbl,
                        float value, float scale, int decimals) {
        if (!s) return;
        const int iv = static_cast<int>(std::lround(value * scale));
        QSignalBlocker b(s);
        s->setValue(iv);
        if (lbl) lbl->setText(QString::number(value, 'f', decimals));
    };

    setSlider(m_sharpeningAmountSlider,  m_sharpeningAmountValue,
              d.sharpeningAmount, 1.0f, 0);
    setSlider(m_sharpeningRadiusSlider,  m_sharpeningRadiusValue,
              d.sharpeningRadius, 10.0f, 1);
    setSlider(m_sharpeningDetailSlider,  m_sharpeningDetailValue,
              d.sharpeningDetail, 1.0f, 0);
    setSlider(m_sharpeningMaskingSlider, m_sharpeningMaskingValue,
              d.sharpeningMasking, 1.0f, 0);
    setSlider(m_luminanceNrSlider,       m_luminanceNrValue,
              d.luminanceNR, 1.0f, 0);
    setSlider(m_luminanceDetailSlider,   m_luminanceDetailValue,
              d.luminanceDetail, 1.0f, 0);
    setSlider(m_colorNrSlider,           m_colorNrValue,
              d.colorNR, 1.0f, 0);
    setSlider(m_colorDetailSlider,       m_colorDetailValue,
              d.colorDetail, 1.0f, 0);
}

// ==============================================================================
// Local masks — slot implementations
//
// All mutations push an undo snapshot first. After mutating m_look, we
// always:
//   1. refreshMaskWidgets()      — sync UI to new state
//   2. refreshUndoRedoActions()  — sync menu enable-state
//   3. markDirty() + debounce    — kick a render
//
// The selected-mask index is the source of truth for which mask the
// detail sliders edit. refreshMaskWidgets is responsible for clamping
// the index when masks are added/removed/reordered.
// ==============================================================================
void MainWindow::addMaskCommon(lps::LocalAdjustment&& mask, const QString& kind)
{
    m_nextHistoryLabel = tr("%1 mask added").arg(kind);
    pushUndoSnapshot();

    // Default name pattern: "Linear 1", "Radial 2", etc. — index by total
    // count so users can tell masks apart at a glance. They can rename
    // later (rename UI not in this step but the data field exists).
    if (mask.name.isEmpty()) {
        mask.name = QStringLiteral("%1 %2")
            .arg(kind)
            .arg(m_look.localAdjustments.size() + 1);
    }
    m_look.localAdjustments.push_back(std::move(mask));

    // Select the newly-added mask so the user can immediately adjust it.
    m_selectedMaskIndex = static_cast<int>(m_look.localAdjustments.size()) - 1;

    refreshMaskWidgets();
    refreshUndoRedoActions();
    markDirty();
    if (m_debounce) m_debounce->start();
}

void MainWindow::onAddLinearMask()
{
    lps::LocalAdjustment m;
    m.type = lps::MaskType::LinearGradient;
    // Default: top-of-frame downward — like Lightroom's "darken sky" gradient.
    m.startPoint = QPointF(0.5, 0.0);
    m.endPoint   = QPointF(0.5, 0.4);
    m.feather    = 0.5f;
    addMaskCommon(std::move(m), tr("Linear"));
}

void MainWindow::onAddRadialMask()
{
    lps::LocalAdjustment m;
    m.type = lps::MaskType::RadialGradient;
    // Default: middle of frame, quarter-image radius.
    m.center  = QPointF(0.5, 0.5);
    m.radius  = 0.25f;
    m.feather = 0.5f;
    addMaskCommon(std::move(m), tr("Radial"));
}

void MainWindow::onAddBrushMask()
{
    lps::LocalAdjustment m;
    m.type = lps::MaskType::Brush;
    m.brushSize = 0.08f;
    m.feather   = 0.5f;
    m.flow      = 0.5f;
    m.density   = 1.0f;
    // Brush is a placeholder — V1 LocalAdjustmentEngine treats brush masks
    // as zero-weight everywhere, so this entry is inert until the brush
    // UI lands. The data round-trips through save/load correctly, which
    // is the spec's first-version requirement.
    addMaskCommon(std::move(m), tr("Brush"));
}

void MainWindow::onDeleteSelectedMask()
{
    if (m_selectedMaskIndex < 0 ||
        m_selectedMaskIndex >= static_cast<int>(m_look.localAdjustments.size())) {
        return;
    }

    m_nextHistoryLabel = tr("Mask deleted");
    pushUndoSnapshot();

    m_look.localAdjustments.erase(
        m_look.localAdjustments.begin() + m_selectedMaskIndex);

    // Adjust selection: stay at the same row index if possible (selecting
    // the mask that was below the deleted one), or move up if we deleted
    // the last mask. -1 if no masks remain.
    const int total = static_cast<int>(m_look.localAdjustments.size());
    if (total == 0) {
        m_selectedMaskIndex = -1;
    } else if (m_selectedMaskIndex >= total) {
        m_selectedMaskIndex = total - 1;
    }

    refreshMaskWidgets();
    refreshUndoRedoActions();
    markDirty();
    if (m_debounce) m_debounce->start();
}

void MainWindow::onMaskListSelectionChanged()
{
    if (!m_maskList) return;
    const int row = m_maskList->currentRow();
    if (row == m_selectedMaskIndex) return;
    m_selectedMaskIndex = row;
    // Refresh the per-mask sliders for the new selection. No undo push —
    // selection change isn't an edit. No render kick either.
    refreshMaskWidgets();
}

void MainWindow::onMaskItemChanged(QListWidgetItem* item)
{
    // Triggered when the user toggles the per-row checkbox. Map the
    // checkbox state into the mask's enabled flag. Push undo so the
    // toggle is reversible; signal-blocked refresh prevents loops.
    if (!m_maskList || !item) return;
    const int row = m_maskList->row(item);
    if (row < 0 || row >= static_cast<int>(m_look.localAdjustments.size())) return;

    const bool checked = (item->checkState() == Qt::Checked);
    if (m_look.localAdjustments[row].enabled == checked) return;

    m_nextHistoryLabel = tr("Mask visibility changed");
    pushUndoSnapshot();
    m_look.localAdjustments[row].enabled = checked;
    refreshUndoRedoActions();
    markDirty();
    if (m_debounce) m_debounce->start();
}

// Rebuild m_maskList rows from m_look.localAdjustments and refresh the
// per-mask sliders from the currently-selected entry. Signal-blocked
// internally so it doesn't kick the debounce or re-fire the selection-
// changed slot.
//
// Called from:
//   - applyLookToUi (undo/redo, project load, preset load)
//   - the add/delete slots
//   - construction (initial state)
void MainWindow::refreshMaskWidgets()
{
    if (!m_maskList) return;

    const int total = static_cast<int>(m_look.localAdjustments.size());

    // Clamp the selection if the list shrank (e.g. after undo/redo).
    if (m_selectedMaskIndex >= total) m_selectedMaskIndex = total - 1;
    if (m_selectedMaskIndex < 0 && total > 0) m_selectedMaskIndex = 0;

    // Rebuild the list rows. We tear down and recreate rather than
    // reconciling — masks don't typically have hundreds of entries, and
    // the simpler logic avoids stale-row-state bugs.
    {
        QSignalBlocker block(m_maskList);
        m_maskList->clear();
        for (int i = 0; i < total; ++i) {
            const auto& la = m_look.localAdjustments[i];
            QString typeStr;
            switch (la.type) {
                case lps::MaskType::LinearGradient: typeStr = tr("Linear");  break;
                case lps::MaskType::RadialGradient: typeStr = tr("Radial");  break;
                case lps::MaskType::Brush:          typeStr = tr("Brush");   break;
            }
            const QString display = la.name.isEmpty()
                ? QStringLiteral("(%1 %2)").arg(typeStr).arg(i + 1)
                : QStringLiteral("%1  —  %2").arg(la.name, typeStr);

            auto* item = new QListWidgetItem(display, m_maskList);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(la.enabled ? Qt::Checked : Qt::Unchecked);
        }
        if (m_selectedMaskIndex >= 0 && m_selectedMaskIndex < total) {
            m_maskList->setCurrentRow(m_selectedMaskIndex);
        }
    }

    // Sync the detail sliders + status label.
    const bool hasSelection = (m_selectedMaskIndex >= 0 &&
                                m_selectedMaskIndex < total);
    const bool brushSelection = hasSelection &&
        m_look.localAdjustments[m_selectedMaskIndex].type == lps::MaskType::Brush;
    if (m_maskDeleteBtn) m_maskDeleteBtn->setEnabled(hasSelection);

    auto setSlider = [](QSlider* s, QLabel* lbl, int v) {
        if (!s) return;
        QSignalBlocker b(s);
        s->setValue(v);
        if (lbl) lbl->setText(QString::number(v));
    };
    auto setEnabled = [hasSelection](QSlider* s) {
        if (s) s->setEnabled(hasSelection);
    };
    setEnabled(m_maskExposureSlider);
    setEnabled(m_maskBrightnessSlider);
    setEnabled(m_maskContrastSlider);
    setEnabled(m_maskSaturationSlider);
    setEnabled(m_maskTemperatureSlider);
    setEnabled(m_maskTintSlider);

    // Geometry controls follow the same enable rule.
    setEnabled(m_maskFeatherSlider);
    setEnabled(m_maskDensitySlider);
    setEnabled(m_maskFlowSlider);
    if (m_maskBrushSizeSlider) m_maskBrushSizeSlider->setEnabled(brushSelection);
    if (m_maskBrushEraseCheck) m_maskBrushEraseCheck->setEnabled(brushSelection);
    if (m_maskResetBrushBtn) m_maskResetBrushBtn->setEnabled(brushSelection);
    if (m_maskNameEdit)    m_maskNameEdit->setEnabled(hasSelection);
    if (m_maskInvertCheck) m_maskInvertCheck->setEnabled(hasSelection);
    if (m_maskResetGeoBtn) m_maskResetGeoBtn->setEnabled(hasSelection);

    if (hasSelection) {
        const auto& la = m_look.localAdjustments[m_selectedMaskIndex];
        setSlider(m_maskExposureSlider,    m_maskExposureValue,
                  static_cast<int>(std::lround(la.exposure * 100.0f)));
        setSlider(m_maskBrightnessSlider,  m_maskBrightnessValue,
                  static_cast<int>(std::lround(la.brightness)));
        setSlider(m_maskContrastSlider,    m_maskContrastValue,
                  static_cast<int>(std::lround(la.contrast)));
        setSlider(m_maskSaturationSlider,  m_maskSaturationValue,
                  static_cast<int>(std::lround(la.saturation)));
        setSlider(m_maskTemperatureSlider, m_maskTemperatureValue,
                  static_cast<int>(std::lround(la.temperature)));
        setSlider(m_maskTintSlider,        m_maskTintValue,
                  static_cast<int>(std::lround(la.tint)));

        // Geometry controls — feather/density/flow as 0..100 slider values.
        setSlider(m_maskFeatherSlider, m_maskFeatherValue,
                  static_cast<int>(std::lround(la.feather * 100.0f)));
        setSlider(m_maskDensitySlider, m_maskDensityValue,
                  static_cast<int>(std::lround(la.density * 100.0f)));
        setSlider(m_maskFlowSlider, m_maskFlowValue,
                  static_cast<int>(std::lround(la.flow * 100.0f)));
        setSlider(m_maskBrushSizeSlider, m_maskBrushSizeValue,
                  static_cast<int>(std::lround(la.brushSize * 1000.0f)));

        if (m_maskNameEdit) {
            QSignalBlocker b(m_maskNameEdit);
            m_maskNameEdit->setText(la.name);
        }
        if (m_maskInvertCheck) {
            QSignalBlocker b(m_maskInvertCheck);
            m_maskInvertCheck->setChecked(la.invert);
        }
        if (m_maskBrushEraseCheck) {
            QSignalBlocker b(m_maskBrushEraseCheck);
            m_maskBrushEraseCheck->setChecked(la.brushEraseMode);
        }

        if (m_maskStatusLabel) {
            QString status = tr("Selected: %1").arg(la.name.isEmpty()
                                                    ? tr("(unnamed)") : la.name);
            if (la.type == lps::MaskType::Brush) {
                status += tr(" - %1 stroke(s)").arg(
                    static_cast<int>(la.brushStrokes.size()));
            }
            m_maskStatusLabel->setText(status);
        }
    } else {
        // Zero out the sliders visually so an old selection's values
        // don't linger after the mask is deleted.
        setSlider(m_maskExposureSlider,    m_maskExposureValue,    0);
        setSlider(m_maskBrightnessSlider,  m_maskBrightnessValue,  0);
        setSlider(m_maskContrastSlider,    m_maskContrastValue,    0);
        setSlider(m_maskSaturationSlider,  m_maskSaturationValue,  0);
        setSlider(m_maskTemperatureSlider, m_maskTemperatureValue, 0);
        setSlider(m_maskTintSlider,        m_maskTintValue,        0);

        setSlider(m_maskFeatherSlider, m_maskFeatherValue, 50);
        setSlider(m_maskDensitySlider, m_maskDensityValue, 100);
        setSlider(m_maskFlowSlider,    m_maskFlowValue,    100);
        setSlider(m_maskBrushSizeSlider, m_maskBrushSizeValue, 80);
        if (m_maskNameEdit) {
            QSignalBlocker b(m_maskNameEdit);
            m_maskNameEdit->clear();
        }
        if (m_maskInvertCheck) {
            QSignalBlocker b(m_maskInvertCheck);
            m_maskInvertCheck->setChecked(false);
        }
        if (m_maskBrushEraseCheck) {
            QSignalBlocker b(m_maskBrushEraseCheck);
            m_maskBrushEraseCheck->setChecked(false);
        }

        if (m_maskStatusLabel) {
            m_maskStatusLabel->setText(total == 0
                ? tr("No masks") : tr("No mask selected"));
        }
    }

    // Push the active-mask pointer to PreviewWidget so the overlay and
    // handles update without needing a manual refresh hop.
    syncActiveMaskToPreview();
}

// ==============================================================================
// Mask geometry helpers
//
// syncActiveMaskToPreview hands the selected mask pointer to the preview
// so it can paint its overlay and handles. Pointer stability: m_look is
// a member variable; m_look.localAdjustments is a std::vector that may
// reallocate on push_back/erase, so this function is called after any
// mutation that could move the underlying storage.
//
// onMaskGeometryChangedFromPreview fires when the user drags a handle
// in the preview. We mark dirty + kick a render. The preview already
// invalidated its overlay cache and called update().
//
// onResetMaskGeometry returns the selected mask to its type-default
// geometry (matching the values onAddLinearMask/onAddRadialMask use).
// ==============================================================================
void MainWindow::syncActiveMaskToPreview()
{
    if (!m_previewLabel) return;
    if (m_selectedMaskIndex < 0 ||
        m_selectedMaskIndex >= static_cast<int>(m_look.localAdjustments.size())) {
        m_previewLabel->setActiveMask(nullptr);
        return;
    }
    m_previewLabel->setActiveMask(
        &m_look.localAdjustments[m_selectedMaskIndex]);
}

void MainWindow::onMaskGeometryChangedFromPreview()
{
    // The preview already updated its overlay cache and repainted. We
    // just need to refresh dependent UI (no slider values change from
    // a handle drag — geometry is the only thing affected) and kick
    // the render debounce so the masked adjustment renders into the
    // photo.
    refreshUndoRedoActions();
    markDirty();
    if (m_debounce) m_debounce->start();
}

void MainWindow::onResetBrushMask()
{
    if (m_selectedMaskIndex < 0 ||
        m_selectedMaskIndex >= static_cast<int>(m_look.localAdjustments.size())) {
        return;
    }
    auto& mask = m_look.localAdjustments[m_selectedMaskIndex];
    if (mask.type != lps::MaskType::Brush) return;

    m_nextHistoryLabel = tr("Brush mask reset");
    pushUndoSnapshot();
    mask.brushStrokes.clear();
    mask.brushEraseMode = false;
    if (m_previewLabel) m_previewLabel->setActiveMask(&mask);
    refreshMaskWidgets();
    refreshUndoRedoActions();
    markDirty();
    if (m_debounce) m_debounce->start();
}

void MainWindow::onResetMaskGeometry()
{
    if (m_selectedMaskIndex < 0 ||
        m_selectedMaskIndex >= static_cast<int>(m_look.localAdjustments.size())) {
        return;
    }
    m_nextHistoryLabel = tr("Mask geometry reset");
    pushUndoSnapshot();

    auto& mask = m_look.localAdjustments[m_selectedMaskIndex];
    switch (mask.type) {
    case lps::MaskType::LinearGradient:
        mask.startPoint = QPointF(0.5, 0.0);
        mask.endPoint   = QPointF(0.5, 0.4);
        mask.feather    = 0.5f;
        break;
    case lps::MaskType::RadialGradient:
        mask.center  = QPointF(0.5, 0.5);
        mask.radius  = 0.25f;
        mask.feather = 0.5f;
        break;
    case lps::MaskType::Brush:
        mask.brushStrokes.clear();
        mask.brushSize = 0.08f;
        mask.feather = 0.5f;
        break;
    }
    mask.invert  = false;
    mask.density = 1.0f;
    mask.flow    = 1.0f;

    refreshMaskWidgets();
    refreshUndoRedoActions();
    markDirty();
    if (m_debounce) m_debounce->start();
}

// ==============================================================================
// Adjustment layers — slot implementations
//
// Same pattern as masks: every mutation pushes an undo snapshot, then
// updates m_look, then refreshes the UI and kicks a render. Selection
// index is the source of truth for which layer the controls edit.
//
// Layer rendering itself is V1-placeholder — the pipeline does not yet
// composite layers on top of the base Look. Save/load + undo/redo of
// layer data work today so users can author layered projects that
// "wake up" once compositing lands.
// ==============================================================================
void MainWindow::onAddLayer()
{
    m_nextHistoryLabel = tr("Layer added");
    pushUndoSnapshot();

    lps::AdjustmentLayer layer;
    layer.name = QStringLiteral("Layer %1")
                     .arg(m_look.adjustmentLayers.size() + 1);
    // Default identity Look — user populates the layer's adjustments via
    // future layer-editing UI (out of scope for V1).
    m_look.adjustmentLayers.push_back(std::move(layer));
    m_selectedLayerIndex =
        static_cast<int>(m_look.adjustmentLayers.size()) - 1;

    refreshLayerWidgets();
    refreshUndoRedoActions();
    markDirty();
    if (m_debounce) m_debounce->start();
}

void MainWindow::onDuplicateLayer()
{
    if (m_selectedLayerIndex < 0 ||
        m_selectedLayerIndex >= static_cast<int>(m_look.adjustmentLayers.size())) {
        return;
    }

    m_nextHistoryLabel = tr("Layer duplicated");
    pushUndoSnapshot();

    // Deep copy: AdjustmentLayer is value-semantic (Look + scalars +
    // QString), so the default copy ctor produces a fully independent
    // duplicate. The duplicated layer sits immediately after the source.
    lps::AdjustmentLayer dup = m_look.adjustmentLayers[m_selectedLayerIndex];
    if (!dup.name.endsWith(tr(" copy"))) dup.name += tr(" copy");
    m_look.adjustmentLayers.insert(
        m_look.adjustmentLayers.begin() + m_selectedLayerIndex + 1,
        std::move(dup));
    m_selectedLayerIndex += 1;   // select the new duplicate

    refreshLayerWidgets();
    refreshUndoRedoActions();
    markDirty();
    if (m_debounce) m_debounce->start();
}

void MainWindow::onDeleteSelectedLayer()
{
    if (m_selectedLayerIndex < 0 ||
        m_selectedLayerIndex >= static_cast<int>(m_look.adjustmentLayers.size())) {
        return;
    }

    m_nextHistoryLabel = tr("Layer deleted");
    pushUndoSnapshot();

    m_look.adjustmentLayers.erase(
        m_look.adjustmentLayers.begin() + m_selectedLayerIndex);

    const int total = static_cast<int>(m_look.adjustmentLayers.size());
    if (total == 0) m_selectedLayerIndex = -1;
    else if (m_selectedLayerIndex >= total) m_selectedLayerIndex = total - 1;

    refreshLayerWidgets();
    refreshUndoRedoActions();
    markDirty();
    if (m_debounce) m_debounce->start();
}

void MainWindow::onLayerListSelectionChanged()
{
    if (!m_layerList) return;
    const int row = m_layerList->currentRow();
    if (row == m_selectedLayerIndex) return;
    m_selectedLayerIndex = row;
    refreshLayerWidgets();
}

void MainWindow::onLayerItemChanged(QListWidgetItem* item)
{
    if (!m_layerList || !item) return;
    const int row = m_layerList->row(item);
    if (row < 0 || row >= static_cast<int>(m_look.adjustmentLayers.size())) return;

    const bool checked = (item->checkState() == Qt::Checked);
    if (m_look.adjustmentLayers[row].enabled == checked) return;

    m_nextHistoryLabel = tr("Layer visibility changed");
    pushUndoSnapshot();
    m_look.adjustmentLayers[row].enabled = checked;
    refreshUndoRedoActions();
    markDirty();
    if (m_debounce) m_debounce->start();
}

void MainWindow::onLayerOpacityChanged(int v)
{
    if (m_selectedLayerIndex < 0 ||
        m_selectedLayerIndex >= static_cast<int>(m_look.adjustmentLayers.size())) {
        return;
    }
    m_look.adjustmentLayers[m_selectedLayerIndex].opacity =
        static_cast<float>(v) / 100.0f;
    if (m_layerOpacityValue) {
        m_layerOpacityValue->setText(QString::number(v));
    }
    if (m_debounce) m_debounce->start();
}

void MainWindow::onLayerBlendModeChanged(int comboIndex)
{
    if (m_selectedLayerIndex < 0 ||
        m_selectedLayerIndex >= static_cast<int>(m_look.adjustmentLayers.size())) {
        return;
    }
    if (comboIndex < 0 || comboIndex > 10) return;
    // Combo selection: push undo (one snapshot per change — there's no
    // "drag" boundary like a slider has).
    auto& layer = m_look.adjustmentLayers[m_selectedLayerIndex];
    const auto newMode = static_cast<lps::BlendMode>(comboIndex);
    if (layer.blendMode == newMode) return;
    m_nextHistoryLabel = tr("Layer blend mode changed");
    pushUndoSnapshot();
    layer.blendMode = newMode;
    refreshUndoRedoActions();
    markDirty();
    if (m_debounce) m_debounce->start();
}

void MainWindow::refreshLayerWidgets()
{
    if (!m_layerList) return;

    const int total = static_cast<int>(m_look.adjustmentLayers.size());

    // Clamp selection if list shrank.
    if (m_selectedLayerIndex >= total) m_selectedLayerIndex = total - 1;
    if (m_selectedLayerIndex < 0 && total > 0) m_selectedLayerIndex = 0;

    {
        QSignalBlocker block(m_layerList);
        m_layerList->clear();
        for (int i = 0; i < total; ++i) {
            const auto& al = m_look.adjustmentLayers[i];
            const QString display = al.name.isEmpty()
                ? QStringLiteral("Layer %1").arg(i + 1)
                : al.name;
            auto* item = new QListWidgetItem(display, m_layerList);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(al.enabled ? Qt::Checked : Qt::Unchecked);
        }
        if (m_selectedLayerIndex >= 0 && m_selectedLayerIndex < total) {
            m_layerList->setCurrentRow(m_selectedLayerIndex);
        }
    }

    const bool hasSelection = (m_selectedLayerIndex >= 0 &&
                                m_selectedLayerIndex < total);
    if (m_layerDeleteBtn)    m_layerDeleteBtn->setEnabled(hasSelection);
    if (m_layerDuplicateBtn) m_layerDuplicateBtn->setEnabled(hasSelection);
    if (m_layerOpacitySlider)  m_layerOpacitySlider->setEnabled(hasSelection);
    if (m_layerBlendModeCombo) m_layerBlendModeCombo->setEnabled(hasSelection);

    if (hasSelection) {
        const auto& al = m_look.adjustmentLayers[m_selectedLayerIndex];
        const int opacityPct = static_cast<int>(std::lround(al.opacity * 100.0f));
        if (m_layerOpacitySlider) {
            QSignalBlocker b(m_layerOpacitySlider);
            m_layerOpacitySlider->setValue(opacityPct);
        }
        if (m_layerOpacityValue) {
            m_layerOpacityValue->setText(QString::number(opacityPct));
        }
        if (m_layerBlendModeCombo) {
            QSignalBlocker b(m_layerBlendModeCombo);
            m_layerBlendModeCombo->setCurrentIndex(static_cast<int>(al.blendMode));
        }
        if (m_layerStatusLabel) {
            m_layerStatusLabel->setText(
                tr("Selected: %1").arg(al.name.isEmpty()
                                        ? tr("(unnamed)") : al.name));
        }
    } else {
        if (m_layerOpacitySlider) {
            QSignalBlocker b(m_layerOpacitySlider);
            m_layerOpacitySlider->setValue(100);
        }
        if (m_layerOpacityValue) m_layerOpacityValue->setText(QStringLiteral("100"));
        if (m_layerBlendModeCombo) {
            QSignalBlocker b(m_layerBlendModeCombo);
            m_layerBlendModeCombo->setCurrentIndex(0);
        }
        if (m_layerStatusLabel) {
            m_layerStatusLabel->setText(total == 0
                ? tr("No layers") : tr("No layer selected"));
        }
    }
}

// ==============================================================================
// 3-way color grading: per-wheel UI builder
//
// Builds a collapsible block for one of the four wheels:
//   - Header row with a chevron toggle and the wheel's name.
//   - Three sliders (Hue, Saturation, Strength) inside a container widget
//     that is shown/hidden by the header toggle.
//
// Slider connections push undo on press (drag start) and update the
// matching m_look.grading field on every value change, then kick the
// debounce. Same pattern as the tone/color sliders elsewhere in the panel.
// ==============================================================================
namespace {

// Pointer-to-member-of-GradingParams for each wheel index, to avoid a
// 4-way switch in three places. Index matches kGradingWheelCount.
struct WheelFields {
    float lps::GradingParams::* hue;
    float lps::GradingParams::* sat;
    float lps::GradingParams::* str;
    float lps::GradingParams::* lum;
};
constexpr WheelFields kWheels[4] = {
    { &lps::GradingParams::shadowsHue,    &lps::GradingParams::shadowsSaturation,    &lps::GradingParams::shadowsStrength,    &lps::GradingParams::shadowsLuminance    },
    { &lps::GradingParams::midtonesHue,   &lps::GradingParams::midtonesSaturation,   &lps::GradingParams::midtonesStrength,   &lps::GradingParams::midtonesLuminance   },
    { &lps::GradingParams::highlightsHue, &lps::GradingParams::highlightsSaturation, &lps::GradingParams::highlightsStrength, &lps::GradingParams::highlightsLuminance },
    { &lps::GradingParams::globalHue,     &lps::GradingParams::globalSaturation,     &lps::GradingParams::globalStrength,     &lps::GradingParams::globalLuminance     },
};

} // namespace

void MainWindow::buildGradingWheel(QWidget* parent, QVBoxLayout* col,
                                   int wheelIndex,
                                   const QString& title)
{
    Q_ASSERT(wheelIndex >= 0 && wheelIndex < kGradingWheelCount);
    auto& w = m_gradingWheels[wheelIndex];
    const WheelFields fields = kWheels[wheelIndex];

    // Header: a flat QToolButton with text "▾ Title" that toggles the
    // body's visibility. Same look as before — keep the existing
    // collapsibility convention.
    auto* header = new QToolButton(parent);
    header->setText(QString::fromUtf8("▾ ") + title);
    header->setToolButtonStyle(Qt::ToolButtonTextOnly);
    header->setAutoRaise(true);
    header->setCursor(Qt::PointingHandCursor);
    header->setStyleSheet(
        "QToolButton { color: #B8BDC6; text-align: left; padding: 4px 6px;"
        "              background: #16181D; border: 1px solid #2A2D35;"
        "              border-radius: 7px; }"
        "QToolButton:hover { color: #CCFF00; border-color: #3D424E; }");
    header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    col->addWidget(header);
    w.header = header;

    // Body: ColorWheelWidget on top, two sliders (Strength + Luminance)
    // below, plus a per-wheel reset button. Parented to a container so
    // we can toggle the whole group via setVisible.
    auto* box = new QWidget(parent);
    box->setObjectName(QStringLiteral("gradingWheelBox"));
    box->setStyleSheet(
        "QWidget#gradingWheelBox { background: #111318; border: 1px solid #2A2D35;"
        "                          border-radius: 8px; }"
        "QLabel { border: 0; background: transparent; }"
        "QSlider { border: 0; background: transparent; }");
    auto* boxLay = new QVBoxLayout(box);
    boxLay->setContentsMargins(8, 8, 8, 8);
    boxLay->setSpacing(6);

    // Wheel + small numeric readout row. The readout shows the current
    // hue (deg) and sat (%) so users have a quantitative reference.
    auto* wheelRow = new QHBoxLayout();
    wheelRow->setContentsMargins(0, 0, 0, 0);
    wheelRow->setSpacing(8);

    w.wheel = new ColorWheelWidget(box);
    w.wheel->setMinimumSize(110, 110);
    w.wheel->setMaximumSize(140, 140);
    wheelRow->addWidget(w.wheel, /*stretch=*/0);

    auto* readout = new QVBoxLayout();
    readout->setContentsMargins(0, 0, 0, 0);
    readout->setSpacing(2);
    auto makeRO = [&](const QString& label, QLabel*& valueOut) {
        auto* lbl = new QLabel(label, box);
        lbl->setStyleSheet("color: #8a8a90; font-size: 10px;");
        readout->addWidget(lbl);
        valueOut = new QLabel("0", box);
        valueOut->setStyleSheet("color: #d0d0d4; font-size: 11px; font-weight: bold;");
        readout->addWidget(valueOut);
    };
    makeRO(tr("Hue"), w.hueValue);
    makeRO(tr("Sat"), w.satValue);

    // Reset button — clears hue/sat/str/lum for this wheel.
    w.resetBtn = new QPushButton(tr("Reset"), box);
    w.resetBtn->setCursor(Qt::PointingHandCursor);
    w.resetBtn->setStyleSheet(
        "QPushButton { padding: 3px 7px; font-size: 10px;"
        "              background: #1E2026; border: 1px solid #2A2D35;"
        "              border-radius: 6px; }"
        "QPushButton:hover { border-color: #CCFF00; color: #CCFF00; }");
    readout->addWidget(w.resetBtn);
    readout->addStretch(1);

    wheelRow->addLayout(readout, /*stretch=*/1);
    boxLay->addLayout(wheelRow);

    // Strength + Luminance sliders.
    boxLay->addWidget(buildSliderRow(tr("Strength"),
                                     0, 100, 0,
                                     w.str, w.strValue));
    boxLay->addWidget(buildSliderRow(tr("Luminance"),
                                     -100, +100, 0,
                                     w.lum, w.lumValue));

    col->addWidget(box);
    w.slidersBox = box;
    w.expanded = true;

    // Header toggle.
    connect(header, &QToolButton::clicked, this,
            [this, wheelIndex, title]() {
        auto& wi = m_gradingWheels[wheelIndex];
        wi.expanded = !wi.expanded;
        if (wi.slidersBox) wi.slidersBox->setVisible(wi.expanded);
        if (wi.header) wi.header->setText(
            (wi.expanded ? QString::fromUtf8("▾ ")
                         : QString::fromUtf8("▸ ")) + title);
    });

    // Wheel wiring.
    //   - dragStarted   → pushUndoSnapshot (one snapshot per drag)
    //   - hueSatChanged → write to m_look.grading via pointer-to-member,
    //                     update readouts, kick debounce
    //   - resetRequested→ reset wheel to identity (push undo first)
    connect(w.wheel, &ColorWheelWidget::dragStarted,
            this, &MainWindow::pushUndoSnapshot);
    connect(w.wheel, &ColorWheelWidget::hueSaturationChanged, this,
            [this, fields, &w](float hueDeg, float sat01) {
        m_look.grading.*(fields.hue) = hueDeg;
        m_look.grading.*(fields.sat) = sat01 * 100.0f;
        if (w.hueValue) w.hueValue->setText(
            QString::number(static_cast<int>(std::lround(hueDeg))) + QStringLiteral("°"));
        if (w.satValue) w.satValue->setText(
            QString::number(static_cast<int>(std::lround(sat01 * 100.0f))) + QStringLiteral("%"));
        if (m_debounce) m_debounce->start();
    });
    connect(w.wheel, &ColorWheelWidget::resetRequested, this,
            [this, wheelIndex]() {
        m_nextHistoryLabel = tr("Color grading reset");
        pushUndoSnapshot();
        const auto& f = kWheels[wheelIndex];
        m_look.grading.*(f.hue) = 0.0f;
        m_look.grading.*(f.sat) = 0.0f;
        // Reset only hue+sat from the wheel's double-click — strength
        // and luminance are separate user-controlled values; don't
        // surprise the user by clearing them. The Reset button below
        // clears everything for this wheel.
        refreshGradingWidgets();
        refreshUndoRedoActions();
        markDirty();
        if (m_debounce) m_debounce->start();
    });

    // Strength slider.
    connect(w.str, &QSlider::sliderPressed,
            this, &MainWindow::pushUndoSnapshot);
    connect(w.str, &QSlider::valueChanged, this,
            [this, fields, &w](int v) {
        m_look.grading.*(fields.str) = static_cast<float>(v);
        if (w.strValue) w.strValue->setText(QString::number(v));
        if (m_debounce) m_debounce->start();
    });

    // Luminance slider.
    connect(w.lum, &QSlider::sliderPressed,
            this, &MainWindow::pushUndoSnapshot);
    connect(w.lum, &QSlider::valueChanged, this,
            [this, fields, &w](int v) {
        m_look.grading.*(fields.lum) = static_cast<float>(v);
        if (w.lumValue) w.lumValue->setText(QString::number(v));
        if (m_debounce) m_debounce->start();
    });

    // Reset button — clears all four fields for this wheel.
    connect(w.resetBtn, &QPushButton::clicked, this,
            [this, wheelIndex]() {
        m_nextHistoryLabel = tr("Color grading reset");
        pushUndoSnapshot();
        const auto& f = kWheels[wheelIndex];
        m_look.grading.*(f.hue) = 0.0f;
        m_look.grading.*(f.sat) = 0.0f;
        m_look.grading.*(f.str) = 0.0f;
        m_look.grading.*(f.lum) = 0.0f;
        refreshGradingWidgets();
        refreshUndoRedoActions();
        markDirty();
        if (m_debounce) m_debounce->start();
    });
}

// Refresh all 3-way grading widgets from m_look.grading. Called from
// applyLookToUi so undo/redo/preset-load updates the sliders. All slider
// updates are wrapped in QSignalBlockers so we don't kick the debounce
// for what's effectively a programmatic refresh.
void MainWindow::refreshGradingWidgets()
{
    for (int i = 0; i < kGradingWheelCount; ++i) {
        const auto& f = kWheels[i];
        auto& w = m_gradingWheels[i];
        if (!w.wheel || !w.str || !w.lum) continue;

        const float hueDeg = m_look.grading.*(f.hue);
        const float satPct = m_look.grading.*(f.sat);
        const int   strV   = static_cast<int>(std::lround(m_look.grading.*(f.str)));
        const int   lumV   = static_cast<int>(std::lround(m_look.grading.*(f.lum)));

        // Wheel takes hue in degrees and sat in [0, 1]. setHueSaturation
        // does NOT emit signals (programmatic update).
        w.wheel->setHueSaturation(hueDeg, satPct / 100.0f);

        // Numeric readouts next to the wheel.
        if (w.hueValue) w.hueValue->setText(
            QString::number(static_cast<int>(std::lround(hueDeg))) + QStringLiteral("°"));
        if (w.satValue) w.satValue->setText(
            QString::number(static_cast<int>(std::lround(satPct))) + QStringLiteral("%"));

        { QSignalBlocker b(w.str); w.str->setValue(strV); }
        if (w.strValue) w.strValue->setText(QString::number(strV));
        { QSignalBlocker b(w.lum); w.lum->setValue(lumV); }
        if (w.lumValue) w.lumValue->setText(QString::number(lumV));
    }

    if (m_balanceSlider) {
        const int v = static_cast<int>(std::lround(m_look.grading.balance));
        QSignalBlocker b(m_balanceSlider);
        m_balanceSlider->setValue(v);
        if (m_balanceValue) m_balanceValue->setText(QString::number(v));
    }
    if (m_blendingSlider) {
        const int v = static_cast<int>(std::lround(m_look.grading.blending));
        QSignalBlocker b(m_blendingSlider);
        m_blendingSlider->setValue(v);
        if (m_blendingValue) m_blendingValue->setText(QString::number(v));
    }

    // Advanced grading — V1 placeholders, no engine effect yet.
    auto refreshIntSlider = [](QSlider* s, QLabel* lbl, float v) {
        if (!s) return;
        const int iv = static_cast<int>(std::lround(v));
        QSignalBlocker b(s);
        s->setValue(iv);
        if (lbl) lbl->setText(QString::number(iv));
    };
    refreshIntSlider(m_liftSlider,   m_liftValue,   m_look.grading.lift);
    refreshIntSlider(m_gammaSlider,  m_gammaValue,  m_look.grading.gamma);
    refreshIntSlider(m_gainSlider,   m_gainValue,   m_look.grading.gain);
    refreshIntSlider(m_offsetSlider, m_offsetValue, m_look.grading.offset);
    refreshIntSlider(m_filmicContrastSlider,   m_filmicContrastValue,
                     m_look.grading.filmicContrast);
    refreshIntSlider(m_highlightRolloffSlider, m_highlightRolloffValue,
                     m_look.grading.highlightRolloff);
    refreshIntSlider(m_shadowLiftSlider,       m_shadowLiftValue,
                     m_look.grading.shadowLift);
    refreshIntSlider(m_fadeBlacksSlider,       m_fadeBlacksValue,
                     m_look.grading.fadeBlacks);
    refreshIntSlider(m_colorSeparationSlider,  m_colorSeparationValue,
                     m_look.grading.colorSeparation);
}

// ==============================================================================
// Edit-menu slots
// ==============================================================================
void MainWindow::onResetEdits()
{
    m_nextHistoryLabel = tr("Reset edits");
    pushUndoSnapshot();
    m_look = lps::Look{};
    applyLookToUi();
    refreshUndoRedoActions();
    if (m_presetNameLabel)
        m_presetNameLabel->setText(tr("(no preset loaded)"));
}

namespace {

float normalizedTransformDegrees(float degrees)
{
    degrees = std::fmod(degrees, 360.0f);
    if (degrees <= -180.0f) degrees += 360.0f;
    if (degrees >   180.0f) degrees -= 360.0f;
    return degrees;
}

} // namespace

void MainWindow::onRotateLeft()
{
    m_nextHistoryLabel = tr("Rotate left");
    pushUndoSnapshot();
    m_look.transform.rotationDegrees =
        normalizedTransformDegrees(m_look.transform.rotationDegrees - 90.0f);
    refreshTransformWidgets();
    if (m_previewLabel) m_previewLabel->zoomToFit();
    if (m_debounce) m_debounce->start();
}

void MainWindow::onRotateRight()
{
    m_nextHistoryLabel = tr("Rotate right");
    pushUndoSnapshot();
    m_look.transform.rotationDegrees =
        normalizedTransformDegrees(m_look.transform.rotationDegrees + 90.0f);
    refreshTransformWidgets();
    if (m_previewLabel) m_previewLabel->zoomToFit();
    if (m_debounce) m_debounce->start();
}

void MainWindow::onFlipHorizontal()
{
    m_nextHistoryLabel = tr("Flip horizontal");
    pushUndoSnapshot();
    m_look.transform.flipHorizontal = !m_look.transform.flipHorizontal;
    refreshTransformWidgets();
    if (m_debounce) m_debounce->start();
}

void MainWindow::onFlipVertical()
{
    m_nextHistoryLabel = tr("Flip vertical");
    pushUndoSnapshot();
    m_look.transform.flipVertical = !m_look.transform.flipVertical;
    refreshTransformWidgets();
    if (m_debounce) m_debounce->start();
}

void MainWindow::onResetTransform()
{
    if (m_look.transform.isIdentity()) return;
    m_nextHistoryLabel = tr("Transform reset");
    pushUndoSnapshot();
    m_look.transform.reset();
    refreshTransformWidgets();
    if (m_previewLabel) m_previewLabel->zoomToFit();
    if (m_debounce) m_debounce->start();
}

void MainWindow::onCopyLook()
{
    const QJsonDocument doc(lps::LookSerializer::toJson(m_look));
    QApplication::clipboard()->setText(QString::fromUtf8(doc.toJson(QJsonDocument::Compact)));
}

void MainWindow::onPasteLook()
{
    const QString text = QApplication::clipboard()->text();
    if (text.isEmpty()) return;

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &err);
    if (doc.isNull() || !doc.isObject()) {
        // Silent failure on bad clipboard data — too noisy to dialog every
        // accidental Ctrl+V over a non-Look clipboard payload.
        return;
    }

    lps::Look loaded;
    QString lookErr;
    if (!lps::LookSerializer::fromJson(doc.object(), loaded, &lookErr)) return;

    m_nextHistoryLabel = tr("Look pasted");
    pushUndoSnapshot();
    m_look = loaded;
    applyLookToUi();
    refreshUndoRedoActions();
}

void MainWindow::onPreferences()
{
    if (!m_settings)
        m_settings = std::make_unique<lps::SettingsManager>();

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Preferences"));
    dialog.resize(560, 380);

    auto* root = new QVBoxLayout(&dialog);
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(10);

    auto* tabs = new QTabWidget(&dialog);
    root->addWidget(tabs, 1);

    auto* generalTab = new QWidget(tabs);
    auto* generalForm = new QFormLayout(generalTab);
    generalForm->setContentsMargins(16, 16, 16, 16);
    generalForm->setSpacing(10);

    auto* welcomeCheck = new QCheckBox(tr("Show Welcome screen on startup"), generalTab);
    welcomeCheck->setChecked(m_settings->showWelcomeOnStartup());
    generalForm->addRow(QString(), welcomeCheck);

    auto* lastOpenEdit = new QLineEdit(m_settings->lastOpenFolder(), generalTab);
    auto* lastOpenRow = new QWidget(generalTab);
    auto* lastOpenLay = new QHBoxLayout(lastOpenRow);
    lastOpenLay->setContentsMargins(0, 0, 0, 0);
    lastOpenLay->addWidget(lastOpenEdit, 1);
    auto* lastOpenBrowse = new QPushButton(tr("Browse"), lastOpenRow);
    lastOpenLay->addWidget(lastOpenBrowse);
    connect(lastOpenBrowse, &QPushButton::clicked, &dialog, [this, lastOpenEdit]() {
        const QString dir = QFileDialog::getExistingDirectory(
            this, tr("Last Open Folder"), lastOpenEdit->text());
        if (!dir.isEmpty()) lastOpenEdit->setText(dir);
    });
    generalForm->addRow(tr("Last Open Folder"), lastOpenRow);

    auto* exportEdit = new QLineEdit(m_settings->defaultExportFolder(), generalTab);
    auto* exportRow = new QWidget(generalTab);
    auto* exportLay = new QHBoxLayout(exportRow);
    exportLay->setContentsMargins(0, 0, 0, 0);
    exportLay->addWidget(exportEdit, 1);
    auto* exportBrowse = new QPushButton(tr("Browse"), exportRow);
    exportLay->addWidget(exportBrowse);
    connect(exportBrowse, &QPushButton::clicked, &dialog, [this, exportEdit]() {
        const QString dir = QFileDialog::getExistingDirectory(
            this, tr("Default Export Folder"), exportEdit->text());
        if (!dir.isEmpty()) exportEdit->setText(dir);
    });
    generalForm->addRow(tr("Default Export Folder"), exportRow);
    tabs->addTab(generalTab, tr("General"));

    auto* appearanceTab = new QWidget(tabs);
    auto* appearanceForm = new QFormLayout(appearanceTab);
    appearanceForm->setContentsMargins(16, 16, 16, 16);
    auto* themeCombo = new QComboBox(appearanceTab);
    themeCombo->addItems(QStringList{ tr("Dark"), tr("System") });
    const int themeIndex = themeCombo->findText(m_settings->themeName());
    if (themeIndex >= 0) themeCombo->setCurrentIndex(themeIndex);
    appearanceForm->addRow(tr("Theme"), themeCombo);
    tabs->addTab(appearanceTab, tr("Appearance"));

    auto* performanceTab = new QWidget(tabs);
    auto* performanceForm = new QFormLayout(performanceTab);
    performanceForm->setContentsMargins(16, 16, 16, 16);
    auto* backendCombo = new QComboBox(performanceTab);
    backendCombo->addItems(QStringList{ tr("CPU"), tr("GPU (future)") });
    const int backendIndex = backendCombo->findText(m_settings->renderBackend());
    if (backendIndex >= 0) backendCombo->setCurrentIndex(backendIndex);
    performanceForm->addRow(tr("Render Backend"), backendCombo);
    tabs->addTab(performanceTab, tr("Performance"));

    auto* pluginsTab = new QWidget(tabs);
    auto* pluginsLay = new QVBoxLayout(pluginsTab);
    pluginsLay->setContentsMargins(16, 16, 16, 16);
    pluginsLay->setSpacing(10);
    if (!m_pluginManager)
        m_pluginManager = std::make_unique<lps::PluginManager>();
    auto* pluginPath = new QLabel(m_pluginManager->pluginsDirPath(), pluginsTab);
    pluginPath->setTextInteractionFlags(Qt::TextSelectableByMouse);
    pluginsLay->addWidget(new QLabel(tr("Plugins Folder"), pluginsTab));
    pluginsLay->addWidget(pluginPath);
    auto* pluginBtns = new QHBoxLayout();
    auto* openPlugins = new QPushButton(tr("Open Plugins Folder"), pluginsTab);
    auto* managerBtn = new QPushButton(tr("Plugin Manager"), pluginsTab);
    pluginBtns->addWidget(openPlugins);
    pluginBtns->addWidget(managerBtn);
    pluginBtns->addStretch(1);
    pluginsLay->addLayout(pluginBtns);
    pluginsLay->addStretch(1);
    connect(openPlugins, &QPushButton::clicked,
            this, &MainWindow::onOpenPluginsFolder);
    connect(managerBtn, &QPushButton::clicked,
            this, &MainWindow::onPluginManager);
    tabs->addTab(pluginsTab, tr("Plugins"));

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    root->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted) return;

    m_settings->setShowWelcomeOnStartup(welcomeCheck->isChecked());
    m_settings->setLastOpenFolder(lastOpenEdit->text());
    m_settings->setDefaultExportFolder(exportEdit->text());
    m_settings->setThemeName(themeCombo->currentText());
    m_settings->setRenderBackend(backendCombo->currentText());

    if (m_welcomeScreen)
        m_welcomeScreen->setShowOnStartup(welcomeCheck->isChecked());
}

// ==============================================================================
// View-menu slots
// ==============================================================================
void MainWindow::onZoomFit()
{
    if (m_previewLabel) m_previewLabel->zoomToFit();
}

void MainWindow::onZoom100()
{
    if (m_previewLabel) m_previewLabel->zoomTo100();
}

void MainWindow::onZoomIn()
{
    if (m_previewLabel) m_previewLabel->zoomBy(1.25);
}

void MainWindow::onZoomOut()
{
    if (m_previewLabel) m_previewLabel->zoomBy(1.0 / 1.25);
}

void MainWindow::onToggleBeforeAfter()
{
    // Distinct from press-and-hold Spacebar. This is the menu's sticky
    // toggle: clicking once shows original, clicking again returns to
    // edited. Updates m_showOriginal (the same flag the spacebar handler
    // uses) so the preview widget displays the right image.
    m_showOriginal = !m_showOriginal;
    if (m_actBeforeAfter) m_actBeforeAfter->setChecked(m_showOriginal);
    if (m_viewModeLabel)  m_viewModeLabel->setText(
        m_showOriginal ? tr("Original") : tr("Edited"));
    refreshPreviewLabel();
}

void MainWindow::onToggleFullscreen()
{
    if (isFullScreen()) {
        showNormal();
        if (m_actFullscreen) m_actFullscreen->setChecked(false);
    } else {
        showFullScreen();
        if (m_actFullscreen) m_actFullscreen->setChecked(true);
    }
}

void MainWindow::onSecondaryViewer()
{
    // Lazy-create on first use. After this, the pointer stays valid for
    // the lifetime of MainWindow — closing the window via X just hides
    // it (default Qt behavior; WA_DeleteOnClose is off).
    if (!m_secondaryViewer) {
        m_secondaryViewer = new SecondaryViewerWindow(this);
        // Seed it with the current processed image so it doesn't open
        // blank if there's already an image in the main viewer.
        m_secondaryViewer->setImage(m_processed);
    }

    // Show / raise / activate. show() is idempotent for already-visible
    // windows; raise+activate handle the "user clicked the menu while
    // the window is hidden behind another app" case.
    m_secondaryViewer->show();
    m_secondaryViewer->raise();
    m_secondaryViewer->activateWindow();
}

// ==============================================================================
// Node Graph
//
// Lazy-creates the dock + widget on first call. Subsequent calls just
// re-show and raise the dock. The dock starts attached at the bottom
// per spec; the user can drag it to float, attach to other edges, or
// close it (close = hide, not destroy — same pattern as the secondary
// viewer).
//
// The node graph is purely visual in V1. Selecting / panning / zooming
// inside it doesn't affect the rendered image. Engine code is unchanged.
// ==============================================================================
void MainWindow::onShowNodeGraph()
{
    if (m_bottomPanelTabs) m_bottomPanelTabs->setCurrentIndex(0);
    if (!m_nodeGraphDock) return;
    m_bottomWorkspaceEnabled = true;
    if (m_settings) m_settings->setBottomWorkspaceVisible(true);
    setBottomWorkspaceCollapsed(false);
    if (m_actShowBottomWorkspace) {
        QSignalBlocker block(m_actShowBottomWorkspace);
        m_actShowBottomWorkspace->setChecked(true);
    }
    if (!m_editorWorkspaceActive) {
        updateBottomWorkspaceVisibility();
        return;
    }
    m_nodeGraphDock->show();
    m_nodeGraphDock->raise();
}

// ==============================================================================
// Window-menu slots
// ==============================================================================
void MainWindow::onResetWorkspaceLayout()
{
    // Only persistent layout state today is the sidebar collapse. Reset =
    // expand the sidebar and show the histogram.
    if (m_sidebarCollapsed) onToggleSidebar();
    setAnalysisPanelCollapsed(false);
    if (m_histogramWidget)  m_histogramWidget->setVisible(true);
    if (m_actShowHistogram) m_actShowHistogram->setChecked(true);
    if (m_actShowControls)  m_actShowControls->setChecked(true);
    m_bottomWorkspaceEnabled = true;
    if (m_settings) m_settings->setBottomWorkspaceVisible(true);
    setBottomWorkspaceCollapsed(false);
    if (m_actShowBottomWorkspace) m_actShowBottomWorkspace->setChecked(true);
    updateBottomWorkspaceVisibility();
    if (m_editorWorkspaceActive && m_nodeGraphDock) {
        m_nodeGraphDock->setFloating(false);
        addDockWidget(Qt::BottomDockWidgetArea, m_nodeGraphDock);
    }
}

void MainWindow::onSaveWorkspaceLayout()
{
    // Placeholder — when persistent settings land, this will write the
    // current sidebar/dock state to QSettings.
    showPlaceholder(this, tr("Save Workspace Layout"));
}

// ==============================================================================
// Plugins
// ==============================================================================
void MainWindow::onPluginManager()
{
    if (!m_pluginManager)
        m_pluginManager = std::make_unique<lps::PluginManager>();
    m_pluginManager->reload();

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Lumen Plugin Manager"));
    dialog.resize(760, 460);

    auto* root = new QVBoxLayout(&dialog);
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(10);

    auto* title = new QLabel(tr("Installed Plugins"), &dialog);
    QFont titleFont = title->font();
    titleFont.setPointSize(titleFont.pointSize() + 2);
    titleFont.setBold(true);
    title->setFont(titleFont);
    root->addWidget(title);

    auto* pathLabel = new QLabel(m_pluginManager->pluginsDirPath(), &dialog);
    pathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    pathLabel->setObjectName(QStringLiteral("subtleLabel"));
    root->addWidget(pathLabel);

    auto* table = new QTableWidget(&dialog);
    table->setColumnCount(4);
    table->setHorizontalHeaderLabels({
        tr("Plugin"),
        tr("Version"),
        tr("Type"),
        tr("Enabled"),
    });
    table->verticalHeader()->hide();
    table->horizontalHeader()->setStretchLastSection(false);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setAlternatingRowColors(true);
    root->addWidget(table, 1);

    auto populateTable = [this, table]() {
        QSignalBlocker block(table);
        table->clearSpans();
        const QList<lps::PluginInfo> plugins = m_pluginManager
            ? m_pluginManager->installedPlugins()
            : QList<lps::PluginInfo>();

        if (plugins.isEmpty()) {
            table->setRowCount(1);
            table->setSpan(0, 0, 1, 4);
            auto* empty = new QTableWidgetItem(
                tr("No plugins installed. Use Install Plugin to copy a folder or ZIP package."));
            empty->setFlags(Qt::ItemIsEnabled);
            table->setItem(0, 0, empty);
            return;
        }

        const int pluginCount = static_cast<int>(plugins.size());
        table->setRowCount(pluginCount);
        for (int row = 0; row < pluginCount; ++row) {
            const lps::PluginInfo& plugin = plugins.at(row);
            const QString name = plugin.name.isEmpty() ? plugin.id : plugin.name;
            const QString tooltip = plugin.valid
                ? tr("%1\nAuthor: %2\nPath: %3")
                    .arg(plugin.description, plugin.author, plugin.path)
                : tr("%1\nPath: %2")
                    .arg(plugin.error, plugin.path);

            auto* nameItem = new QTableWidgetItem(plugin.valid
                ? name
                : tr("%1  (invalid)").arg(name));
            nameItem->setToolTip(tooltip);
            nameItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);

            auto* versionItem = new QTableWidgetItem(plugin.version);
            versionItem->setToolTip(tooltip);
            versionItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);

            auto* typeItem = new QTableWidgetItem(plugin.type);
            typeItem->setToolTip(tooltip);
            typeItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);

            auto* enabledItem = new QTableWidgetItem();
            enabledItem->setToolTip(tooltip);
            enabledItem->setData(Qt::UserRole, plugin.id);
            enabledItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            if (plugin.valid) {
                enabledItem->setFlags(enabledItem->flags() | Qt::ItemIsUserCheckable);
                enabledItem->setCheckState(plugin.enabled ? Qt::Checked : Qt::Unchecked);
            } else {
                enabledItem->setText(QStringLiteral("-"));
            }

            table->setItem(row, 0, nameItem);
            table->setItem(row, 1, versionItem);
            table->setItem(row, 2, typeItem);
            table->setItem(row, 3, enabledItem);
        }
    };

    connect(table, &QTableWidget::itemChanged, &dialog,
            [this, populateTable](QTableWidgetItem* item) {
        if (!item || item->column() != 3 || !m_pluginManager) return;
        const QString pluginId = item->data(Qt::UserRole).toString();
        if (pluginId.isEmpty()) return;

        QString error;
        const bool enabled = item->checkState() == Qt::Checked;
        if (!m_pluginManager->setPluginEnabled(pluginId, enabled, &error)) {
            QMessageBox::warning(this, tr("Plugin Manager"), error);
        }
        populateTable();
    });

    auto* buttons = new QHBoxLayout();
    buttons->setContentsMargins(0, 0, 0, 0);
    buttons->setSpacing(8);

    auto* installBtn = new QPushButton(tr("Install Plugin"), &dialog);
    auto* openFolderBtn = new QPushButton(tr("Open Plugins Folder"), &dialog);
    auto* reloadBtn = new QPushButton(tr("Reload Plugins"), &dialog);
    auto* closeBtn = new QPushButton(tr("Close"), &dialog);

    buttons->addWidget(installBtn);
    buttons->addWidget(openFolderBtn);
    buttons->addWidget(reloadBtn);
    buttons->addStretch(1);
    buttons->addWidget(closeBtn);
    root->addLayout(buttons);

    connect(installBtn, &QPushButton::clicked, &dialog, [this, populateTable]() {
        onInstallPlugin();
        if (m_pluginManager) m_pluginManager->reload();
        populateTable();
    });
    connect(openFolderBtn, &QPushButton::clicked,
            this, &MainWindow::onOpenPluginsFolder);
    connect(reloadBtn, &QPushButton::clicked, &dialog, [this, populateTable]() {
        if (m_pluginManager) m_pluginManager->reload();
        populateTable();
    });
    connect(closeBtn, &QPushButton::clicked, &dialog, &QDialog::accept);

    populateTable();
    dialog.exec();
}

void MainWindow::onInstallPlugin()
{
    if (!m_pluginManager)
        m_pluginManager = std::make_unique<lps::PluginManager>();

    QMessageBox choice(this);
    choice.setWindowTitle(tr("Install Plugin"));
    choice.setText(tr("Install a plugin folder or copy a ZIP package into the plugins folder."));
    auto* zipButton = choice.addButton(tr("Select ZIP"), QMessageBox::AcceptRole);
    auto* folderButton = choice.addButton(tr("Select Folder"), QMessageBox::ActionRole);
    choice.addButton(QMessageBox::Cancel);
    choice.exec();

    QString path;
    if (choice.clickedButton() == zipButton) {
        path = QFileDialog::getOpenFileName(
            this, tr("Install Plugin Package"),
            QStandardPaths::writableLocation(QStandardPaths::DownloadLocation),
            tr("Plugin Packages (*.zip)"));
    } else if (choice.clickedButton() == folderButton) {
        path = QFileDialog::getExistingDirectory(
            this, tr("Install Plugin Folder"),
            QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
    }
    if (path.isEmpty()) return;

    QString error;
    if (!m_pluginManager->installPluginFromPath(path, &error)) {
        QMessageBox::warning(this, tr("Install Plugin"), error);
        return;
    }

    QMessageBox::information(this, tr("Install Plugin"),
        tr("Plugin copied to:\n%1").arg(m_pluginManager->pluginsDirPath()));
}

void MainWindow::onReloadPlugins()
{
    // Placeholder — no plugin runtime exists yet.
    if (!m_pluginManager)
        m_pluginManager = std::make_unique<lps::PluginManager>();
    m_pluginManager->reload();
    QMessageBox::information(this, tr("Reload Plugins"),
        tr("Found %1 plugin item(s).")
            .arg(static_cast<int>(m_pluginManager->installedPlugins().size())));
}

void MainWindow::onOpenPluginsFolder()
{
    if (!m_pluginManager)
        m_pluginManager = std::make_unique<lps::PluginManager>();
    if (!m_pluginManager->ensurePluginsFolder()) {
        QMessageBox::warning(this, tr("Open Plugins Folder"),
            m_pluginManager->lastError());
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(m_pluginManager->pluginsDirPath()));
}

// ==============================================================================
// Help
// ==============================================================================
void MainWindow::onShowKeyboardShortcuts()
{
    QString body;
    body += tr("File\n");
    body += tr("  Ctrl+I              Open Image\n");
    body += tr("  Ctrl+O              Open Project\n");
    body += tr("  Ctrl+S              Save Project\n");
    body += tr("  Ctrl+Shift+S        Save Project As\n");
    body += tr("  Ctrl+E              Export Image\n\n");
    body += tr("Edit\n");
    body += tr("  Ctrl+Z              Undo\n");
    body += tr("  Ctrl+Y              Redo\n");
    body += tr("  Ctrl+[ / Ctrl+]     Rotate Left / Right\n");
    body += tr("  Ctrl+Alt+C / V      Copy / Paste Look\n\n");
    body += tr("View\n");
    body += tr("  Ctrl+0              Fit to Screen\n");
    body += tr("  Ctrl+1              100%\n");
    body += tr("  Ctrl+= / Ctrl+-     Zoom In / Out\n");
    body += tr("  \\                   Toggle Before/After\n");
    body += tr("  Hold Space          Show Original (press-and-hold)\n");
    body += tr("  F11                 Fullscreen\n\n");
    body += tr("Preview\n");
    body += tr("  Ctrl + Wheel        Zoom\n");
    body += tr("  Double-click        Toggle Fit / 100%\n");
    body += tr("  Middle-drag         Pan\n");
    body += tr("  Alt + Left-drag     Pan");
    QMessageBox::information(this, tr("Keyboard Shortcuts"), body);
}

void MainWindow::onShowDocumentation()
{
    showPlaceholder(this, tr("Documentation"));
}

void MainWindow::onAbout()
{
    QMessageBox::about(this, tr("About Lumen Photo Studio"),
        tr("<b>Lumen Photo Studio</b><br><br>"
           "A non-destructive photo editor built on a custom Qt6/C++20 "
           "engine.<br><br>"
           "Tone, color, HSL, curves, histogram, undo/redo, project "
           "save/load, and export to PNG/JPG/TIFF/WebP."));
}

// ==============================================================================
// Sidebar collapse / expand
//
// Toggles the QStackedLayout between the full controls panel and the
// narrow icon strip. The full panel widget tree stays alive across
// toggles — slider values, HSL channel selection, curve editor state
// all persist.
//
// The sidebar host's max-width is adjusted so the host actually shrinks
// when collapsed (otherwise QStackedLayout would size to the full
// panel's 320 px and just center the mini strip inside that area).
// ==============================================================================
void MainWindow::onToggleSidebar()
{
    if (!m_sidebarStack || !m_sidebarHost) return;
    m_sidebarCollapsed = !m_sidebarCollapsed;

    if (m_sidebarCollapsed) {
        m_sidebarHost->setMinimumWidth(0);
        m_sidebarHost->setMaximumWidth(0);
        m_sidebarHost->setVisible(false);
    } else {
        m_sidebarHost->setVisible(true);
        m_sidebarStack->setCurrentWidget(m_sidebarFull);
        m_sidebarHost->setMinimumWidth(320);
        m_sidebarHost->setMaximumWidth(QWIDGETSIZE_MAX);
    }

    // Keep the View > Show Controls Panel checkbox in sync. setChecked
    // emits toggled, which would re-enter onToggleSidebar; guard with a
    // signal blocker.
    if (m_actShowControls) {
        QSignalBlocker block(m_actShowControls);
        m_actShowControls->setChecked(!m_sidebarCollapsed);
    }
}

// ==============================================================================
// Preview right-click context menu
//
// Built on the fly each time so newly-changed action text/state is
// reflected. Hosts the same QAction members as the top menus, so
// triggering an item runs the exact same slot — no separate command
// path to keep in sync.
// ==============================================================================
void MainWindow::onPreviewContextMenu(const QPoint& posInPreview)
{
    if (!m_previewLabel) return;

    // No image — context menu items wouldn't apply. The empty-state
    // overlay handles its own click-to-open interaction; users get to
    // the Open Image dialog from there or from the File menu.
    if (m_originalFullRes.isNull()) return;

    QMenu menu(this);
    menu.addAction(m_actZoomFit);
    menu.addAction(m_actZoom100);
    menu.addAction(m_actZoomIn);
    menu.addAction(m_actZoomOut);
    menu.addSeparator();
    menu.addAction(m_actBeforeAfter);
    menu.addSeparator();
    menu.addAction(m_actRotateLeft);
    menu.addAction(m_actRotateRight);
    menu.addAction(m_actFlipHorizontal);
    menu.addAction(m_actFlipVertical);
    menu.addSeparator();
    menu.addAction(m_actOpenImage);
    menu.addAction(m_actExportImage);
    menu.addSeparator();
    menu.addAction(m_actResetEdits);

    menu.exec(m_previewLabel->mapToGlobal(posInPreview));
}
