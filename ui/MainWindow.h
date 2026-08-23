// ==============================================================================
// ui/MainWindow.h
// Minimal Qt Widgets editor for the Lumen Photo Studio engine.
//
// Wires a set of tone sliders to a lps::Look, renders the preview via
// lps::ImagePipeline on a background thread, and displays the result in a
// QLabel-backed preview area.
//
// Design notes:
//   - The original full-res QImage is kept for Save. A preview-sized copy
//     (max 1800px on the longest edge) is what slider drags render against,
//     so interactive editing doesn't scale with image size.
//   - Render runs on a worker thread via QtConcurrent. A monotonically
//     increasing generation counter lets us discard stale results if the
//     user keeps moving sliders while a render is in flight.
//   - A single 30ms debounce timer coalesces rapid slider signals before
//     kicking off a render.
// ==============================================================================
#pragma once

#include "core/Look.h"
#include "local/LocalAdjustmentEngine.h"

#include <QDateTime>
#include <QImage>
#include <QMainWindow>
#include <QVBoxLayout>

#include <memory>
#include <vector>

QT_BEGIN_NAMESPACE
class LibraryView;   // ui/LibraryView.h — the catalog workspace
class QAction;
class QCheckBox;
class QDragEnterEvent;
class QDockWidget;
class QDropEvent;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QMenu;
class QObject;
class QPushButton;
class QScrollArea;
class QSlider;
class QStackedLayout;
class QStackedWidget;
class QTabBar;
class QTabWidget;
class QTimer;
class QToolButton;
class QWidget;
class QComboBox;
QT_END_NAMESPACE

class CurveEditorWidget;
class ColorWheelWidget;
class HistogramWidget;
class EmptyStateOverlay;
class NodeGraphWidget;
class PreviewWidget;
class SecondaryViewerWindow;
class WelcomeScreenWidget;

