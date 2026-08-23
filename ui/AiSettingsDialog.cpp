// ==============================================================================
// ui/AiSettingsDialog.cpp
// ==============================================================================
#include "AiSettingsDialog.h"

#include "ai/AiClient.h"
#include "ai/AiSettings.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFont>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QUrl>
#include <QVBoxLayout>
#include <QVector>
#include <QWidget>

namespace {

QFrame* makeCard(QWidget* parent)
{
    auto* card = new QFrame(parent);
    card->setObjectName(QStringLiteral("aiCard"));
    auto* lay = new QVBoxLayout(card);
    lay->setContentsMargins(14, 12, 14, 12);
    lay->setSpacing(10);
    return card;
}

QLabel* makeSectionTitle(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    QFont font = label->font();
    font.setBold(true);
    font.setPointSize(font.pointSize() + 1);
    label->setFont(font);
    label->setStyleSheet(QStringLiteral("color: #E7E9EE;"));
    return label;
}

QLabel* makeHelpLabel(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setStyleSheet(QStringLiteral("color: #9AA1AC;"));
    return label;
}

} // namespace

AiSettingsDialog::AiSettingsDialog(QWidget* parent)
    : QDialog(parent)
    , m_client(new lps::AiClient(this))
{
    setWindowTitle(tr("AI Assistant Settings"));
    setModal(true);
    resize(620, 760);

    buildUi();
    loadFromSettings();
    updateEndpointPreview();

    connect(m_client, &lps::AiClient::responseReceived, this,
            [this](const QString& text) {
        // A reply of any shape proves auth + routing + model id are all valid.
        const QString reply = text.simplified();
        setStatus(reply.isEmpty()
                      ? tr("Connection succeeded.")
                      : tr("Connection succeeded. The model replied: %1")
                            .arg(reply.left(120)),
                  false);
    });

    connect(m_client, &lps::AiClient::errorOccurred, this,
            [this](const QString& message) {
        setStatus(message, true);
    });

    connect(m_client, &lps::AiClient::finished, this, [this]() {
        setTestRunning(false);
    });
}

AiSettingsDialog::~AiSettingsDialog() = default;

// ------------------------------------------------------------------------------
// Construction
// ------------------------------------------------------------------------------

