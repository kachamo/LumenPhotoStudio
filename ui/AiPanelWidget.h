// ==============================================================================
// ui/AiPanelWidget.h
//
// A modest, self-contained photo-assistant panel: prompt box, Send, Cancel and
// a response area. Drop it into a QDockWidget, a tab or a standalone window —
// it depends on nothing but lps::AiClient and lps::AiSettings, and it takes no
// image data.
//
// Deliberately NOT wired into the render pipeline. Letting a model drive
// exposure or curves is a much larger design question (determinism, undo
// semantics, how a suggestion becomes a Look) and is out of scope here. This
// panel answers questions; the user makes the edits.
//
// Nothing leaves the machine until the user presses Send. No image pixels, no
// metadata and no telemetry are ever transmitted — only the prompt text typed
// into this panel plus the configured system prompt.
// ==============================================================================
#pragma once

#include <QString>
#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTextBrowser;

namespace lps { class AiClient; }

class AiPanelWidget final : public QWidget
{
    Q_OBJECT

public:
    explicit AiPanelWidget(QWidget* parent = nullptr);
    ~AiPanelWidget() override;

public slots:
    // Re-reads the saved provider and key. Call after anything that could have
    // changed them; the panel also does this itself whenever it is shown.
    void reloadSettings();

    // Opens the AI settings dialog and reloads on accept.
    void openSettings();

protected:
    void showEvent(QShowEvent* event) override;

private:
    void buildUi();
    void onSend();
    void onCancel();
    void appendUserMessage(const QString& text);
    void appendAssistantMessage(const QString& text);
    void appendErrorMessage(const QString& text);
    void setBusy(bool busy);
    void updateReadyState();

    lps::AiClient* m_client = nullptr;
    bool m_busy = false;

    QLabel*         m_providerLabel = nullptr;
    QLabel*         m_statusLabel   = nullptr;
    QTextBrowser*   m_transcript    = nullptr;
    QPlainTextEdit* m_promptEdit    = nullptr;
    QPushButton*    m_sendButton    = nullptr;
    QPushButton*    m_cancelButton  = nullptr;
    QPushButton*    m_settingsButton = nullptr;
    QPushButton*    m_clearButton   = nullptr;
};
