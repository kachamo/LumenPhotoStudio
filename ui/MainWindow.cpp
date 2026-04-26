// ==============================================================================
// ui/MainWindow.cpp
// ==============================================================================
#include "MainWindow.h"

#include "CurveEditorWidget.h"
#include "EmptyStateOverlay.h"
#include "HistogramWidget.h"
#include "PreviewWidget.h"
#include "SecondaryViewerWindow.h"

#include "core/ImagePipeline.h"
#include "preset/LookSerializer.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFuture>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSlider>
#include <QStackedLayout>
#include <QStandardPaths>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>
#include <cmath>
#include <array>

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

    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(kDebounceMs);
    connect(m_debounce, &QTimer::timeout, this, &MainWindow::onDebounceFired);

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
}

MainWindow::~MainWindow() = default;

// ==============================================================================
// UI construction
// ==============================================================================
void MainWindow::buildUi()
{
    // Top-level menus (File, Edit, View, Window, Plugins, Help). Built in
    // a dedicated method to keep buildUi() focused on widget layout.
    // buildMenus also stores QAction* members for actions that need to be
    // referenced after construction (Undo/Redo enable-state, panel
    // visibility toggles, etc.).
    buildMenus();

    auto* central = new QWidget(this);
    auto* root = new QHBoxLayout(central);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(8);

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
    m_previewLabel = new PreviewWidget(central);
    root->addWidget(m_previewLabel, /*stretch=*/1);

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
        if (!maybePromptUnsavedChanges()) return;
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
    m_sidebarFull->setMinimumWidth(320);

    m_sidebarMini = new QWidget(central);
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
        connect(expandBtn, &QToolButton::clicked,
                this, &MainWindow::onToggleSidebar);
        miniLay->addWidget(expandBtn, 0, Qt::AlignHCenter);

        miniLay->addSpacing(8);

        // Section icons (text glyphs in lieu of real icon assets — keeps
        // the visual language consistent without adding image deps).
        // Each one re-expands the sidebar; future work could scroll-to
        // the corresponding section after expanding.
        const struct { const char* label; const char* tip; } kSections[] = {
            { "H", "Histogram"     },
            { "T", "Tone"          },
            { "C", "Color"         },
            { "S", "HSL"           },
            { "K", "Curves"        },
        };
        for (const auto& s : kSections) {
            auto* btn = new QToolButton(m_sidebarMini);
            btn->setText(QString::fromLatin1(s.label));
            btn->setToolTip(tr(s.tip));
            btn->setCursor(Qt::PointingHandCursor);
            btn->setFixedSize(32, 28);
            btn->setStyleSheet(
                "QToolButton { color: #9A9AA0; border: 1px solid #3a3a3f;"
                "              border-radius: 3px; }"
                "QToolButton:hover { color: #d0d0d4; border-color: #6a6a70; }");
            connect(btn, &QToolButton::clicked,
                    this, &MainWindow::onToggleSidebar);
            miniLay->addWidget(btn, 0, Qt::AlignHCenter);
        }
        miniLay->addStretch(1);
    }

    m_sidebarHost  = new QWidget(central);
    m_sidebarStack = new QStackedLayout(m_sidebarHost);
    m_sidebarStack->setContentsMargins(0, 0, 0, 0);
    m_sidebarStack->addWidget(m_sidebarFull);
    m_sidebarStack->addWidget(m_sidebarMini);
    m_sidebarStack->setCurrentWidget(m_sidebarFull);   // start expanded
    root->addWidget(m_sidebarHost, /*stretch=*/0);

    setCentralWidget(central);

    // Initial title — no project, no image yet.
    updateWindowTitle();
}

