// ==============================================================================
// ui/AiPanelWidget.cpp
// ==============================================================================
#include "AiPanelWidget.h"

#include "AiSettingsDialog.h"

#include "ai/AiClient.h"
#include "ai/AiSettings.h"

#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QShortcut>
#include <QShowEvent>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace {

// The transcript is built as HTML, so every piece of text that comes from the
// user or from the model MUST be escaped. A model is perfectly capable of
// emitting <img src="http://..."> , and an unescaped transcript would fetch it.
QString escaped(const QString& text)
{
    return text.toHtmlEscaped().replace(QStringLiteral("\n"),
                                        QStringLiteral("<br>"));
}

} // namespace

AiPanelWidget::AiPanelWidget(QWidget* parent)
    : QWidget(parent)
    , m_client(new lps::AiClient(this))
{
    setObjectName(QStringLiteral("aiPanel"));

    buildUi();
    reloadSettings();

    connect(m_client, &lps::AiClient::responseReceived,
            this, &AiPanelWidget::appendAssistantMessage);
    connect(m_client, &lps::AiClient::errorOccurred,
            this, &AiPanelWidget::appendErrorMessage);
    connect(m_client, &lps::AiClient::finished, this, [this]() {
        setBusy(false);
    });
}

AiPanelWidget::~AiPanelWidget() = default;

// ------------------------------------------------------------------------------
// Construction
// ------------------------------------------------------------------------------

void AiPanelWidget::buildUi()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(8);

    // ---- Header ------------------------------------------------------------
    {
        auto* header = new QWidget(this);
        auto* headerLay = new QHBoxLayout(header);
        headerLay->setContentsMargins(0, 0, 0, 0);
        headerLay->setSpacing(8);

        auto* title = new QLabel(tr("Photo Assistant"), header);
        QFont titleFont = title->font();
        titleFont.setBold(true);
        titleFont.setPointSize(titleFont.pointSize() + 1);
        title->setFont(titleFont);
        headerLay->addWidget(title);

        headerLay->addStretch(1);

        m_settingsButton = new QPushButton(tr("Settings..."), header);
        m_settingsButton->setToolTip(tr("Choose a provider and paste your API key"));
        headerLay->addWidget(m_settingsButton);
        connect(m_settingsButton, &QPushButton::clicked,
                this, &AiPanelWidget::openSettings);

        root->addWidget(header);
    }

    m_providerLabel = new QLabel(this);
    m_providerLabel->setWordWrap(true);
    m_providerLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_providerLabel->setStyleSheet(QStringLiteral("color: #9AA1AC;"));
    root->addWidget(m_providerLabel);

    // ---- Transcript --------------------------------------------------------
    m_transcript = new QTextBrowser(this);
    m_transcript->setObjectName(QStringLiteral("aiTranscript"));
    m_transcript->setFrameShape(QFrame::StyledPanel);
    m_transcript->setMinimumHeight(160);
    // No link following and no remote content: the transcript renders text the
    // model produced, and it is not allowed to reach back out to the network.
    m_transcript->setOpenLinks(false);
    m_transcript->setOpenExternalLinks(false);
    root->addWidget(m_transcript, 1);

    // ---- Prompt ------------------------------------------------------------
    m_promptEdit = new QPlainTextEdit(this);
    m_promptEdit->setPlaceholderText(
        tr("Ask about an edit, e.g. \"how do I recover these blown "
           "highlights?\"  (Ctrl+Enter to send)"));
    m_promptEdit->setMinimumHeight(72);
    m_promptEdit->setMaximumHeight(140);
    root->addWidget(m_promptEdit);

    auto* sendShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return),
                                       m_promptEdit);
    sendShortcut->setContext(Qt::WidgetShortcut);
    connect(sendShortcut, &QShortcut::activated, this, &AiPanelWidget::onSend);

    // ---- Actions -----------------------------------------------------------
    {
        auto* actions = new QWidget(this);
        auto* actionsLay = new QHBoxLayout(actions);
        actionsLay->setContentsMargins(0, 0, 0, 0);
        actionsLay->setSpacing(8);

        m_clearButton = new QPushButton(tr("Clear"), actions);
        m_clearButton->setToolTip(tr("Clear the transcript"));
        actionsLay->addWidget(m_clearButton);
        connect(m_clearButton, &QPushButton::clicked, this, [this]() {
            m_transcript->clear();
        });

        actionsLay->addStretch(1);

        m_cancelButton = new QPushButton(tr("Cancel"), actions);
        m_cancelButton->setEnabled(false);
        actionsLay->addWidget(m_cancelButton);
        connect(m_cancelButton, &QPushButton::clicked,
                this, &AiPanelWidget::onCancel);

        m_sendButton = new QPushButton(tr("Send"), actions);
        m_sendButton->setDefault(true);
        actionsLay->addWidget(m_sendButton);
        connect(m_sendButton, &QPushButton::clicked, this, &AiPanelWidget::onSend);

        root->addWidget(actions);
    }

    m_statusLabel = new QLabel(this);
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setStyleSheet(QStringLiteral("color: #9AA1AC;"));
    root->addWidget(m_statusLabel);
}

