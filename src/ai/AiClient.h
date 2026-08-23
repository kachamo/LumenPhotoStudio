// ==============================================================================
// src/ai/AiClient.h
//
// Asynchronous, signal-based HTTP client for a user-supplied AI endpoint.
// This is the only place in Lumen that performs network I/O.
//
// Design notes:
//   * One in-flight request at a time. A second sendPrompt() while busy is
//     rejected via errorOccurred() rather than silently queued, so the UI can
//     never end up with two racing replies feeding one response box.
//   * Never blocks the UI thread: QNetworkAccessManager is event-driven and
//     every result arrives on a signal.
//   * Hard timeout enforced by a QTimer, because a stalled TCP connection can
//     otherwise leave a reply pending indefinitely.
//   * Redirects are NOT followed. A 3xx would let a mistyped or hijacked base
//     URL forward the Authorization / x-api-key header to a third-party host.
//   * The API key is held in memory only, never logged, and never placed in an
//     error string or a URL.
//   * No image data is ever transmitted. Only the prompt text the user typed
//     and the configured system prompt are sent.
// ==============================================================================
#pragma once

#include "ai/AiProvider.h"

#include <QByteArray>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace lps {

class AiClient final : public QObject
{
    Q_OBJECT

public:
    explicit AiClient(QObject* parent = nullptr);
    ~AiClient() override;

    void setProvider(const AiProvider& provider);
    AiProvider provider() const { return m_provider; }

    // Held in memory for the lifetime of the client. Never persisted here,
    // never logged. Pass an empty string for keyless local endpoints.
    void setApiKey(const QString& key);
    bool hasApiKey() const { return !m_apiKey.isEmpty(); }

    bool isBusy() const { return m_reply != nullptr; }

    // Single-turn chat. systemPrompt may be empty.
    void sendPrompt(const QString& userPrompt, const QString& systemPrompt = QString());

    // Minimal round trip used by the settings dialog's "Test connection".
    // Same endpoint and auth as a real request, but a trivial prompt and a
    // small token budget so it costs as close to nothing as possible.
    void testConnection();

    // Safe to call when idle. Aborts the in-flight reply; errorOccurred() and
    // finished() still fire so the UI can always re-enable its controls.
    void cancel();

signals:
    // Assistant text, already extracted from whichever envelope came back.
    void responseReceived(const QString& text);

    // Human-readable, actionable, and guaranteed never to contain the API key.
    void errorOccurred(const QString& message);

    // Always emitted exactly once per request, after responseReceived() or
    // errorOccurred(), success or failure.
    void finished();

private:
    void startRequest(const QString& userPrompt,
                      const QString& systemPrompt,
                      bool probe);
    void handleReplyFinished();
    void handleTimeout();
    void teardownReply();
    void failWith(const QString& message);

    // Returns false and fills errorOut when the provider is unusable.
    bool buildRequest(QNetworkRequest* requestOut, QString* errorOut) const;

    QJsonObject buildRequestBody(const QString& userPrompt,
                                 const QString& systemPrompt,
                                 bool probe) const;

    // Success envelopes. Returns the assistant text, or an empty string and a
    // filled errorOut when the payload is shaped unexpectedly.
    static QString extractText(const QJsonObject& root,
                               AiWireFormat format,
                               QString* errorOut);

    // Error envelopes. The two formats disagree, so both shapes are probed.
    static QString extractErrorMessage(const QByteArray& payload,
                                       AiWireFormat format);

    static QString httpStatusExplanation(int status, AiWireFormat format);

    QNetworkAccessManager* m_network = nullptr;
    QNetworkReply* m_reply = nullptr;
    QTimer* m_timeout = nullptr;

    AiProvider m_provider;
    QString m_apiKey;
    bool m_cancelRequested = false;
};

} // namespace lps