QWidget* MainWindow::buildControlPanel()
{
    // Outer wrapper: a fixed-height header row (collapse button) on top of
    // a scrollable region with all the actual controls. Wrapping like this
    // keeps the collapse toggle pinned at the top regardless of how far
    // the user has scrolled inside the controls.
    auto* outer = new QWidget(this);
    auto* outerLay = new QVBoxLayout(outer);
    outerLay->setContentsMargins(0, 0, 0, 0);
    outerLay->setSpacing(0);

    // ---- Header row with collapse toggle ----------------------------------
    {
        auto* header = new QWidget(outer);
        auto* headerLay = new QHBoxLayout(header);
        headerLay->setContentsMargins(4, 4, 4, 4);
        headerLay->addStretch(1);

        auto* collapseBtn = new QToolButton(header);
        collapseBtn->setText(QStringLiteral("›"));
        collapseBtn->setToolTip(tr("Collapse controls panel"));
        collapseBtn->setCursor(Qt::PointingHandCursor);
        collapseBtn->setFixedSize(28, 24);
        connect(collapseBtn, &QToolButton::clicked,
                this, &MainWindow::onToggleSidebar);
        headerLay->addWidget(collapseBtn);
        outerLay->addWidget(header);
    }

    // ---- Scrollable content area ------------------------------------------
    // Scrollable so the controls don't get clipped on short windows.
    auto* scroll = new QScrollArea(outer);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* panel = new QWidget(scroll);
    auto* col = new QVBoxLayout(panel);
    col->setContentsMargins(4, 4, 4, 4);
    col->setSpacing(6);

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
    // HISTOGRAM). Hoisted to the top so we can reuse it across sections
    // without repeating the QFont setup.
    QFont hf = panel->font();
    hf.setBold(true);

    // ---- Section: HISTOGRAM -----------------------------------------------
    // Lives at the top of the panel so users see live histogram feedback
    // alongside whatever section they're scrolled to. The widget itself is
    // ~80 px tall; setImage() recomputes bins after every preview render.
    {
        auto* histHeader = new QLabel(tr("HISTOGRAM"), panel);
        histHeader->setFont(hf);
        histHeader->setStyleSheet("color: #9A9AA0; padding-top: 4px;");
        col->addWidget(histHeader);

        m_histogramWidget = new HistogramWidget(panel);
        m_histogramWidget->setMinimumHeight(80);
        col->addWidget(m_histogramWidget);
    }

    // ---- Section header ----------------------------------------------------
    auto* toneHeader = new QLabel(tr("TONE"), panel);
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

    auto* slider = new QSlider(Qt::Horizontal, row);
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
    if (m_isApplyingLookToUi) return;

    m_undoStack.push_back(m_look);

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
}

void MainWindow::undo()
{
    if (m_undoStack.empty()) return;

    // Current state → redo stack; previous state → m_look.
    m_redoStack.push_back(m_look);
    m_look = m_undoStack.back();
    m_undoStack.pop_back();

    applyLookToUi();
    refreshUndoRedoActions();
}