// ------------------------------------------------------------------------------
// Settings
// ------------------------------------------------------------------------------

void AiPanelWidget::reloadSettings()
{
    const lps::AiSettings settings;
    const lps::AiProvider provider = settings.provider();

    m_client->setProvider(provider);
    m_client->setApiKey(settings.apiKey());

    updateReadyState();
}

void AiPanelWidget::openSettings()
{
    AiSettingsDialog dialog(this);
    if (dialog.exec() == QDialog::Accepted)
        reloadSettings();
}

void AiPanelWidget::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);

    // Picks up a key or provider changed elsewhere (including by another
    // window) without the host having to remember to tell us.
    if (!m_busy)
        reloadSettings();
}

void AiPanelWidget::updateReadyState()
{
    const lps::AiSettings settings;
    const lps::AiProvider provider = settings.provider();
    const bool ready = settings.isConfigured();

    if (ready) {
        const QString name = provider.displayName.isEmpty()
            ? provider.resolvedBaseUrl()
            : provider.displayName;
        m_providerLabel->setText(
            tr("Using %1 - model %2. Only the text you type is sent; your "
               "photo never leaves this machine.")
                .arg(name, provider.modelId));
        m_statusLabel->clear();
    } else {
        m_providerLabel->setText(settings.configurationHint()
                                 + QLatin1Char('\n')
                                 + tr("Press Settings... to choose a provider "
                                      "and paste your own API key."));
        m_statusLabel->clear();
    }

    m_promptEdit->setEnabled(ready && !m_busy);
    m_sendButton->setEnabled(ready && !m_busy);
    m_cancelButton->setEnabled(m_busy);
    m_settingsButton->setEnabled(!m_busy);
}

// ------------------------------------------------------------------------------
// Conversation
// ------------------------------------------------------------------------------

void AiPanelWidget::onSend()
{
    if (m_busy) return;

    const QString prompt = m_promptEdit->toPlainText().trimmed();
    if (prompt.isEmpty()) {
        m_statusLabel->setText(tr("Type a question first."));
        return;
    }

    const lps::AiSettings settings;
    if (!settings.isConfigured()) {
        m_statusLabel->setText(settings.configurationHint());
        return;
    }

    // Refresh right before sending: the settings dialog may have been used
    // from somewhere else since this panel was last shown.
    reloadSettings();

    appendUserMessage(prompt);
    m_promptEdit->clear();

    setBusy(true);
    m_statusLabel->setText(tr("Waiting for a reply..."));

    // Single-turn: no prior messages are replayed. That keeps token cost
    // predictable and means nothing from an earlier session is resent.
    m_client->sendPrompt(prompt, settings.systemPrompt());
}

void AiPanelWidget::onCancel()
{
    if (!m_busy) return;
    m_client->cancel();
}

void AiPanelWidget::appendUserMessage(const QString& text)
{
    m_transcript->append(
        QStringLiteral("<p style=\"color:#E7E9EE;\"><b>%1</b><br>%2</p>")
            .arg(escaped(tr("You")), escaped(text)));
}

void AiPanelWidget::appendAssistantMessage(const QString& text)
{
    m_transcript->append(
        QStringLiteral("<p style=\"color:#C8D0DA;\"><b>%1</b><br>%2</p>")
            .arg(escaped(tr("Assistant")), escaped(text)));
    m_statusLabel->clear();
}

void AiPanelWidget::appendErrorMessage(const QString& text)
{
    m_transcript->append(
        QStringLiteral("<p style=\"color:#E4685D;\"><b>%1</b><br>%2</p>")
            .arg(escaped(tr("Error")), escaped(text)));
    m_statusLabel->clear();
}

void AiPanelWidget::setBusy(bool busy)
{
    m_busy = busy;
    m_sendButton->setText(busy ? tr("Sending...") : tr("Send"));
    updateReadyState();
}
