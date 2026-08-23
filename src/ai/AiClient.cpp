// ==============================================================================
// src/ai/AiClient.cpp
// ==============================================================================
#include "ai/AiClient.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <QLatin1String>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSslSocket>
#include <QStringList>
#include <QTimer>
#include <QUrl>

namespace {

// Anthropic pins its request/response shape to a dated version header. This is
// the current stable value and is unrelated to the model id.
constexpr auto kAnthropicVersion = "2023-06-01";

// "Test connection" budget: enough to prove auth, routing and model id are all
// correct, small enough that the user is not billed anything meaningful.
constexpr int kProbeMaxTokens = 16;

QString userAgent()
{
    const QString version = QCoreApplication::applicationVersion();
    return version.isEmpty()
        ? QStringLiteral("LumenPhotoStudio")
        : QStringLiteral("LumenPhotoStudio/") + version;
}

// Providers occasionally return a very long HTML error page (a proxy, a
// captive portal, a 502 from a CDN). Keep the UI readable.
QString elide(const QString& text, int maxChars = 400)
{
    const QString clean = text.simplified();
    if (clean.size() <= maxChars) return clean;
    return clean.left(maxChars) + QStringLiteral("...");
}

} // namespace

namespace lps {

AiClient::AiClient(QObject* parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
    , m_timeout(new QTimer(this))
{
    m_timeout->setSingleShot(true);
    connect(m_timeout, &QTimer::timeout, this, &AiClient::handleTimeout);
}

AiClient::~AiClient()
{
    // Abort rather than leak a pending reply. Signals are already disconnected
    // by teardownReply(), so nothing is emitted from a half-destroyed object.
    teardownReply();
}

void AiClient::setProvider(const AiProvider& provider)
{
    m_provider = provider;
}

void AiClient::setApiKey(const QString& key)
{
    m_apiKey = key.trimmed();
}

void AiClient::sendPrompt(const QString& userPrompt, const QString& systemPrompt)
{
    startRequest(userPrompt, systemPrompt, false);
}

void AiClient::testConnection()
{
    startRequest(
        tr("Reply with the single word: ok"),
        tr("You are a connection test. Reply with the single word: ok"),
        true);
}

void AiClient::cancel()
{
    if (!m_reply) return;

    m_cancelRequested = true;
    teardownReply();
    emit errorOccurred(tr("Request cancelled."));
    emit finished();
    m_cancelRequested = false;
}

// ------------------------------------------------------------------------------
// Request construction
// ------------------------------------------------------------------------------

void AiClient::startRequest(const QString& userPrompt,
                            const QString& systemPrompt,
                            bool probe)
{
    // A rejected call is not a request, so no finished() is emitted for it.
    if (m_reply) {
        emit errorOccurred(tr("A request is already running. Cancel it before "
                              "sending another."));
        return;
    }

    if (userPrompt.trimmed().isEmpty()) {
        failWith(tr("Nothing to send: the prompt is empty."));
        return;
    }

    QNetworkRequest request;
    QString error;
    if (!buildRequest(&request, &error)) {
        failWith(error);
        return;
    }

    const QJsonObject body = buildRequestBody(userPrompt, systemPrompt, probe);
    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);

    m_reply = m_network->post(request, payload);
    if (!m_reply) {
        failWith(tr("Could not start the request. The network stack refused to "
                    "create it."));
        return;
    }

    connect(m_reply, &QNetworkReply::finished,
            this, &AiClient::handleReplyFinished);

    m_timeout->start(m_provider.timeoutMs > 0 ? m_provider.timeoutMs : 60000);
}

bool AiClient::buildRequest(QNetworkRequest* requestOut, QString* errorOut) const
{
    const auto fail = [errorOut](const QString& message) {
        if (errorOut) *errorOut = message;
        return false;
    };

    if (m_provider.resolvedBaseUrl().isEmpty())
        return fail(tr("No AI service is configured. Open the AI settings and "
                       "enter a base URL."));

    if (m_provider.modelId.trimmed().isEmpty())
        return fail(tr("No model id is configured. Open the AI settings and "
                       "enter the model your provider exposes."));

    const QUrl url = m_provider.endpointUrl();
    if (!url.isValid() || url.host().isEmpty())
        return fail(tr("The base URL is not a valid address: %1")
                        .arg(m_provider.resolvedBaseUrl()));

    const QString scheme = url.scheme().toLower();
    if (scheme != QStringLiteral("http") && scheme != QStringLiteral("https"))
        return fail(tr("The base URL must start with http:// or https://."));

    // Refuse to put a credential on the wire in the clear. Loopback is exempt:
    // local runtimes speak plain http and the traffic never leaves the machine.
    if (scheme == QStringLiteral("http")
        && !m_provider.isLocalEndpoint()
        && !m_apiKey.isEmpty()) {
        return fail(tr("Refusing to send your API key over an unencrypted "
                       "http:// connection to %1. Use https:// instead.")
                        .arg(url.host()));
    }

    if (m_apiKey.isEmpty() && !m_provider.isLocalEndpoint())
        return fail(tr("No API key has been saved for this provider. Open the "
                       "AI settings and paste your own key."));

    if (scheme == QStringLiteral("https") && !QSslSocket::supportsSsl()) {
        return fail(tr("This build cannot open an HTTPS connection: no TLS "
                       "backend is available (Qt reports OpenSSL is missing). "
                       "Install the OpenSSL runtime, or point Lumen at a local "
                       "http://localhost endpoint."));
    }

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    request.setHeader(QNetworkRequest::UserAgentHeader, userAgent());
    request.setRawHeader(QByteArrayLiteral("Accept"),
                         QByteArrayLiteral("application/json"));

    // Do not follow redirects: a 3xx from a mistyped or hostile base URL would
    // otherwise re-send the auth header to whatever host it names.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);

    // User-supplied headers first, so the auth headers below always win and a
    // stray entry cannot blank out the credential.
    for (auto it = m_provider.extraHeaders.constBegin();
         it != m_provider.extraHeaders.constEnd(); ++it) {
        const QString name = it.key().trimmed();
        if (name.isEmpty()) continue;
        request.setRawHeader(name.toUtf8(), it.value().toUtf8());
    }

    if (!m_apiKey.isEmpty()) {
        if (m_provider.wireFormat == AiWireFormat::Anthropic) {
            request.setRawHeader(QByteArrayLiteral("x-api-key"),
                                 m_apiKey.toUtf8());
            request.setRawHeader(QByteArrayLiteral("anthropic-version"),
                                 QByteArray(kAnthropicVersion));
        } else {
            request.setRawHeader(QByteArrayLiteral("Authorization"),
                                 QByteArrayLiteral("Bearer ") + m_apiKey.toUtf8());
        }
    } else if (m_provider.wireFormat == AiWireFormat::Anthropic) {
        // Keyless Anthropic-shaped endpoint (a local proxy). The version header
        // is part of the wire format, not the credential, so it still applies.
        request.setRawHeader(QByteArrayLiteral("anthropic-version"),
                             QByteArray(kAnthropicVersion));
    }

    *requestOut = request;
    return true;
}

QJsonObject AiClient::buildRequestBody(const QString& userPrompt,
                                       const QString& systemPrompt,
                                       bool probe) const
{
    const int maxTokens = probe
        ? kProbeMaxTokens
        : (m_provider.maxTokens > 0 ? m_provider.maxTokens : 1024);

    QJsonObject body;
    body.insert(QStringLiteral("model"), m_provider.modelId.trimmed());
    body.insert(QStringLiteral("max_tokens"), maxTokens);

    QJsonArray messages;

    if (m_provider.wireFormat == AiWireFormat::Anthropic) {
        // Anthropic takes the system prompt as a TOP-LEVEL string. Sending it
        // as a {"role":"system"} message is rejected with a 400.
        if (!systemPrompt.trimmed().isEmpty())
            body.insert(QStringLiteral("system"), systemPrompt.trimmed());
    } else if (!systemPrompt.trimmed().isEmpty()) {
        QJsonObject system;
        system.insert(QStringLiteral("role"),    QStringLiteral("system"));
        system.insert(QStringLiteral("content"), systemPrompt.trimmed());
        messages.append(system);
    }

    QJsonObject user;
    user.insert(QStringLiteral("role"),    QStringLiteral("user"));
    user.insert(QStringLiteral("content"), userPrompt.trimmed());
    messages.append(user);

    body.insert(QStringLiteral("messages"), messages);

    // temperature and top_p are deliberately omitted: several current models
    // reject any non-default sampling parameters, and the defaults are fine
    // for a text assistant.
    return body;
}

// ------------------------------------------------------------------------------
// Reply handling
// ------------------------------------------------------------------------------

void AiClient::handleReplyFinished()
{
    if (!m_reply) return;

    const int status = m_reply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError networkError = m_reply->error();
    const QString networkErrorText = m_reply->errorString();
    const QByteArray payload = m_reply->readAll();

    teardownReply();

    if (m_cancelRequested)
        return; // cancel() already reported and finished.

    // ---- Transport-level failures (no usable HTTP status) -----------------
    if (status == 0) {
        QString message;
        switch (networkError) {
        case QNetworkReply::ConnectionRefusedError:
            message = tr("Connection refused. If this is a local model server, "
                         "check that it is running and listening on the "
                         "configured port.");
            break;
        case QNetworkReply::HostNotFoundError:
            message = tr("Host not found. Check the base URL for a typo.");
            break;
        case QNetworkReply::TimeoutError:
        case QNetworkReply::OperationCanceledError:
            message = tr("The connection timed out before the service "
                         "responded.");
            break;
        case QNetworkReply::SslHandshakeFailedError:
            message = tr("TLS handshake failed. The certificate was rejected, "
                         "or this build has no working OpenSSL runtime.");
            break;
        case QNetworkReply::ProxyConnectionRefusedError:
        case QNetworkReply::ProxyNotFoundError:
            message = tr("The system proxy refused the connection.");
            break;
        default:
            message = tr("Network error: %1").arg(elide(networkErrorText));
            break;
        }
        failWith(message);
        return;
    }

    // ---- HTTP-level failures ----------------------------------------------
    if (status < 200 || status >= 300) {
        const QString fromEnvelope =
            extractErrorMessage(payload, m_provider.wireFormat);
        const QString explanation =
            httpStatusExplanation(status, m_provider.wireFormat);

        QString message = tr("HTTP %1 - %2").arg(status).arg(explanation);
        if (!fromEnvelope.isEmpty())
            message += QStringLiteral("\n\n") + tr("Service said: %1")
                                                    .arg(elide(fromEnvelope));
        failWith(message);
        return;
    }

    // ---- Success ----------------------------------------------------------
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        failWith(tr("The service returned a %1 but the body was not valid JSON. "
                    "Check that the base URL points at an API endpoint and not "
                    "at a web page.").arg(status));
        return;
    }