namespace lps {
class AutosaveManager;
struct ProjectDocument;
class PluginManager;
class SettingsManager;
}

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    // Switches to the Library (catalog) workspace, opening the catalog
    // on first use. Public so the --smoke-test path can exercise the
    // catalog in CI, where there is no one to click the rail button.
    void showLibraryWorkspace();
    void showEditorWorkspace();
    // Public so --screenshot can populate the editor; an empty editor
    // makes a poor screenshot of a photo application.
    bool loadImageFromPath(const QString& path);

    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    // Rescale the displayed preview when the window resizes. The cached
    // processed image is repainted scaled to the label's current size.
    void resizeEvent(QResizeEvent* event) override;

    // Press-and-hold Spacebar to temporarily view the unedited original.
    // Auto-repeat events are ignored so holding the key doesn't toggle.
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;

    // Application-wide event filter. Forwards Space key events to our
    // keyPressEvent/keyReleaseEvent overrides regardless of which child
    // widget currently has keyboard focus. Without this, pressing space
    // while a slider has focus would never reach MainWindow because the
    // slider would consume the event.
    bool eventFilter(QObject* watched, QEvent* event) override;

    // Prompts to save unsaved changes. If the user picks Cancel, the close
    // is blocked via event->ignore().
    void closeEvent(QCloseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private slots:
    // Fires after the debounce interval. Snapshots the current Look and
    // launches a worker render. Also marks the project dirty (unless we're
    // mid-load).
    void onDebounceFired();

private:
    // ---- UI construction ----------------------------------------------------
    void buildUi();
    QWidget* buildControlPanel();
    QWidget* buildAnalysisPanel();
    void showWelcomeScreen();

    void updateBottomWorkspaceVisibility();
    void setBottomWorkspaceCollapsed(bool collapsed);
    void setAnalysisPanelCollapsed(bool collapsed);
    void handleRailAction(const QString& action);
    void scrollInspectorTo(QWidget* section);
    void refreshWelcomeRecentFiles();
    void updateNavigatorPreview();
    void updateMetadataPanel();

    // Helper: build one labeled slider row. `onChange` is invoked in response
    // to valueChanged signals (with the slider's integer value); each caller
    // translates that to the matching lps::Look field and restarts the
    // debounce.
    QWidget* buildSliderRow(const QString& label,
                            int minValue, int maxValue, int initialValue,
                            QSlider*& outSlider,
                            QLabel*& outValueLabel);

    // ---- Render plumbing ----------------------------------------------------
    // Kick a render if no worker is currently in flight. If one is, flags
    // that another pass is needed when the current one finishes.
    void requestRender();

    // Worker-side entry: runs on a QtConcurrent thread. Receives a snapshot
    // of the Look and the current preview image; returns the rendered frame.
    // Static (or pure function with captured state by value) so no shared
    // mutable state gets touched off-thread.
    static QImage renderOnWorker(QImage source, lps::Look look);

    // Called on the GUI thread when a worker render completes. Decides
    // whether to display the result or discard it as stale.
    void onRenderFinished(quint64 generation, QImage result);

    // Update the preview QLabel from m_processed, scaling to label size.
    void refreshPreviewLabel();

    // ---- State --------------------------------------------------------------
    lps::Look m_look;                 // current edit state — UI source of truth

    QImage    m_originalFullRes;      // full-res original from disk (for Save)
    QImage    m_previewSource;        // preview-sized copy (interactive path)
    QImage    m_processed;            // last successful preview render

    // Press-and-hold Spacebar toggles which of the above two is shown.
    // True while the user is holding space → display m_previewSource;
    // false (default) → display m_processed. No re-render is triggered
    // by this toggle — it swaps the QLabel's contents from the cache.
    bool      m_showOriginal = false;

    // Generation counter — bumped on each requestRender. Workers carry the
    // generation they started with; if the counter has moved past that value
    // when they finish, their result is stale and dropped.
    quint64   m_generation = 0;
    bool      m_renderInFlight = false;
    bool      m_pendingRender  = false;

    // ---- Widgets (non-owning once parented) ---------------------------------
    // m_previewLabel is the image-display surface (PreviewWidget; handles
    // its own zoom/pan/fit logic). m_emptyState is an overlay widget
    // parented to the preview that paints the welcome screen when no
    // image is loaded; it's hidden after loadImageFromPath succeeds and
    // shown again when the image is cleared.
    //
    // Designed as an overlay (not a PreviewWidget subclass) so future
    // multi-tab / multi-document workflows can re-parent the same overlay
    // across multiple preview surfaces without each surface needing to
    // know about empty-state painting.
    PreviewWidget*     m_previewLabel = nullptr;
    EmptyStateOverlay* m_emptyState   = nullptr;

    // ---- Workspace shell ----------------------------------------------------
    // Professional photo-editor frame: top workspace bar, left icon rail,
    // centered canvas, right inspector, and bottom dock tabs.
    QStackedWidget*      m_workspaceStack = nullptr;

    // Library (catalog) workspace — stack index 2, alongside the
    // welcome screen (0) and the editor (1).
    LibraryView*         m_libraryView    = nullptr;
    int                  m_libraryIndex   = -1;
    WelcomeScreenWidget* m_welcomeScreen  = nullptr;
    QWidget*             m_editorWorkspace = nullptr;
    QWidget*             m_workspaceBar   = nullptr;
    QWidget*             m_toolRail       = nullptr;
    QWidget*             m_analysisPanel  = nullptr;
    QToolButton*         m_analysisCollapseBtn = nullptr;
    QScrollArea*         m_analysisScroll = nullptr;
    bool                 m_analysisPanelCollapsed = false;
    QTabWidget*          m_bottomPanelTabs = nullptr;
    QWidget*             m_bottomWorkspaceContainer = nullptr;
    QToolButton*         m_bottomWorkspaceCollapseBtn = nullptr;
    QTabBar*            m_documentTabs = nullptr;
    QListWidget*         m_historyList = nullptr;
    bool                 m_editorWorkspaceActive = false;
    bool                 m_bottomWorkspaceEnabled = true;
    bool                 m_bottomWorkspaceCollapsed = false;
    bool                 m_syncingBottomWorkspaceVisibility = false;
    bool                 m_syncingDocumentTabs = false;

    // ---- Sidebar (controls panel) collapse ----------------------------------
    // The full controls panel and a narrow icon-strip "collapsed" view both
    // live as siblings inside m_sidebarStack (a QStackedLayout). Switching
    // between them preserves widget state — sliders, HSL selection, curve
    // editor, etc. all stay alive across collapse/expand cycles.
    QWidget*        m_sidebarHost  = nullptr;   // outer container
    QStackedLayout* m_sidebarStack = nullptr;
    QWidget*        m_sidebarFull  = nullptr;   // the 320px controls panel
    QWidget*        m_sidebarMini  = nullptr;   // the ~36px collapsed strip
    QScrollArea*    m_controlScroll = nullptr;
    bool            m_sidebarCollapsed = false;

    QWidget* m_toneSection = nullptr;
    QWidget* m_colorSection = nullptr;
    QWidget* m_hslSection = nullptr;
    QWidget* m_curvesSection = nullptr;
    QWidget* m_gradingSection = nullptr;
    QWidget* m_lensSection = nullptr;
    QWidget* m_detailsSection = nullptr;
    QWidget* m_transformSection = nullptr;
    QWidget* m_masksSection = nullptr;
    QWidget* m_layersSection = nullptr;

    QLabel* m_navigatorPreview = nullptr;
    QLabel* m_metaFileName = nullptr;
    QLabel* m_metaDimensions = nullptr;
    QLabel* m_metaDateTime = nullptr;
    QLabel* m_metaIso = nullptr;
    QLabel* m_metaFocalLength = nullptr;
    QLabel* m_metaAperture = nullptr;
    QLabel* m_metaShutterSpeed = nullptr;
    QLabel* m_metaCameraModel = nullptr;
    QLabel* m_metaLensModel = nullptr;

    // Small status readout in the control panel: "Edited" when viewing the
    // processed preview, "Original" while Spacebar is held.
    QLabel*   m_viewModeLabel = nullptr;

    // ---- Menu actions -------------------------------------------------------
    // Stored as members so multiple menus + the preview context menu can
    // share the same action object (one source of truth for text, shortcut,
    // enabled state). Also lets us toggle Undo/Redo enabled-state from
    // pushUndoSnapshot/undo/redo without a search-and-update.
    //
    // Only actions referenced after construction are stored here. One-shot
    // actions (Open Image, Exit, etc.) are created locally inside
    // buildMenus and connected via lambda; their pointers are not kept.
    QAction* m_actUndo               = nullptr;
    QAction* m_actRedo               = nullptr;
    QAction* m_actResetEdits         = nullptr;
    QAction* m_actCopyLook           = nullptr;
    QAction* m_actPasteLook          = nullptr;
    QAction* m_actRotateLeft         = nullptr;
    QAction* m_actRotateRight        = nullptr;
    QAction* m_actFlipHorizontal     = nullptr;
    QAction* m_actFlipVertical       = nullptr;
    QAction* m_actZoomFit            = nullptr;
    QAction* m_actZoom100            = nullptr;
    QAction* m_actZoomIn             = nullptr;
    QAction* m_actZoomOut            = nullptr;
    QAction* m_actBeforeAfter        = nullptr;
    QAction* m_actShowHistogram      = nullptr;
    QAction* m_actShowControls       = nullptr;   // toggles sidebar collapse
    QAction* m_actShowBottomWorkspace = nullptr;
    QAction* m_actFullscreen         = nullptr;
    QAction* m_actOpenImage          = nullptr;
    QAction* m_actExportImage        = nullptr;

    // Build all top-level menus (File, Edit, View, Window, Plugins, Help)
    // and their actions. Called from buildUi() before the central widget
    // layout. Connects every action to its slot at the same time.
    void buildMenus();

    // Update Undo/Redo action enabled state from the stack contents. Called
    // after pushUndoSnapshot/undo/redo and after applyLookToUi.
    void refreshUndoRedoActions();

    // Live histogram of the latest processed preview. Updated whenever
    // m_processed is reassigned (after a render, or cleared on project load
    // with no source image).
    HistogramWidget* m_histogramWidget = nullptr;

    // Optional second-monitor preview window. Lazy-created on first
    // View → Secondary Viewer trigger; hidden on close (not destroyed),
    // so subsequent menu activations re-show the same window with
    // its position and size preserved across close/reopen cycles.
    //
    // null until the user opens it for the first time. After that, the
    // pointer remains valid for the lifetime of MainWindow (the window
    // is parented as a child for cleanup, but flagged Qt::Window so
    // it lives as an independent top-level window movable to any
    // monitor).
    SecondaryViewerWindow* m_secondaryViewer = nullptr;

    // Node Graph foundation. Lazy-created on first activation from the
    // Window menu (or future sidebar icon click). Hosted in a QDockWidget
    // so it can be docked at the bottom or floated to a second monitor.
    // V1 is visual only — no render hookup.
    NodeGraphWidget* m_nodeGraph     = nullptr;
    QDockWidget*     m_nodeGraphDock = nullptr;

    // Bring-your-own-key AI assistant. Self-contained: the panel owns its
    // own client and settings, so MainWindow only hosts the dock.
    QDockWidget*     m_aiDock        = nullptr;

    std::unique_ptr<lps::PluginManager> m_pluginManager;
    std::unique_ptr<lps::SettingsManager> m_settings;
    std::unique_ptr<lps::AutosaveManager> m_autosaveManager;

    QSlider*  m_exposureSlider   = nullptr;
    QSlider*  m_contrastSlider   = nullptr;
    QSlider*  m_highlightsSlider = nullptr;
    QSlider*  m_shadowsSlider    = nullptr;
    QSlider*  m_whitesSlider     = nullptr;
    QSlider*  m_blacksSlider     = nullptr;
    QSlider*  m_brightnessSlider = nullptr;

    QSlider*  m_temperatureSlider = nullptr;
    QSlider*  m_tintSlider        = nullptr;
    QSlider*  m_vibranceSlider    = nullptr;
    QSlider*  m_saturationSlider  = nullptr;

    QLabel*   m_exposureValue   = nullptr;
    QLabel*   m_contrastValue   = nullptr;
    QLabel*   m_highlightsValue = nullptr;
    QLabel*   m_shadowsValue    = nullptr;
    QLabel*   m_whitesValue     = nullptr;
    QLabel*   m_blacksValue     = nullptr;
    QLabel*   m_brightnessValue = nullptr;

    QLabel*   m_temperatureValue = nullptr;
    QLabel*   m_tintValue        = nullptr;
    QLabel*   m_vibranceValue    = nullptr;
    QLabel*   m_saturationValue  = nullptr;

    // ---- Curve editor -------------------------------------------------------
    // Replaces the four "curve bias" sliders from the earlier step. A single
    // CurveEditorWidget instance handles all four channels; the four buttons
    // switch which CurvePoints it's editing by calling setCurve().
    //
    // m_selectedCurveChannel indexes into kCurveChannels[] in MainWindow.cpp's
    // anonymous namespace — the same pattern we use for the HSL channel
    // selector.
    int       m_selectedCurveChannel = 0;   // 0 = Master, default

    static constexpr int kCurveChannelCount = 4;
    QPushButton* m_curveChannelButtons[kCurveChannelCount] {};

    class CurveEditorWidget* m_curveEditor = nullptr;

    // Point the editor at the correct CurvePoints in m_look, update the
    // accent color for the new channel, and sync button check states. Called
    // on tab click and once at construction for the default selection.
    void selectCurveChannel(int channelIndex);

    // ---- COLOR GRADING (LUT) widgets ----------------------------------------
    // The slider drives m_look.grading.lutOpacity (mapped 0..100 → 0..1f).
    // The label shows the basename of the loaded LUT, or "(none)". Both
    // are kept as members so applyLookToUi can refresh them after undo /
    // redo / preset load.
    QSlider*  m_lutOpacitySlider = nullptr;
    QLabel*   m_lutOpacityValue  = nullptr;
    QLabel*   m_lutNameLabel     = nullptr;
    QPushButton* m_lutLoadBtn    = nullptr;
    QPushButton* m_lutClearBtn   = nullptr;
    QCheckBox*   m_lutEnabledCheck = nullptr;   // master on/off; preserves opacity

    // ---- Advanced grading (DaVinci-style) — V1 placeholders -----------------
    // Sliders write to m_look.grading.{lift,gamma,gain,offset}. Engine
    // math is a follow-up; values persist through save/load and undo/redo.
    QSlider* m_liftSlider   = nullptr;  QLabel* m_liftValue   = nullptr;
    QSlider* m_gammaSlider  = nullptr;  QLabel* m_gammaValue  = nullptr;
    QSlider* m_gainSlider   = nullptr;  QLabel* m_gainValue   = nullptr;
    QSlider* m_offsetSlider = nullptr;  QLabel* m_offsetValue = nullptr;

    // ---- Filmic look controls — V1 placeholders ----------------------------
    QSlider* m_filmicContrastSlider   = nullptr; QLabel* m_filmicContrastValue   = nullptr;
    QSlider* m_highlightRolloffSlider = nullptr; QLabel* m_highlightRolloffValue = nullptr;
    QSlider* m_shadowLiftSlider       = nullptr; QLabel* m_shadowLiftValue       = nullptr;
    QSlider* m_fadeBlacksSlider       = nullptr; QLabel* m_fadeBlacksValue       = nullptr;
    QSlider* m_colorSeparationSlider  = nullptr; QLabel* m_colorSeparationValue  = nullptr;

    // ---- Lens correction (master + per-control widgets) --------------------
    // Vignetting is the only V1-active engine; the rest persist as data.
    // Sliders are disabled in the UI when the master "Enable Lens
    // Corrections" checkbox is off — same enable-gating pattern as the
    // LUT controls.
    QCheckBox* m_lensEnabledCheck    = nullptr;
    QCheckBox* m_lensRemoveCaCheck   = nullptr;
    QSlider*   m_lensDistortionSlider = nullptr; QLabel* m_lensDistortionValue = nullptr;
    QSlider*   m_lensVignettingSlider = nullptr; QLabel* m_lensVignettingValue = nullptr;
    QSlider*   m_lensPurpleFringeSlider = nullptr; QLabel* m_lensPurpleFringeValue = nullptr;
    QSlider*   m_lensGreenFringeSlider  = nullptr; QLabel* m_lensGreenFringeValue  = nullptr;

    // Refresh lens widgets from m_look.lens. Called from applyLookToUi
    // after undo/redo / preset load. Signal-blocked internally.
    void refreshLensWidgets();

    // ---- Transform (crop / rotate / flip / straighten) --------------------
    QPushButton* m_transformFlipHorizontalBtn = nullptr;
    QPushButton* m_transformFlipVerticalBtn   = nullptr;
    QPushButton* m_cropToolBtn                = nullptr;
    QComboBox*   m_cropAspectCombo            = nullptr;
    QCheckBox*   m_cropLockAspectCheck        = nullptr;
    QSlider*     m_straightenSlider           = nullptr;
    QLabel*      m_straightenValue            = nullptr;

    void refreshTransformWidgets();
    void updateCropAspectConstraint();

    // ---- HDR tone mapping -------------------------------------------------
    QCheckBox* m_hdrEnabledCheck = nullptr;
    QSlider*   m_hdrExposureBiasSlider = nullptr; QLabel* m_hdrExposureBiasValue = nullptr;
    QSlider*   m_hdrHighlightCompressionSlider = nullptr; QLabel* m_hdrHighlightCompressionValue = nullptr;
    QSlider*   m_hdrShoulderStrengthSlider = nullptr; QLabel* m_hdrShoulderStrengthValue = nullptr;
    QSlider*   m_hdrMidtonePivotSlider = nullptr; QLabel* m_hdrMidtonePivotValue = nullptr;
    QSlider*   m_hdrSaturationPreserveSlider = nullptr; QLabel* m_hdrSaturationPreserveValue = nullptr;

    void refreshHdrWidgets();

    // ---- Details (sharpening + noise reduction) ---------------------------
    QSlider* m_sharpeningAmountSlider  = nullptr; QLabel* m_sharpeningAmountValue  = nullptr;
    QSlider* m_sharpeningRadiusSlider  = nullptr; QLabel* m_sharpeningRadiusValue  = nullptr;
    QSlider* m_sharpeningDetailSlider  = nullptr; QLabel* m_sharpeningDetailValue  = nullptr;
    QSlider* m_sharpeningMaskingSlider = nullptr; QLabel* m_sharpeningMaskingValue = nullptr;
    QSlider* m_luminanceNrSlider       = nullptr; QLabel* m_luminanceNrValue       = nullptr;
    QSlider* m_luminanceDetailSlider   = nullptr; QLabel* m_luminanceDetailValue   = nullptr;
    QSlider* m_colorNrSlider           = nullptr; QLabel* m_colorNrValue           = nullptr;
    QSlider* m_colorDetailSlider       = nullptr; QLabel* m_colorDetailValue       = nullptr;

    void refreshDetailsWidgets();

    // Most-recently-loaded preset filename (display only). Updated by
    // onLoadPreset on success; reset to "(no preset loaded)" on Reset Edits
    // so it doesn't lie about what's actually applied.
    QLabel*   m_presetNameLabel  = nullptr;

    // ---- 3-way color grading widgets ----------------------------------------
    // Four wheels (Shadows / Midtones / Highlights / Global), each with
    // hue, saturation, strength sliders. Plus global Balance and Blending.
    //
    // Indexed by wheel via the kGradingWheel enum (declared in the .cpp's
    // anonymous namespace). Storing pointers as flat arrays rather than
    // 12 individually-named members keeps the code more compact and lets
    // refreshGradingWidgets iterate generically.
    static constexpr int kGradingWheelCount = 4;
    struct GradingWheelWidgets {
        ColorWheelWidget* wheel = nullptr;
        QLabel*  hueValue   = nullptr;   // numeric readout near the wheel
        QLabel*  satValue   = nullptr;   // numeric readout near the wheel
        QSlider* str        = nullptr;
        QLabel*  strValue   = nullptr;
        QSlider* lum        = nullptr;
        QLabel*  lumValue   = nullptr;
        QPushButton* resetBtn = nullptr;
        QWidget* slidersBox = nullptr;   // collapsible body (hidden when not expanded)
        QToolButton* header = nullptr;   // expand/collapse header
        bool     expanded   = true;
    };
    GradingWheelWidgets m_gradingWheels[kGradingWheelCount];

    QSlider* m_balanceSlider  = nullptr;
    QLabel*  m_balanceValue   = nullptr;
    QSlider* m_blendingSlider = nullptr;
    QLabel*  m_blendingValue  = nullptr;

    // Build a single wheel block and wire its sliders to the right Look
    // fields. Called four times during buildControlPanel.
    void buildGradingWheel(QWidget* parent, QVBoxLayout* col,
                           int wheelIndex,
                           const QString& title);

    // Refresh all wheel + balance/blending widgets from m_look.grading.
    // Called by applyLookToUi so undo/redo/preset-load updates the UI.
    void refreshGradingWidgets();

    // Refresh the LUT widgets from m_look.grading. Called by applyLookToUi
    // after any non-user-initiated mutation (undo/redo, preset load).
    void refreshLutWidgets();

    // ---- Local masks ---------------------------------------------------------
    // List of masks (one row per LocalAdjustment), with checkbox per row
    // for enabled-state toggle. Selecting a row drives the mask-detail
    // sliders below the list. Add/Delete buttons and an inline status
    // label round out the section.
    QListWidget* m_maskList            = nullptr;
    QPushButton* m_maskAddLinearBtn    = nullptr;
    QPushButton* m_maskAddRadialBtn    = nullptr;
    QPushButton* m_maskAddBrushBtn     = nullptr;
    QPushButton* m_maskDeleteBtn       = nullptr;
    QLabel*      m_maskStatusLabel     = nullptr;

    // Per-mask sliders. These edit the currently-selected mask's adjustment
    // values. Kept enabled only when a mask is selected.
    QSlider* m_maskExposureSlider     = nullptr;  QLabel* m_maskExposureValue    = nullptr;
    QSlider* m_maskBrightnessSlider   = nullptr;  QLabel* m_maskBrightnessValue  = nullptr;
    QSlider* m_maskContrastSlider     = nullptr;  QLabel* m_maskContrastValue    = nullptr;
    QSlider* m_maskSaturationSlider   = nullptr;  QLabel* m_maskSaturationValue  = nullptr;
    QSlider* m_maskTemperatureSlider  = nullptr;  QLabel* m_maskTemperatureValue = nullptr;
    QSlider* m_maskTintSlider         = nullptr;  QLabel* m_maskTintValue        = nullptr;

    // Geometry / structural mask controls. Distinct from adjustment sliders:
    // these change WHERE the mask hits, not what it does.
    QLineEdit*   m_maskNameEdit       = nullptr;
    QCheckBox*   m_maskInvertCheck    = nullptr;
    QSlider*     m_maskFeatherSlider  = nullptr; QLabel* m_maskFeatherValue = nullptr;
    QSlider*     m_maskDensitySlider  = nullptr; QLabel* m_maskDensityValue = nullptr;
    QSlider*     m_maskFlowSlider     = nullptr; QLabel* m_maskFlowValue    = nullptr;
    QSlider*     m_maskBrushSizeSlider = nullptr; QLabel* m_maskBrushSizeValue = nullptr;
    QCheckBox*   m_maskBrushEraseCheck = nullptr;
    QPushButton* m_maskResetBrushBtn   = nullptr;
    QPushButton* m_maskResetGeoBtn    = nullptr;

    // Mask overlay UI controls. Drive PreviewWidget's overlay state.
    QCheckBox*   m_maskShowOverlayCheck   = nullptr;
    QSlider*     m_maskOverlayOpacitySlider = nullptr;
    QLabel*      m_maskOverlayOpacityValue  = nullptr;
    QComboBox*   m_maskViewModeCombo      = nullptr;

    // Index of the currently-selected mask in m_look.localAdjustments,
    // or -1 if no mask is selected. After undo/redo the index may need
    // clamping if the count shrank — refreshMaskWidgets handles this.
    int m_selectedMaskIndex = -1;

    // Slots for the masks section.
    void onAddLinearMask();
    void onAddRadialMask();
    void onAddBrushMask();
    void onDeleteSelectedMask();
    void onMaskListSelectionChanged();
    void onMaskItemChanged(QListWidgetItem* item);   // checkbox toggle
    void onMaskGeometryChangedFromPreview();         // from PreviewWidget signal
    void onResetMaskGeometry();                      // reset selected mask's geometry
    void onResetBrushMask();                         // clear selected brush strokes

    // Push the active-mask pointer to PreviewWidget. Called from
    // refreshMaskWidgets so the preview always reflects the selected
    // mask without an extra round trip.
    void syncActiveMaskToPreview();

    // Rebuild m_maskList rows from m_look.localAdjustments and refresh
    // the per-mask sliders from the currently-selected entry. Signal-
    // blocked internally so it doesn't kick the debounce.
    void refreshMaskWidgets();

    // Helper: append a new mask, take an undo snapshot, select it.
    void addMaskCommon(lps::LocalAdjustment&& mask, const QString& kind);

    // ---- Adjustment layers ---------------------------------------------------
    // Stackable layers UI. Each list row maps to one entry in
    // m_look.adjustmentLayers, with a checkbox for enabled-state and
    // selection driving the per-layer Opacity slider + Blend Mode combo.
    //
    // Layer rendering is V1-placeholder: data round-trips through save/
    // load and undo/redo, but the pipeline doesn't yet apply layer Looks
    // on top of the base. The follow-up step adds the compositing pass.
    QListWidget* m_layerList            = nullptr;
    QPushButton* m_layerAddBtn          = nullptr;
    QPushButton* m_layerDuplicateBtn    = nullptr;
    QPushButton* m_layerDeleteBtn       = nullptr;
    QSlider*     m_layerOpacitySlider   = nullptr;
    QLabel*      m_layerOpacityValue    = nullptr;
    QComboBox*   m_layerBlendModeCombo  = nullptr;
    QLabel*      m_layerStatusLabel     = nullptr;

    // Index of currently-selected layer in m_look.adjustmentLayers, or
    // -1 if no layer is selected. Clamped by refreshLayerWidgets after
    // any list-size change (undo/redo, delete).
    int m_selectedLayerIndex = -1;

    // Slots for the layers section.
    void onAddLayer();
    void onDuplicateLayer();
    void onDeleteSelectedLayer();
    void onLayerListSelectionChanged();
    void onLayerItemChanged(QListWidgetItem* item);
    void onLayerOpacityChanged(int v);
    void onLayerBlendModeChanged(int comboIndex);

    // Rebuild m_layerList from m_look.adjustmentLayers and refresh the
    // per-layer controls. Signal-blocked internally.
    void refreshLayerWidgets();

    // ---- HSL (selective color) controls -------------------------------------
    // Lightroom-style: eight channel-selector buttons, three sliders that
    // drive whichever channel is currently selected. Sliders are reused
    // across all eight channels — when the user switches the active channel,
    // the sliders are programmatically repositioned to reflect the stored
    // values for that channel (with signals blocked so no phantom renders
    // are triggered by the repositioning).
    //
    // m_selectedHslChannel indexes into kHslChannels[] in MainWindow.cpp's
    // anonymous namespace — same table that drives the pointer-to-member
    // dispatch when sliders are moved.
    int       m_selectedHslChannel = 0;   // 0 = Red, default

    static constexpr int kHslChannelCount = 8;
    QPushButton* m_hslChannelButtons[kHslChannelCount] {};

    QSlider*  m_hslHueSlider        = nullptr;
    QSlider*  m_hslSaturationSlider = nullptr;
    QSlider*  m_hslLuminanceSlider  = nullptr;

    QLabel*   m_hslHueValue        = nullptr;
    QLabel*   m_hslSaturationValue = nullptr;
    QLabel*   m_hslLuminanceValue  = nullptr;

    // Repopulate the three sliders from m_look.color.hsl for the given
    // channel, without firing valueChanged signals. Also updates the
    // checked-state of the channel buttons. Called on construction and
    // every time the user clicks a different channel.
    void selectHslChannel(int channelIndex);

    // ---- Targeted Color Adjustment Tool (HSL eyedropper) -------------------
    // The toggle button enables sampling mode in PreviewWidget. The
    // status label gives feedback ("Click color in image" → "Selected:
    // Orange" after sampling). Sampling does NOT trigger a render —
    // only the user dragging an HSL slider does.
    QPushButton* m_hslTargetButton = nullptr;
    QLabel*      m_hslTargetStatus = nullptr;

    // Slot for PreviewWidget::colorSampled. Handles the RGB→HSV→nearest-
    // channel mapping and drives selectHslChannel.
    void onColorSampled(QColor color, QPoint imagePos);

    QTimer*   m_debounce = nullptr;

    // ---- Undo / Redo ---------------------------------------------------------
    // The full Look is snapshotted at the start of each user-initiated edit
    // operation (slider press, curve-edit press). During the operation the
    // user continues to mutate m_look freely; one snapshot per operation
    // means one undo step per operation.
    //
    // pushUndoSnapshot() guards against re-entry via m_isApplyingLookToUi
    // so applyLookToUi's programmatic updates don't fabricate fake edits.
    std::vector<lps::Look> m_undoStack;
    std::vector<lps::Look> m_redoStack;

    static constexpr size_t kMaxUndoDepth = 100;

    struct HistoryEntry {
        QString label;
        lps::Look snapshot;
    };

    struct ImageDocument {
        QString imagePath;
        QString projectPath;
        QDateTime projectCreatedDate;
        QDateTime projectModifiedDate;
        QImage originalFullRes;
        QImage previewSource;
        QImage processed;
        lps::Look look;
        bool dirty = false;
        std::vector<lps::Look> undoStack;
        std::vector<lps::Look> redoStack;
        std::vector<HistoryEntry> historyEntries;
        int historyCurrentIndex = -1;
        QString nextHistoryLabel;
        int selectedMaskIndex = -1;
        int selectedLayerIndex = -1;
    };

    std::vector<ImageDocument> m_documents;
    int  m_activeDocumentIndex = -1;
    bool m_restoringDocument = false;

    std::vector<HistoryEntry> m_historyEntries;
    int     m_historyCurrentIndex = -1;
    bool    m_syncingHistorySelection = false;
    QString m_nextHistoryLabel;

    // True while applyLookToUi is actively pushing values into the UI.
    // Guards pushUndoSnapshot() so side-effects of programmatic widget
    // updates can't fabricate undo entries.
    bool m_isApplyingLookToUi = false;

    // Per-drag guard on the curve editor: editStarted can fire multiple
    // times in corner cases (reentrant events, focus changes); this flag
    // ensures exactly one snapshot per drag sequence.
    bool m_curveDragUndoCaptured = false;

    // Snapshot the current Look onto the undo stack. No-op while
    // m_isApplyingLookToUi is true. Clears the redo stack (any new edit
    // invalidates the redo chain). Caps the undo stack at kMaxUndoDepth.
    void pushUndoSnapshot();
    void recordHistoryStep(QString label);
    void clearHistory(const QString& baselineLabel = QString());
    void refreshHistoryList();
    void updateCurrentHistorySnapshot();
    QString historyLabelForSender(const QObject* senderObj) const;

    // Restore the previous Look from the undo stack. Current state moves
    // to the redo stack. Updates the UI and re-renders.
    void undo();

    // Restore the next Look from the redo stack. Current state moves
    // to the undo stack. Updates the UI and re-renders.
    void redo();

    // Push every UI control's visible state from m_look. Used after undo/redo
    // (and, prospectively, after preset load). Signals are blocked throughout
    // so these programmatic setValue()/setChecked() calls don't kick the
    // debounce or fabricate undo entries. At the end, a single debounce kick
    // triggers one re-render.
    void applyLookToUi();

    // ---- Project state -------------------------------------------------------
    // A "project" is an editable .lps file: a JSON envelope around the
    // source image path + the Look JSON LookSerializer already produces.
    // Distinct from "exporting an image" (which writes a baked PNG/JPG/etc.).
    //
    // m_currentImagePath: filesystem path of the loaded source image.
    //   Empty string when no image is loaded.
    // m_currentProjectPath: filesystem path of the .lps project this Look
    //   came from (or was saved to). Empty string for unsaved projects.
    // m_projectDirty: true when m_look has changed since the last save.
    //   Set in the debounce handler (the natural funnel for "user just
    //   edited something") and cleared on Save / Save As / Open Project.
    QString m_currentImagePath;
    QString m_currentProjectPath;
    QDateTime m_projectCreatedDate;
    QDateTime m_projectModifiedDate;
    bool    m_projectDirty = false;

    // True while we're loading a project from disk. Suppresses the dirty-mark
    // that the debounce handler would otherwise set when the load triggers
    // a re-render.
    bool    m_isLoadingProject = false;

    // ---- File / project actions ---------------------------------------------
    void onOpenImage();
    void onOpenProject();
    void onSaveProject();
    void onSaveProjectAs();
    void onExportImage();

    // Preset I/O — saves/loads just the Look JSON (no image path), useful
    // for sharing edit recipes across projects. Distinct from Save Project
    // which includes the source image path and project metadata.
    void onSavePreset();
    void onLoadPreset();

    // ---- LUT (color grading) actions ----------------------------------------
    // Load LUT prompts for a .cube file and writes its absolute path to
    // m_look.grading.lutPath. If opacity was 0 when the LUT loaded, it's
    // bumped to 1.0 so the LUT is immediately visible. Clear LUT empties
    // the path and resets opacity to its default (1.0).
    //
    // Both push an undo snapshot before mutating, mark dirty, and kick a
    // render via the debounce so users see the change immediately.
    void onLoadLut();
    void onClearLut();

    // ---- Edit-menu actions --------------------------------------------------
    // Reset wipes m_look to defaults but keeps the loaded image (unlike
    // Open Image, which also reloads the image and clears history).
    void onResetEdits();

    // Non-destructive image orientation.
    // Stored in m_look.transform so it round-trips through presets/projects
    // and participates in undo/redo.
    void onRotateLeft();
    void onRotateRight();
    void onFlipHorizontal();
    void onFlipVertical();
    void onResetTransform();

    // Clipboard Look transfer — JSON via QClipboard. Round-trips through
    // LookSerializer so the format matches preset files.
    void onCopyLook();
    void onPasteLook();

    void onPreferences();   // placeholder dialog

    // ---- View-menu actions --------------------------------------------------
    void onZoomFit();
    void onZoom100();
    void onZoomIn();
    void onZoomOut();
    void onToggleBeforeAfter();   // sticky toggle distinct from press-and-hold
    void onToggleFullscreen();
    void onSecondaryViewer();     // placeholder

    // ---- Window-menu actions ------------------------------------------------
    // Workspace = which docked panels are visible + their sizes. v1 only
    // tracks the controls-panel collapse state; placeholder otherwise.
    void onResetWorkspaceLayout();
    void onSaveWorkspaceLayout();

    // Show / focus the Node Graph dock. Lazy-creates the widget and
    // dock on first call; subsequent calls just bring it to front.
    void onShowNodeGraph();

    // ---- Plugins-menu actions -----------------------------------------------
    void onPluginManager();
    void onInstallPlugin();
    void onReloadPlugins();
    void onOpenPluginsFolder();

    // ---- Help-menu actions --------------------------------------------------
    void onShowKeyboardShortcuts();
    void onShowDocumentation();
    void onAbout();

    // ---- Sidebar collapse ---------------------------------------------------
    // Toggle between full-width controls panel and a narrow icon-only strip.
    // The full panel widget tree is preserved across toggles — only its
    // visibility changes — so slider values and per-section state survive.
    void onToggleSidebar();

    // ---- Preview context menu ----------------------------------------------
    // Built once and reused for every right-click on the preview. Hosts the
    // same QActions as the top menus, so commands stay in lock-step.
    void onPreviewContextMenu(const QPoint& posInPreview);

    // The "load this specific path" portion of onOpenImage, factored out
    // so drag-and-drop can call it directly with the dropped file path.
    // Performs the same state reset (Look default, undo cleared, dirty=
    // false, title refresh) and triggers a render. Returns true on a
    // successful load, false if the image couldn't be read (a warning is
    // shown to the user in that case). Caller is responsible for the
    // unsaved-changes prompt — this method blindly replaces the current
    // image, so the prompt must happen above it if the project is dirty.

    // Mark the project dirty and refresh the window title. Cheap; safe to
    // call multiple times in a row. Suppressed during project load via
    // m_isLoadingProject.
    void markDirty();

    // Refresh the window title from m_currentProjectPath + m_projectDirty.
    void updateWindowTitle();

    // Returns true if it's OK to discard the current project (saved, not
    // dirty, or user clicked Discard). Returns false if the user cancelled.
    // Called from any flow that would replace the current Look — close
    // event, Open Project, Open Image (deliberately we DON'T prompt on
    // Open Image; see implementation).
    bool maybePromptUnsavedChanges();
    bool maybePromptSaveDocument(int index);
    bool maybePromptAllUnsavedDocuments();

    // Multi-document foundation. Existing editor fields remain the active
    // working copy; these helpers snapshot/restore that working copy to the
    // tabbed document list.
    ImageDocument makeDocumentFromCurrentState() const;
    ImageDocument* activeDocument();
    const ImageDocument* activeDocument() const;
    QString documentTitle(const ImageDocument& document) const;
    void saveActiveDocumentState();
    int appendCurrentStateAsDocument();
    bool setActiveDocumentIndex(int index);
    bool closeDocumentAt(int index);
    void updateDocumentTabs();
    void clearEditorStateForNoDocuments();

    // Underlying file I/O. Return true on success.
    bool saveProjectToPath(const QString& path);
    bool loadProjectFromPath(const QString& path);
    bool recoverAutosaveFromPath(const QString& path);
    void checkAutosaveRecovery();
    void scheduleAutosave();
    lps::ProjectDocument currentProjectDocumentForAutosave() const;
};