void MainWindow::redo()
{
    if (m_redoStack.empty()) return;

    // Current state → undo stack; next state → m_look.
    m_undoStack.push_back(m_look);
    m_look = m_redoStack.back();
    m_redoStack.pop_back();

    // Note: we do NOT cap the undo stack here. The user can't possibly have
    // redone more than they undid (that'd require the redo stack to have
    // more entries than the undo stack's capacity), so the natural growth
    // is bounded by the cap enforced in pushUndoSnapshot().

    applyLookToUi();
    refreshUndoRedoActions();
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
// Project file format version. Bump when the .lumen JSON envelope structure
// changes (the embedded "look" object follows LookSerializer's own versioning).
constexpr int kProjectSchemaVersion = 1;

void MainWindow::onOpenImage()
{
    // Behavior: opening a new image gives a clean slate — all edits reset
    // to default. (Previous version preserved the Look across image opens
    // for "apply consistent edits to a series" workflows; users found the
    // hidden carry-over surprising, so we now reset.) If you want to
    // share edits across images, use Save Project / Open Project instead.
    //
    // Because the reset DOES discard work, we prompt for unsaved changes
    // before showing the file dialog. Drag-and-drop runs the same prompt
    // in MainWindow's drop handler before calling loadImageFromPath.
    if (!maybePromptUnsavedChanges()) return;

    const QString picturesDir =
        QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    const QString path = QFileDialog::getOpenFileName(
        this,
        tr("Open Image"),
        picturesDir,
        tr("Images (*.png *.jpg *.jpeg *.bmp *.tiff *.webp)"));
    if (path.isEmpty()) return;

    loadImageFromPath(path);
}

bool MainWindow::loadImageFromPath(const QString& path)
{
    QImage img;
    if (!img.load(path)) {
        QMessageBox::warning(this, tr("Open Image"),
                             tr("Could not load: %1").arg(path));
        return false;
    }

    // ---- Commit the new image + reset all editor state -------------------
    m_originalFullRes = img;
    m_currentImagePath = path;
    m_currentProjectPath.clear();   // new image = no project yet

    const int longest = std::max(img.width(), img.height());
    if (longest <= kPreviewMaxEdge) {
        m_previewSource = img;
    } else {
        m_previewSource = img.scaled(kPreviewMaxEdge, kPreviewMaxEdge,
                                     Qt::KeepAspectRatio,
                                     Qt::SmoothTransformation);
    }

    // Reset the edit state. lps::Look is a default-constructible aggregate
    // whose member defaults all correspond to identity — sliders at 0,
    // curves at the {(0,0),(1,1)} two-point identity, etc. Plain assignment
    // is the canonical reset.
    m_look = lps::Look{};

    // Clear undo history. The new image is its own clean baseline; letting
    // the user undo into the previous image's edit chain would be very
    // confusing (and the snapshots would be applied to a different source).
    m_undoStack.clear();
    m_redoStack.clear();
    refreshUndoRedoActions();

    // Push the freshly-default Look into every UI widget. m_isLoadingProject
    // is repurposed here as a generic "next render isn't a user edit"
    // suppression flag — it gates the dirty-mark inside the debounce
    // handler, so the render that applyLookToUi triggers doesn't make the
    // new untitled project look dirty before the user has touched anything.
    m_isLoadingProject = true;
    applyLookToUi();
    // applyLookToUi kicked the debounce; cancel it because we're about to
    // render directly. Without this, we'd render twice for the same Look.
    if (m_debounce) m_debounce->stop();
    m_isLoadingProject = false;

    // Clean baseline: not dirty, refresh the title to "Untitled" with no
    // asterisk.
    m_projectDirty = false;
    updateWindowTitle();

    // Hide the empty-state overlay so the preview widget's image content
    // shows through. Symmetric with showing it again on the no-image
    // branches (see loadProjectFromPath).
    if (m_emptyState) m_emptyState->hide();

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
    if (!maybePromptUnsavedChanges()) return;

    const QString picturesDir =
        QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    const QString path = QFileDialog::getOpenFileName(
        this,
        tr("Open Project"),
        picturesDir,
        tr("Lumen Projects (*.lumen);;All Files (*)"));
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
    saveProjectToPath(m_currentProjectPath);
}

void MainWindow::onSaveProjectAs()
{
    const QString picturesDir =
        QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    QString defaultName = QStringLiteral("Untitled.lumen");
    if (!m_currentImagePath.isEmpty()) {
        // Suggest the source image's basename + .lumen.
        const QFileInfo fi(m_currentImagePath);
        defaultName = fi.completeBaseName() + QStringLiteral(".lumen");
    }

    const QString path = QFileDialog::getSaveFileName(
        this,
        tr("Save Project As"),
        picturesDir + QLatin1Char('/') + defaultName,
        tr("Lumen Projects (*.lumen)"));
    if (path.isEmpty()) return;

    // Force the .lumen extension if the user didn't supply one. Some platforms'
    // dialogs don't auto-append based on the filter.
    QString finalPath = path;
    if (!finalPath.endsWith(QStringLiteral(".lumen"), Qt::CaseInsensitive)) {
        finalPath += QStringLiteral(".lumen");
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

    const QString picturesDir =
        QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);

    // Suggest source basename as the default export name.
    QString defaultName = QStringLiteral("export.png");
    if (!m_currentImagePath.isEmpty()) {
        const QFileInfo fi(m_currentImagePath);
        defaultName = fi.completeBaseName() + QStringLiteral("_export.png");
    }

    // Filter list. WebP support depends on the Qt build's image plugins;
    // Qt's QImage::save returns false at runtime if the format isn't
    // available, so we offer the option and surface a clean error if it
    // fails. Same applies to TIFF (usually present, occasionally trimmed
    // out of minimal Qt builds).
    const QString filters = tr(
        "PNG (*.png);;"
        "JPEG (*.jpg *.jpeg);;"
        "TIFF (*.tif *.tiff);;"
        "WebP (*.webp)");

    const QString path = QFileDialog::getSaveFileName(
        this,
        tr("Export Image"),
        picturesDir + QLatin1Char('/') + defaultName,
        filters);
    if (path.isEmpty()) return;

    // Full-resolution render through the same pipeline. Blocks the UI thread —
    // acceptable for a desktop tool at typical photo sizes; a future step
    // could move this onto the existing QFutureWatcher infrastructure for
    // huge files.
    lps::ImagePipeline pipeline;
    const lps::RenderResult r = pipeline.render(m_originalFullRes, m_look);
    if (r.image.isNull()) {
        QMessageBox::warning(this, tr("Export Image"), tr("Render failed."));
        return;
    }

    if (!r.image.save(path)) {
        QMessageBox::warning(
            this, tr("Export Image"),
            tr("Could not write: %1\n\n"
               "If you exported as WebP or TIFF, your Qt build may not "
               "include support for that format. Try PNG or JPEG.")
                .arg(path));
    }
}

// ==============================================================================
// Project I/O — .lumen file format
//
// JSON envelope:
//   {
//     "schemaVersion":      1,
//     "projectName":        "...",
//     "originalImagePath":  "...",
//     "look":               { ... LookSerializer JSON ... }
//   }
//
// The "look" object is exactly what LookSerializer::toJson produces, embedded
// inline (not stringified) so the .lumen file is human-readable. This means
// future Look schema changes propagate automatically.
//
// originalImagePath is stored as an absolute path. Future work: consider
// storing it relative to the .lumen file's directory so projects can be
// moved/shared. Out of scope for this step.
// ==============================================================================
bool MainWindow::saveProjectToPath(const QString& path)
{
    QJsonObject root;
    root.insert(QStringLiteral("schemaVersion"), kProjectSchemaVersion);
    root.insert(QStringLiteral("projectName"),
                QFileInfo(path).completeBaseName());
    root.insert(QStringLiteral("originalImagePath"), m_currentImagePath);
    root.insert(QStringLiteral("look"), lps::LookSerializer::toJson(m_look));

    QJsonDocument doc(root);
    const QByteArray bytes = doc.toJson(QJsonDocument::Indented);

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, tr("Save Project"),
                             tr("Could not open for writing: %1").arg(path));
        return false;
    }
    if (f.write(bytes) != bytes.size()) {
        QMessageBox::warning(this, tr("Save Project"),
                             tr("Write failed: %1").arg(path));
        return false;
    }
    f.close();

    m_currentProjectPath = path;
    m_projectDirty = false;
    updateWindowTitle();
    return true;
}