    QString extractError;
    const QString text =
        extractText(doc.object(), m_provider.wireFormat, &extractError);

    if (!extractError.isEmpty()) {
        failWith(extractError);
        return;
    }

    emit responseReceived(text);
    emit finished();
}

void AiClient::handleTimeout()
{
    if (!m_reply) return;

    const int seconds = (m_provider.timeoutMs > 0 ? m_provider.timeoutMs : 60000)
                        / 1000;
    teardownReply();
    emit errorOccurred(tr("No response after %n second(s). The request was "
                          "aborted.", nullptr, seconds));
    emit finished();
}

void AiClient::teardownReply()
{
    if (m_timeout) m_timeout->stop();

    if (!m_reply) return;

    QNetworkReply* reply = m_reply;
    m_reply = nullptr;

    // Disconnect first: abort() synchronously emits finished(), and we have
    // already taken everything we need off the reply.
    reply->disconnect(this);
    if (reply->isRunning())
        reply->abort();
    reply->deleteLater();
}

void AiClient::failWith(const QString& message)
{
    emit errorOccurred(message);
    emit finished();
}

// ------------------------------------------------------------------------------
// Envelope parsing
// ------------------------------------------------------------------------------

QString AiClient::extractText(const QJsonObject& root,
                              AiWireFormat format,
                              QString* errorOut)
{
    const auto fail = [errorOut](const QString& message) {
        if (errorOut) *errorOut = message;
        return QString();
    };

    if (format == AiWireFormat::Anthropic) {
        // { "content": [ { "type": "text", "text": "..." }, ... ],
        //   "stop_reason": "end_turn" }
        const QJsonArray blocks = root.value(QStringLiteral("content")).toArray();
        QStringList parts;
        for (const QJsonValue& block : blocks) {
            const QJsonObject obj = block.toObject();
            if (obj.value(QStringLiteral("type")).toString()
                != QStringLiteral("text")) {
                continue;
            }
            const QString text = obj.value(QStringLiteral("text")).toString();
            if (!text.isEmpty()) parts.append(text);
        }

        if (parts.isEmpty()) {
            const QString stopReason =
                root.value(QStringLiteral("stop_reason")).toString();
            if (stopReason == QStringLiteral("max_tokens"))
                return fail(tr("The reply was cut off before any text was "
                               "produced. Raise the maximum response length in "
                               "the AI settings."));
            return fail(tr("The service replied successfully but the response "
                           "contained no text."));
        }
        return parts.join(QString());
    }

    // OpenAI-compatible:
    // { "choices": [ { "message": { "content": "..." },
    //                  "finish_reason": "stop" } ] }
    const QJsonArray choices = root.value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty()) {
        return fail(tr("The service replied successfully but returned no "
                       "choices. If this endpoint is not OpenAI-compatible, "
                       "switch the wire format in the AI settings."));
    }

    const QJsonObject choice = choices.first().toObject();
    QString text = choice.value(QStringLiteral("message"))
                       .toObject()
                       .value(QStringLiteral("content"))
                       .toString();

    // Some gateways answer a non-streaming request with a delta object, and a
    // few legacy completion endpoints use a bare "text" field.
    if (text.isEmpty()) {
        text = choice.value(QStringLiteral("delta"))
                   .toObject()
                   .value(QStringLiteral("content"))
                   .toString();
    }
    if (text.isEmpty())
        text = choice.value(QStringLiteral("text")).toString();

    if (text.isEmpty()) {
        const QString finishReason =
            choice.value(QStringLiteral("finish_reason")).toString();
        if (finishReason == QStringLiteral("length"))
            return fail(tr("The reply was cut off before any text was produced. "
                           "Raise the maximum response length in the AI "
                           "settings."));
        if (finishReason == QStringLiteral("content_filter"))
            return fail(tr("The provider's content filter blocked the reply."));
        return fail(tr("The service replied successfully but the response "
                       "contained no text."));
    }

    return text;
}