void AiSettingsDialog::buildUi()
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* content = new QWidget(scroll);
    auto* root = new QVBoxLayout(content);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(12);

    // ---- Intro -------------------------------------------------------------
    {
        auto* intro = makeHelpLabel(
            tr("Lumen has no AI service of its own and ships no API key. Bring "
               "your own key from any provider, or point Lumen at a model "
               "running on this machine. Nothing is sent anywhere until you "
               "type a prompt and press Send."),
            content);
        root->addWidget(intro);
    }

    // ---- Provider ----------------------------------------------------------
    {
        auto* card = makeCard(content);
        auto* lay = static_cast<QVBoxLayout*>(card->layout());
        lay->addWidget(makeSectionTitle(tr("Provider"), card));

        auto* form = new QFormLayout();
        form->setContentsMargins(0, 0, 0, 0);
        form->setSpacing(8);

        m_presetCombo = new QComboBox(card);
        const QVector<lps::AiProvider> presets = lps::AiProvider::builtInPresets();
        for (const lps::AiProvider& preset : presets)
            m_presetCombo->addItem(preset.displayName, preset.id);
        form->addRow(tr("Preset"), m_presetCombo);
        connect(m_presetCombo, &QComboBox::currentIndexChanged,
                this, &AiSettingsDialog::onPresetChanged);

        m_wireFormatCombo = new QComboBox(card);
        m_wireFormatCombo->addItem(
            lps::AiProvider::wireFormatLabel(lps::AiWireFormat::OpenAiCompatible),
            static_cast<int>(lps::AiWireFormat::OpenAiCompatible));
        m_wireFormatCombo->addItem(
            lps::AiProvider::wireFormatLabel(lps::AiWireFormat::Anthropic),
            static_cast<int>(lps::AiWireFormat::Anthropic));
        form->addRow(tr("API format"), m_wireFormatCombo);
        connect(m_wireFormatCombo, &QComboBox::currentIndexChanged,
                this, &AiSettingsDialog::onWireFormatChanged);

        m_baseUrlEdit = new QLineEdit(card);
        m_baseUrlEdit->setPlaceholderText(
            QStringLiteral("https://api.openai.com/v1"));
        form->addRow(tr("Base URL"), m_baseUrlEdit);
        connect(m_baseUrlEdit, &QLineEdit::textChanged,
                this, [this](const QString&) { updateEndpointPreview(); });

        m_modelEdit = new QLineEdit(card);
        m_modelEdit->setPlaceholderText(tr("model id, e.g. gpt-4o-mini"));
        form->addRow(tr("Model"), m_modelEdit);

        lay->addLayout(form);

        m_endpointLabel = makeHelpLabel(QString(), card);
        lay->addWidget(m_endpointLabel);

        lay->addWidget(makeHelpLabel(
            tr("Choose the format your service speaks. OpenAI-compatible "
               "covers OpenAI, Groq, Together, Mistral, OpenRouter, DeepSeek, "
               "xAI, LM Studio and Ollama. Anthropic covers the Claude API."),
            card));

        root->addWidget(card);
    }

    // ---- API key -----------------------------------------------------------
    {
        auto* card = makeCard(content);
        auto* lay = static_cast<QVBoxLayout*>(card->layout());
        lay->addWidget(makeSectionTitle(tr("API Key"), card));

        auto* keyRow = new QWidget(card);
        auto* keyLay = new QHBoxLayout(keyRow);
        keyLay->setContentsMargins(0, 0, 0, 0);
        keyLay->setSpacing(8);

        m_apiKeyEdit = new QLineEdit(keyRow);
        m_apiKeyEdit->setEchoMode(QLineEdit::Password);
        m_apiKeyEdit->setPlaceholderText(tr("Paste your API key here"));
        // Never let a key end up in a completion popup or a clipboard history
        // driven by the input method.
        m_apiKeyEdit->setInputMethodHints(Qt::ImhHiddenText
                                          | Qt::ImhNoPredictiveText
                                          | Qt::ImhNoAutoUppercase
                                          | Qt::ImhSensitiveData);
        keyLay->addWidget(m_apiKeyEdit, 1);

        m_revealButton = new QPushButton(tr("Show"), keyRow);
        m_revealButton->setCheckable(true);
        m_revealButton->setToolTip(tr("Reveal the key so you can check it"));
        keyLay->addWidget(m_revealButton);
        connect(m_revealButton, &QPushButton::toggled,
                this, &AiSettingsDialog::onRevealToggled);

        m_clearKeyButton = new QPushButton(tr("Clear"), keyRow);
        m_clearKeyButton->setToolTip(tr("Forget the saved key"));
        keyLay->addWidget(m_clearKeyButton);
        connect(m_clearKeyButton, &QPushButton::clicked,
                this, &AiSettingsDialog::onClearKey);

        lay->addWidget(keyRow);

        lay->addWidget(makeHelpLabel(
            tr("A key is not required for a local endpoint such as Ollama or "
               "LM Studio."),
            card));

        // ---- The honest storage warning ------------------------------------
        // This text must never overstate what Lumen does. See the security
        // note at the top of src/ai/AiSettings.h.
        const lps::AiSettings settings;
        auto* warning = new QLabel(settings.storageWarningText(), card);
        warning->setWordWrap(true);
        warning->setTextInteractionFlags(Qt::TextSelectableByMouse);
        warning->setStyleSheet(QStringLiteral(
            "color: #F2C14E; border: 1px solid #6B5A22; border-radius: 6px; "
            "padding: 10px;"));
        lay->addWidget(warning);

        root->addWidget(card);
    }

    // ---- Request options ---------------------------------------------------
    {
        auto* card = makeCard(content);
        auto* lay = static_cast<QVBoxLayout*>(card->layout());
        lay->addWidget(makeSectionTitle(tr("Requests"), card));

        auto* form = new QFormLayout();
        form->setContentsMargins(0, 0, 0, 0);
        form->setSpacing(8);

        m_maxTokensSpin = new QSpinBox(card);
        m_maxTokensSpin->setRange(16, 32768);
        m_maxTokensSpin->setSingleStep(64);
        m_maxTokensSpin->setValue(1024);
        m_maxTokensSpin->setToolTip(
            tr("Upper bound on the reply length. Required by the Anthropic "
               "format and honoured by OpenAI-compatible endpoints."));
        form->addRow(tr("Max response tokens"), m_maxTokensSpin);

        m_timeoutSpin = new QSpinBox(card);
        m_timeoutSpin->setRange(1, 600);
        m_timeoutSpin->setSuffix(tr(" s"));
        m_timeoutSpin->setValue(60);
        form->addRow(tr("Timeout"), m_timeoutSpin);

        lay->addLayout(form);

        lay->addWidget(makeSectionTitle(tr("System prompt"), card));
        m_systemPromptEdit = new QPlainTextEdit(card);
        m_systemPromptEdit->setMinimumHeight(110);
        m_systemPromptEdit->setPlaceholderText(
            lps::AiSettings::defaultSystemPrompt());
        lay->addWidget(m_systemPromptEdit);
        lay->addWidget(makeHelpLabel(
            tr("Sent with every message to set the assistant's role. Leave "
               "empty to use the built-in photo-assistant prompt."),
            card));

        root->addWidget(card);
    }

    // ---- Test connection ---------------------------------------------------
    {
        auto* card = makeCard(content);
        auto* lay = static_cast<QVBoxLayout*>(card->layout());
        lay->addWidget(makeSectionTitle(tr("Test"), card));

        auto* row = new QWidget(card);
        auto* rowLay = new QHBoxLayout(row);
        rowLay->setContentsMargins(0, 0, 0, 0);
        rowLay->setSpacing(8);

        m_testButton = new QPushButton(tr("Test connection"), row);
        m_testButton->setToolTip(
            tr("Sends a two-word prompt to the endpoint above using the "
               "settings on this page."));
        rowLay->addWidget(m_testButton);
        rowLay->addStretch(1);
        connect(m_testButton, &QPushButton::clicked,
                this, &AiSettingsDialog::onTestConnection);

        lay->addWidget(row);

        m_statusLabel = new QLabel(
            tr("Not tested yet. A test sends one short prompt using the "
               "settings on this page; it does not save them."), card);
        m_statusLabel->setWordWrap(true);
        m_statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_statusLabel->setStyleSheet(QStringLiteral("color: #9AA1AC;"));
        lay->addWidget(m_statusLabel);

        root->addWidget(card);
    }

    root->addStretch(1);
    scroll->setWidget(content);
    outer->addWidget(scroll, 1);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    buttons->setContentsMargins(16, 8, 16, 12);
    connect(buttons, &QDialogButtonBox::accepted, this, &AiSettingsDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &AiSettingsDialog::reject);
    outer->addWidget(buttons);
}