bool MainWindow::loadProjectFromPath(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("Open Project"),
                             tr("Could not open: %1").arg(path));
        return false;
    }
    const QByteArray bytes = f.readAll();
    f.close();

    QJsonParseError parseErr;
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &parseErr);
    if (doc.isNull() || !doc.isObject()) {
        QMessageBox::warning(this, tr("Open Project"),
                             tr("Invalid project file: %1\n\n%2")
                                 .arg(path, parseErr.errorString()));
        return false;
    }

    const QJsonObject root = doc.object();
    const int fileSchema = root.value(QStringLiteral("schemaVersion")).toInt(1);
    if (fileSchema > kProjectSchemaVersion) {
        QMessageBox::warning(
            this, tr("Open Project"),
            tr("This project was saved with a newer version of Lumen "
               "(schema %1). Some settings may not load correctly.")
                .arg(fileSchema));
        // Continue loading anyway — the embedded LookSerializer JSON is
        // tolerant of missing fields, so we'll get a best-effort import.
    }

    const QString imagePath = root.value(QStringLiteral("originalImagePath")).toString();
    const QJsonValue lookVal = root.value(QStringLiteral("look"));

    if (!lookVal.isObject()) {
        QMessageBox::warning(this, tr("Open Project"),
                             tr("Project file is missing Look data: %1").arg(path));
        return false;
    }

    lps::Look loadedLook;
    QString lookErr;
    if (!lps::LookSerializer::fromJson(lookVal.toObject(), loadedLook, &lookErr)) {
        QMessageBox::warning(this, tr("Open Project"),
                             tr("Could not parse Look:\n%1").arg(lookErr));
        return false;
    }

    // Try to load the referenced source image. If it's missing/unreadable,
    // surface a non-fatal warning and continue — the user can still see
    // the Look settings, and "relink" workflows are common in real editors.
    QImage img;
    bool imageLoaded = false;
    if (!imagePath.isEmpty() && img.load(imagePath)) {
        imageLoaded = true;
    } else if (!imagePath.isEmpty()) {
        QMessageBox::information(
            this, tr("Open Project"),
            tr("Original image not found:\n%1\n\n"
               "Project loaded but no preview will be shown until you "
               "open a replacement image.").arg(imagePath));
    }

    // ---- Commit the load -------------------------------------------------
    // Suppress dirty-marking while we update m_look + UI. The debounce kick
    // inside applyLookToUi would otherwise call markDirty().
    m_isLoadingProject = true;

    m_look = loadedLook;
    m_currentImagePath = imagePath;
    m_currentProjectPath = path;

    if (imageLoaded) {
        m_originalFullRes = img;
        const int longest = std::max(img.width(), img.height());
        m_previewSource = (longest <= kPreviewMaxEdge)
            ? img
            : img.scaled(kPreviewMaxEdge, kPreviewMaxEdge,
                         Qt::KeepAspectRatio, Qt::SmoothTransformation);
    } else {
        // No image — clear preview state so the editor doesn't try to render
        // against stale pixels from a previous session.
        m_originalFullRes = QImage();
        m_previewSource = QImage();
        m_processed = QImage();
        // Also clear the histogram so it doesn't keep showing the previous
        // image's distribution.
        if (m_histogramWidget) m_histogramWidget->setImage(QImage());
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
    refreshUndoRedoActions();

    // Push the loaded values into all UI widgets, then trigger a render.
    // applyLookToUi() kicks the debounce; that fires onDebounceFired which
    // will short-circuit the markDirty() because m_isLoadingProject is true.
    applyLookToUi();

    if (imageLoaded) {
        requestRender();
    } else if (m_previewLabel) {
        m_previewLabel->setOriginalImage(QImage());
        m_previewLabel->setEditedImage(QImage());
    }

    m_isLoadingProject = false;

    m_projectDirty = false;
    updateWindowTitle();
    return true;
}