QString AiClient::extractErrorMessage(const QByteArray& payload,
                                      AiWireFormat format)
{
    // `format` is part of the signature for symmetry with extractText() and
    // for future format-specific shapes; both current envelopes nest their
    // detail under the same "error" key, so one probe covers them.
    Q_UNUSED(format)

    if (payload.isEmpty()) return QString();

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        // Not JSON at all: a proxy, a captive portal or an HTML error page.
        return elide(QString::fromUtf8(payload), 200);
    }

    const QJsonObject root = doc.object();

    // Anthropic:        { "type": "error", "error": { "type", "message" } }
    // OpenAI-compatible:{ "error": { "message", "type", "code" } }
    // Both land on the same nested object, so probe it regardless of `format`
    // and only use `format` to decide which label reads better.
    const QJsonValue errorValue = root.value(QStringLiteral("error"));

    if (errorValue.isObject()) {
        const QJsonObject error = errorValue.toObject();
        const QString message = error.value(QStringLiteral("message")).toString();
        const QString type    = error.value(QStringLiteral("type")).toString();
        const QString code    = error.value(QStringLiteral("code")).toString();

        QStringList parts;
        if (!message.isEmpty()) parts.append(message);
        if (!type.isEmpty())    parts.append(QStringLiteral("[") + type
                                             + QStringLiteral("]"));
        else if (!code.isEmpty()) parts.append(QStringLiteral("[") + code
                                               + QStringLiteral("]"));
        if (!parts.isEmpty()) return parts.join(QLatin1Char(' '));
    }

    // Ollama and a few self-hosted servers return { "error": "some text" }.
    if (errorValue.isString() && !errorValue.toString().isEmpty())
        return errorValue.toString();

    // Last resort shapes seen in the wild.
    for (const QString& key : { QStringLiteral("message"),
                                QStringLiteral("detail"),
                                QStringLiteral("msg") }) {
        const QString value = root.value(key).toString();
        if (!value.isEmpty()) return value;
    }

    return QString();
}