// ------------------------------------------------------------------------------
// Load / save
// ------------------------------------------------------------------------------

void AiSettingsDialog::loadFromSettings()
{
    const lps::AiSettings settings;
    const lps::AiProvider provider = settings.provider();

    // Blocked: applyProviderToForm() drives the preset combo, and its change
    // handler would immediately overwrite the values we just loaded.
    const QSignalBlocker blockPreset(m_presetCombo);
    const QSignalBlocker blockWire(m_wireFormatCombo);

    applyProviderToForm(provider);

    const int presetIndex = m_presetCombo->findData(provider.id);
    m_presetCombo->setCurrentIndex(presetIndex >= 0 ? presetIndex : 0);

    m_apiKeyEdit->setText(settings.apiKey());

    const QString systemPrompt = settings.systemPrompt();
    if (systemPrompt != lps::AiSettings::defaultSystemPrompt())
        m_systemPromptEdit->setPlainText(systemPrompt);
}

void AiSettingsDialog::applyProviderToForm(const lps::AiProvider& provider)
{
    m_baseUrlEdit->setText(provider.baseUrl);
    m_modelEdit->setText(provider.modelId);
    m_maxTokensSpin->setValue(provider.maxTokens > 0 ? provider.maxTokens : 1024);
    m_timeoutSpin->setValue(provider.timeoutMs > 0 ? provider.timeoutMs / 1000 : 60);

    const int wireIndex =
        m_wireFormatCombo->findData(static_cast<int>(provider.wireFormat));
    m_wireFormatCombo->setCurrentIndex(wireIndex >= 0 ? wireIndex : 0);

    updateEndpointPreview();
}

lps::AiProvider AiSettingsDialog::providerFromForm() const
{
    lps::AiProvider provider;
    provider.id = m_presetCombo->currentData().toString();
    if (provider.id.isEmpty())
        provider.id = QStringLiteral("custom");
    provider.displayName = m_presetCombo->currentText();
    provider.baseUrl     = m_baseUrlEdit->text().trimmed();
    provider.modelId     = m_modelEdit->text().trimmed();
    provider.wireFormat  = static_cast<lps::AiWireFormat>(
        m_wireFormatCombo->currentData().toInt());
    provider.maxTokens   = m_maxTokensSpin->value();
    provider.timeoutMs   = m_timeoutSpin->value() * 1000;

    // Extra headers are preserved from the saved config: they are an advanced,
    // rarely-used field with no editor in this dialog, and silently dropping
    // them on every save would be a nasty surprise.
    const lps::AiSettings settings;
    const lps::AiProvider saved = settings.provider();
    if (saved.id == provider.id)
        provider.extraHeaders = saved.extraHeaders;

    return provider;
}

