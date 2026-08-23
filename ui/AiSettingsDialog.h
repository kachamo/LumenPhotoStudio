// ==============================================================================
// ui/AiSettingsDialog.h
//
// "Paste your API key here" — the single place a user points Lumen at an AI
// service. Provider-agnostic by construction: a preset only prefills the base
// URL, wire format and model id, and every field stays editable so any
// OpenAI-compatible or Anthropic-shaped endpoint works, including a local one.
//
// The dialog is self-contained: construct it with any parent (or none), exec()
// it, and it persists through lps::AiSettings on accept. It owns its own
// lps::AiClient purely for the "Test connection" round trip.
// ==============================================================================
#pragma once

#include "ai/AiProvider.h"

#include <QDialog>
#include <QString>

class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;

namespace lps { class AiClient; }

class AiSettingsDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit AiSettingsDialog(QWidget* parent = nullptr);
    ~AiSettingsDialog() override;

protected:
    void accept() override;
    void reject() override;

private:
    void buildUi();
    void loadFromSettings();

    // Reads the form into a provider. Does not touch persisted state, so it is
    // safe to call for a test connection before the user commits.
    lps::AiProvider providerFromForm() const;

    void applyProviderToForm(const lps::AiProvider& provider);
    void onPresetChanged(int index);
    void onWireFormatChanged(int index);
    void onRevealToggled(bool revealed);
    void onTestConnection();
    void onClearKey();
    void setTestRunning(bool running);
    void setStatus(const QString& message, bool isError);
    void updateEndpointPreview();

    lps::AiClient* m_client = nullptr;
    bool m_testRunning = false;

    QComboBox*      m_presetCombo    = nullptr;
    QComboBox*      m_wireFormatCombo = nullptr;
    QLineEdit*      m_baseUrlEdit    = nullptr;
    QLineEdit*      m_modelEdit      = nullptr;
    QLineEdit*      m_apiKeyEdit     = nullptr;
    QPushButton*    m_revealButton   = nullptr;
    QPushButton*    m_clearKeyButton = nullptr;
    QSpinBox*       m_maxTokensSpin  = nullptr;
    QSpinBox*       m_timeoutSpin    = nullptr;
    QPlainTextEdit* m_systemPromptEdit = nullptr;
    QLabel*         m_endpointLabel  = nullptr;
    QLabel*         m_statusLabel    = nullptr;
    QPushButton*    m_testButton     = nullptr;
};