QString AiClient::httpStatusExplanation(int status, AiWireFormat format)
{
    if (status >= 300 && status < 400) {
        return tr("the service redirected the request. Lumen does not follow "
                  "redirects, because that could forward your API key to "
                  "another host. Correct the base URL instead.");
    }

    switch (status) {
    case 400:
        return tr("the service rejected the request. The model id is usually "
                  "the culprit, or a parameter this endpoint does not accept.");
    case 401:
        return tr("the API key was rejected. Check that it was pasted in full, "
                  "has not been revoked, and belongs to this provider.");
    case 403:
        return tr("the key is recognised but not allowed to do this. It may "
                  "lack access to the requested model, or the account may be "
                  "restricted in your region.");
    case 404:
        return (format == AiWireFormat::Anthropic)
            ? tr("endpoint or model not found. The Anthropic base URL should "
                 "NOT include /v1 - Lumen appends /v1/messages itself.")
            : tr("endpoint or model not found. An OpenAI-compatible base URL "
                 "usually ends in /v1 - Lumen appends /chat/completions "
                 "itself.");
    case 408:
        return tr("the service timed out while handling the request.");
    case 413:
        return tr("the prompt was too large for this endpoint.");
    case 422:
        return tr("the service understood the request but refused the "
                  "contents.");
    case 429:
        return tr("rate limited, or the account is out of credit. Wait and "
                  "retry, or check your billing page.");
    case 500:
    case 502:
    case 503:
    case 504:
        return tr("the service failed on its side. This is not a problem with "
                  "your key or your settings - retry shortly.");
    default:
        break;
    }

    if (status >= 500)
        return tr("the service reported an internal error.");

    return tr("the request was not accepted.");
}

} // namespace lps