void AiSettingsDialog::accept()
{
    const lps::AiProvider provider = providerFromForm();

    if (!provider.resolvedBaseUrl().isEmpty()) {
        const QUrl url(provider.resolvedBaseUrl());
        const QString scheme = url.scheme().toLower();
        if (!url.isValid() || url.host().isEmpty()
            || (scheme != QStringLiteral("http")
                && scheme != QStringLiteral("https"))) {
            QMessageBox::warning(
                this,
                tr("AI Assistant Settings"),
                tr("The base URL must be a full http:// or https:// address, "
                   "for example https://api.openai.com/v1."));
            return;
        }
    }

    if (m_testRunning)
        m_client->cancel();

    lps::AiSettings settings;
    settings.setProvider(provider);
    settings.setSystemPrompt(m_systemPromptEdit->toPlainText());

    const QString key = m_apiKeyEdit->text().trimmed();
    if (key.isEmpty())
        settings.clearApiKey();
    else
        settings.setApiKey(key);

    QDialog::accept();
}

void AiSettingsDialog::reject()
{
    if (m_testRunning)
        m_client->cancel();
    QDialog::reject();
}

// ------------------------------------------------------------------------------
// Interaction
// ------------------------------------------------------------------------------

void AiSettingsDialog::onPresetChanged(int index)
{
    if (index < 0) return;

    const QString id = m_presetCombo->itemData(index).toString();

    // "Custom" is a marker, not a template: switching to it must not wipe out
    // whatever the user has already typed.
    if (id == QStringLiteral("custom")) {
        updateEndpointPreview();
        return;
    }

    const lps::AiProvider preset = lps::AiProvider::presetById(id);
    const QSignalBlocker blockWire(m_wireFormatCombo);
    applyProviderToForm(preset);

    setStatus(tr("Preset loaded. Your API key was left unchanged."), false);
}

void AiSettingsDialog::onWireFormatChanged(int index)
{
    Q_UNUSED(index)
    updateEndpointPreview();
}

void AiSettingsDialog::onRevealToggled(bool revealed)
{
    m_apiKeyEdit->setEchoMode(revealed ? QLineEdit::Normal : QLineEdit::Password);
    m_revealButton->setText(revealed ? tr("Hide") : tr("Show"));
}

void AiSettingsDialog::onClearKey()
{
    m_apiKeyEdit->clear();
    setStatus(tr("The saved key will be removed when you press Save."), false);
}

void AiSettingsDialog::onTestConnection()
{
    if (m_testRunning) {
        m_client->cancel();
        return;
    }

    const lps::AiProvider provider = providerFromForm();
    if (!provider.isValid()) {
        setStatus(tr("Enter a base URL and a model id before testing."), true);
        return;
    }

    // Test what is on screen, not what is saved, so the user can validate a
    // key before committing it.
    m_client->setProvider(provider);
    m_client->setApiKey(m_apiKeyEdit->text().trimmed());

    setTestRunning(true);
    setStatus(tr("Contacting %1...").arg(provider.endpointUrl().toString()), false);
    m_client->testConnection();
}

void AiSettingsDialog::setTestRunning(bool running)
{
    m_testRunning = running;
    m_testButton->setText(running ? tr("Cancel test") : tr("Test connection"));
}

void AiSettingsDialog::setStatus(const QString& message, bool isError)
{
    m_statusLabel->setText(message);
    m_statusLabel->setStyleSheet(isError
        ? QStringLiteral("color: #E4685D;")
        : QStringLiteral("color: #9AA1AC;"));
}

void AiSettingsDialog::updateEndpointPreview()
{
    if (!m_endpointLabel) return;

    const lps::AiWireFormat format = static_cast<lps::AiWireFormat>(
        m_wireFormatCombo->currentData().toInt());

    lps::AiProvider probe;
    probe.baseUrl    = m_baseUrlEdit->text();
    probe.wireFormat = format;

    const QUrl endpoint = probe.endpointUrl();
    m_endpointLabel->setText(endpoint.isEmpty()
        ? tr("Requests go to: (enter a base URL)")
        : tr("Requests go to: %1").arg(endpoint.toString()));
}