// ==============================================================================
// Dirty state + window title
// ==============================================================================
void MainWindow::markDirty()
{
    if (m_projectDirty) return;   // already dirty, nothing to do
    m_projectDirty = true;
    updateWindowTitle();
}

void MainWindow::updateWindowTitle()
{
    QString projectLabel;
    if (m_currentProjectPath.isEmpty()) {
        projectLabel = tr("Untitled");
    } else {
        projectLabel = QFileInfo(m_currentProjectPath).fileName();
    }
    const QString dirtyMark = m_projectDirty ? QStringLiteral(" *") : QString();
    setWindowTitle(tr("Lumen Photo Studio — %1%2").arg(projectLabel, dirtyMark));
}

bool MainWindow::maybePromptUnsavedChanges()
{
    if (!m_projectDirty) return true;

    const auto reply = QMessageBox::question(
        this, tr("Unsaved Changes"),
        tr("This project has unsaved changes. Save before continuing?"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Save);

    if (reply == QMessageBox::Save) {
        if (m_currentProjectPath.isEmpty()) {
            // No path yet — Save As. If the user cancels the file dialog,
            // saveProjectToPath never runs, and m_projectDirty stays true.
            // That's our signal to treat the whole flow as cancelled.
            onSaveProjectAs();
            return !m_projectDirty;
        }
        return saveProjectToPath(m_currentProjectPath);
    }
    if (reply == QMessageBox::Discard) return true;
    return false;   // Cancel
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (maybePromptUnsavedChanges()) {
        event->accept();
    } else {
        event->ignore();
    }
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
    if (!m_isLoadingProject) markDirty();
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

    // Mirror to the secondary viewer if it's open. The viewer holds an
    // implicitly-shared copy of the QImage — no per-pixel work, just a
    // refcount bump. setImage() repaints if the image actually changed.
    if (m_secondaryViewer) m_secondaryViewer->setImage(m_processed);
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
                this, [this]() { showPlaceholder(this, tr("Node Graph")); });
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

    // Disable the opacity slider and Clear button when no LUT is loaded —
    // they have nothing to act on. The Load LUT button stays enabled.
    m_lutOpacitySlider->setEnabled(hasLut);
    if (m_lutClearBtn) m_lutClearBtn->setEnabled(hasLut);
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
};
constexpr WheelFields kWheels[4] = {
    { &lps::GradingParams::shadowsHue,    &lps::GradingParams::shadowsSaturation,    &lps::GradingParams::shadowsStrength    },
    { &lps::GradingParams::midtonesHue,   &lps::GradingParams::midtonesSaturation,   &lps::GradingParams::midtonesStrength   },
    { &lps::GradingParams::highlightsHue, &lps::GradingParams::highlightsSaturation, &lps::GradingParams::highlightsStrength },
    { &lps::GradingParams::globalHue,     &lps::GradingParams::globalSaturation,     &lps::GradingParams::globalStrength     },
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
    // body's visibility. We use a QToolButton (not a QPushButton) so we
    // can keep the styling minimal — the button reads as a clickable
    // header rather than a chunky 3D button.
    auto* header = new QToolButton(parent);
    header->setText(QString::fromUtf8("▾ ") + title);
    header->setToolButtonStyle(Qt::ToolButtonTextOnly);
    header->setAutoRaise(true);
    header->setCursor(Qt::PointingHandCursor);
    header->setStyleSheet(
        "QToolButton { color: #b0b0b6; text-align: left; padding: 2px 4px; }"
        "QToolButton:hover { color: #d0d0d4; }");
    header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    col->addWidget(header);
    w.header = header;

    // Body: three sliders in a column, parented to a container so we can
    // toggle their visibility as a group via a single setVisible call.
    auto* box = new QWidget(parent);
    auto* boxLay = new QVBoxLayout(box);
    boxLay->setContentsMargins(8, 0, 0, 0);   // small left indent
    boxLay->setSpacing(2);

    boxLay->addWidget(buildSliderRow(tr("Hue"),
                                     0, 360, 0,
                                     w.hue, w.hueValue));
    boxLay->addWidget(buildSliderRow(tr("Saturation"),
                                     0, 100, 0,
                                     w.sat, w.satValue));
    boxLay->addWidget(buildSliderRow(tr("Strength"),
                                     0, 100, 0,
                                     w.str, w.strValue));
    col->addWidget(box);
    w.slidersBox = box;
    w.expanded = true;

    // Header toggle: hide/show the body. Update the chevron glyph so users
    // see the state at a glance.
    connect(header, &QToolButton::clicked, this,
            [this, wheelIndex, title]() {
        auto& wi = m_gradingWheels[wheelIndex];
        wi.expanded = !wi.expanded;
        if (wi.slidersBox) wi.slidersBox->setVisible(wi.expanded);
        if (wi.header) wi.header->setText(
            (wi.expanded ? QString::fromUtf8("▾ ")
                         : QString::fromUtf8("▸ ")) + title);
    });

    // Slider wiring. Each slider:
    //   - sliderPressed → pushUndoSnapshot (one snapshot per drag)
    //   - valueChanged  → write to m_look.grading via the wheel's
    //                     pointer-to-member, update label, kick debounce
    connect(w.hue, &QSlider::sliderPressed,
            this, &MainWindow::pushUndoSnapshot);
    connect(w.hue, &QSlider::valueChanged, this,
            [this, fields, &w](int v) {
        m_look.grading.*(fields.hue) = static_cast<float>(v);
        if (w.hueValue) w.hueValue->setText(QString::number(v));
        if (m_debounce) m_debounce->start();
    });

    connect(w.sat, &QSlider::sliderPressed,
            this, &MainWindow::pushUndoSnapshot);
    connect(w.sat, &QSlider::valueChanged, this,
            [this, fields, &w](int v) {
        m_look.grading.*(fields.sat) = static_cast<float>(v);
        if (w.satValue) w.satValue->setText(QString::number(v));
        if (m_debounce) m_debounce->start();
    });

    connect(w.str, &QSlider::sliderPressed,
            this, &MainWindow::pushUndoSnapshot);
    connect(w.str, &QSlider::valueChanged, this,
            [this, fields, &w](int v) {
        m_look.grading.*(fields.str) = static_cast<float>(v);
        if (w.strValue) w.strValue->setText(QString::number(v));
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
        if (!w.hue || !w.sat || !w.str) continue;

        const int hueV = static_cast<int>(std::lround(m_look.grading.*(f.hue)));
        const int satV = static_cast<int>(std::lround(m_look.grading.*(f.sat)));
        const int strV = static_cast<int>(std::lround(m_look.grading.*(f.str)));

        { QSignalBlocker b(w.hue); w.hue->setValue(hueV); }
        if (w.hueValue) w.hueValue->setText(QString::number(hueV));
        { QSignalBlocker b(w.sat); w.sat->setValue(satV); }
        if (w.satValue) w.satValue->setText(QString::number(satV));
        { QSignalBlocker b(w.str); w.str->setValue(strV); }
        if (w.strValue) w.strValue->setText(QString::number(strV));
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
}

// ==============================================================================
// Edit-menu slots
// ==============================================================================
void MainWindow::onResetEdits()
{
    pushUndoSnapshot();
    m_look = lps::Look{};
    applyLookToUi();
    refreshUndoRedoActions();
    if (m_presetNameLabel)
        m_presetNameLabel->setText(tr("(no preset loaded)"));
}

namespace {

// QImage rotation/flip helpers. Wrap QImage::transformed so the call sites
// stay readable. All transforms preserve format and alpha.
inline QImage rotated90(const QImage& src, bool clockwise)
{
    QTransform t;
    t.rotate(clockwise ? 90.0 : -90.0);
    return src.transformed(t, Qt::SmoothTransformation);
}
inline QImage flipped(const QImage& src, bool horizontal)
{
    return src.mirrored(horizontal, !horizontal);
}

} // namespace

void MainWindow::onRotateLeft()
{
    if (m_originalFullRes.isNull()) return;
    m_originalFullRes = rotated90(m_originalFullRes, /*clockwise=*/false);
    if (!m_previewSource.isNull())
        m_previewSource = rotated90(m_previewSource, false);
    m_processed = QImage();   // dimensions changed; render will refresh
    if (m_previewLabel) m_previewLabel->zoomToFit();   // dimensions changed
    markDirty();
    requestRender();
}

void MainWindow::onRotateRight()
{
    if (m_originalFullRes.isNull()) return;
    m_originalFullRes = rotated90(m_originalFullRes, /*clockwise=*/true);
    if (!m_previewSource.isNull())
        m_previewSource = rotated90(m_previewSource, true);
    m_processed = QImage();
    if (m_previewLabel) m_previewLabel->zoomToFit();
    markDirty();
    requestRender();
}

void MainWindow::onFlipHorizontal()
{
    if (m_originalFullRes.isNull()) return;
    m_originalFullRes = flipped(m_originalFullRes, /*horizontal=*/true);
    if (!m_previewSource.isNull())
        m_previewSource = flipped(m_previewSource, true);
    m_processed = QImage();
    markDirty();
    requestRender();
}

void MainWindow::onFlipVertical()
{
    if (m_originalFullRes.isNull()) return;
    m_originalFullRes = flipped(m_originalFullRes, /*horizontal=*/false);
    if (!m_previewSource.isNull())
        m_previewSource = flipped(m_previewSource, false);
    m_processed = QImage();
    markDirty();
    requestRender();
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

    pushUndoSnapshot();
    m_look = loaded;
    applyLookToUi();
    refreshUndoRedoActions();
}

void MainWindow::onPreferences()
{
    showPlaceholder(this, tr("Preferences"));
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
// Window-menu slots
// ==============================================================================
void MainWindow::onResetWorkspaceLayout()
{
    // Only persistent layout state today is the sidebar collapse. Reset =
    // expand the sidebar and show the histogram.
    if (m_sidebarCollapsed) onToggleSidebar();
    if (m_histogramWidget)  m_histogramWidget->setVisible(true);
    if (m_actShowHistogram) m_actShowHistogram->setChecked(true);
    if (m_actShowControls)  m_actShowControls->setChecked(true);
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
    QMessageBox box(this);
    box.setWindowTitle(tr("Lumen Plugin Manager"));
    box.setText(tr("Plugin management is not yet available.\n\n"
                   "Future versions will let you browse, enable, "
                   "disable, and update plugins from this dialog."));
    box.setIcon(QMessageBox::Information);
    box.exec();
}

void MainWindow::onInstallPlugin()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Install Plugin"),
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation),
        tr("Lumen Plugins (*.plugin);;All Files (*)"));
    if (path.isEmpty()) return;
    // Future: copy to plugins folder and reload. Placeholder for now.
    QMessageBox::information(this, tr("Install Plugin"),
        tr("Plugin installation is not yet implemented.\n\n"
           "Selected: %1").arg(path));
}

void MainWindow::onReloadPlugins()
{
    // Placeholder — no plugin runtime exists yet.
    showPlaceholder(this, tr("Reload Plugins"));
}

void MainWindow::onOpenPluginsFolder()
{
    // Resolve plugins folder relative to the running executable. Create
    // it if missing so users can drop files in immediately.
    const QString pluginsDir =
        QCoreApplication::applicationDirPath() + QStringLiteral("/plugins");
    QDir dir(pluginsDir);
    if (!dir.exists()) {
        if (!dir.mkpath(QStringLiteral("."))) {
            QMessageBox::warning(this, tr("Open Plugins Folder"),
                tr("Could not create plugins folder:\n%1").arg(pluginsDir));
            return;
        }
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(pluginsDir));
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
        m_sidebarStack->setCurrentWidget(m_sidebarMini);
        m_sidebarHost->setMaximumWidth(40);
    } else {
        m_sidebarStack->setCurrentWidget(m_sidebarFull);
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
