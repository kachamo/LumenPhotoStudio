// ==============================================================================
// src/ai/AiSettings.cpp
// ==============================================================================
#include "ai/AiSettings.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSettings>

#include <utility>

namespace {

// Keys live under an "ai/" group, matching SettingsManager's "group/key" style.
constexpr auto kKeyPresetId     = "ai/presetId";
constexpr auto kKeyDisplayName  = "ai/displayName";
constexpr auto kKeyBaseUrl      = "ai/baseUrl";
constexpr auto kKeyWireFormat   = "ai/wireFormat";
constexpr auto kKeyModelId      = "ai/modelId";
constexpr auto kKeyExtraHeaders = "ai/extraHeaders";
constexpr auto kKeyMaxTokens    = "ai/maxTokens";
constexpr auto kKeyTimeoutMs    = "ai/timeoutMs";
constexpr auto kKeySystemPrompt = "ai/systemPrompt";

// Deliberately named so it is obvious in the settings file that this value is
// a secret sitting in the clear.
constexpr auto kKeyApiKeyPlain  = "ai/apiKeyPlaintext";

constexpr int kMinMaxTokens = 16;
constexpr int kMaxMaxTokens = 32768;
constexpr int kMinTimeoutMs = 1000;
constexpr int kMaxTimeoutMs = 600000;

int clampInt(int value, int lo, int hi)
{
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

} // namespace

namespace lps {

// ------------------------------------------------------------------------------
// AiCredentialStore
// ------------------------------------------------------------------------------

AiCredentialStore::~AiCredentialStore() = default;

// ------------------------------------------------------------------------------
// QSettingsCredentialStore
// ------------------------------------------------------------------------------

QString QSettingsCredentialStore::apiKey() const
{
    return QSettings().value(QLatin1String(kKeyApiKeyPlain)).toString();
}

void QSettingsCredentialStore::setApiKey(const QString& key)
{
    const QString trimmed = key.trimmed();
    if (trimmed.isEmpty()) {
        clearApiKey();
        return;
    }
    QSettings().setValue(QLatin1String(kKeyApiKeyPlain), trimmed);
}

void QSettingsCredentialStore::clearApiKey()
{
    QSettings().remove(QLatin1String(kKeyApiKeyPlain));
}

QString QSettingsCredentialStore::storageLocationDescription() const
{
    return AiSettings::settingsLocationDescription();
}

// ------------------------------------------------------------------------------
// AiSettings
// ------------------------------------------------------------------------------

AiSettings::AiSettings()
    : m_credentials(std::make_shared<QSettingsCredentialStore>())
{
}

AiSettings::AiSettings(std::shared_ptr<AiCredentialStore> credentialStore)
    : m_credentials(credentialStore ? std::move(credentialStore)
                                    : std::static_pointer_cast<AiCredentialStore>(
                                          std::make_shared<QSettingsCredentialStore>()))
{
}

AiProvider AiSettings::provider() const
{
    QSettings settings;

    // Nothing saved yet: hand back the neutral "custom" preset so the dialog
    // opens on an empty, editable form instead of a half-populated one.
    if (!settings.contains(QLatin1String(kKeyBaseUrl)))
        return AiProvider::presetById(QStringLiteral("custom"));

    AiProvider provider;
    provider.id = settings.value(QLatin1String(kKeyPresetId),
                                 QStringLiteral("custom")).toString();
    provider.displayName =
        settings.value(QLatin1String(kKeyDisplayName)).toString();
    provider.baseUrl =
        settings.value(QLatin1String(kKeyBaseUrl)).toString();
    provider.wireFormat = AiProvider::wireFormatFromString(
        settings.value(QLatin1String(kKeyWireFormat)).toString());
    provider.modelId =
        settings.value(QLatin1String(kKeyModelId)).toString();
    provider.maxTokens = clampInt(
        settings.value(QLatin1String(kKeyMaxTokens), 1024).toInt(),
        kMinMaxTokens, kMaxMaxTokens);
    provider.timeoutMs = clampInt(
        settings.value(QLatin1String(kKeyTimeoutMs), 60000).toInt(),
        kMinTimeoutMs, kMaxTimeoutMs);

    // Extra headers round-trip as a compact JSON object rather than a
    // QVariantMap, so the settings file stays readable and portable.
    const QByteArray raw =
        settings.value(QLatin1String(kKeyExtraHeaders)).toString().toUtf8();
    if (!raw.isEmpty()) {
        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(raw, &parseError);
        if (parseError.error == QJsonParseError::NoError && doc.isObject()) {
            const QJsonObject obj = doc.object();
            for (auto it = obj.constBegin(); it != obj.constEnd(); ++it)
                provider.extraHeaders.insert(it.key(), it.value().toString());
        }
    }

    if (provider.displayName.isEmpty())
        provider.displayName = AiProvider::presetById(provider.id).displayName;

    return provider;
}

void AiSettings::setProvider(const AiProvider& provider)
{
    QSettings settings;
    settings.setValue(QLatin1String(kKeyPresetId),    provider.id);
    settings.setValue(QLatin1String(kKeyDisplayName), provider.displayName);
    settings.setValue(QLatin1String(kKeyBaseUrl),     provider.baseUrl.trimmed());
    settings.setValue(QLatin1String(kKeyWireFormat),
                      AiProvider::wireFormatToString(provider.wireFormat));
    settings.setValue(QLatin1String(kKeyModelId),     provider.modelId.trimmed());
    settings.setValue(QLatin1String(kKeyMaxTokens),
                      clampInt(provider.maxTokens, kMinMaxTokens, kMaxMaxTokens));
    settings.setValue(QLatin1String(kKeyTimeoutMs),
                      clampInt(provider.timeoutMs, kMinTimeoutMs, kMaxTimeoutMs));

    QJsonObject headers;
    for (auto it = provider.extraHeaders.constBegin();
         it != provider.extraHeaders.constEnd(); ++it) {
        if (it.key().trimmed().isEmpty()) continue;
        headers.insert(it.key(), it.value());
    }
    settings.setValue(QLatin1String(kKeyExtraHeaders),
                      QString::fromUtf8(QJsonDocument(headers)
                                            .toJson(QJsonDocument::Compact)));
}

QString AiSettings::apiKey() const
{
    return m_credentials ? m_credentials->apiKey() : QString();
}

void AiSettings::setApiKey(const QString& key)
{
    if (m_credentials) m_credentials->setApiKey(key);
}

void AiSettings::clearApiKey()
{
    if (m_credentials) m_credentials->clearApiKey();
}

bool AiSettings::hasApiKey() const
{
    return !apiKey().isEmpty();
}

bool AiSettings::isConfigured() const
{
    const AiProvider p = provider();
    if (!p.isValid()) return false;
    return p.isLocalEndpoint() || hasApiKey();
}

QString AiSettings::configurationHint() const
{
    const AiProvider p = provider();

    if (p.resolvedBaseUrl().isEmpty())
        return tr("No AI service is configured yet. Add a base URL, a model "
                  "and your own API key to get started.");

    if (p.modelId.trimmed().isEmpty())
        return tr("No model has been set for %1. Enter the model id your "
                  "account or local runtime exposes.")
            .arg(p.displayName.isEmpty() ? p.resolvedBaseUrl() : p.displayName);

    if (!p.isLocalEndpoint() && !hasApiKey())
        return tr("No API key has been saved. Paste your own key to enable the "
                  "assistant. Lumen never ships or bundles a key.");

    return QString();
}

QString AiSettings::systemPrompt() const
{
    const QString saved =
        QSettings().value(QLatin1String(kKeySystemPrompt)).toString();
    return saved.trimmed().isEmpty() ? defaultSystemPrompt() : saved;
}

void AiSettings::setSystemPrompt(const QString& prompt)
{
    QSettings settings;
    if (prompt.trimmed().isEmpty() || prompt.trimmed() == defaultSystemPrompt()) {
        settings.remove(QLatin1String(kKeySystemPrompt));
        return;
    }
    settings.setValue(QLatin1String(kKeySystemPrompt), prompt.trimmed());
}

QString AiSettings::defaultSystemPrompt()
{
    return tr("You are a photo-editing assistant inside Lumen Photo Studio, a "
              "non-destructive raw editor with exposure, contrast, highlights, "
              "shadows, whites, blacks, white balance, vibrance, saturation, "
              "HSL, tone curves, colour grading wheels, clarity, texture, "
              "sharpening, noise reduction, lens corrections, transform, "
              "vignette and grain controls. Answer concisely and practically. "
              "Recommend concrete slider moves and explain the reasoning in one "
              "or two sentences. You cannot see the user's photo unless they "
              "describe it, so ask a short clarifying question when the answer "
              "genuinely depends on the image.");
}

QString AiSettings::settingsLocationDescription()
{
    QString path = QSettings().fileName();
    if (path.isEmpty())
        return tr("the application settings store");

    // With the native backend on Windows there is no file: QSettings reports a
    // registry key path (with a leading backslash). Label it, so the user knows
    // what to actually go and inspect or delete.
    if (path.startsWith(QLatin1String("\\HKEY"))
        || path.startsWith(QLatin1String("HKEY"))) {
        while (path.startsWith(QLatin1Char('\\')))
            path.remove(0, 1);
        return tr("the Windows registry, under %1").arg(path);
    }

    return path;
}

QString AiSettings::storageWarningText() const
{
    if (usesSecureStorage()) {
        return tr("Your API key is stored by the operating system credential "
                  "store (%1).")
            .arg(m_credentials->storageLocationDescription());
    }

    return tr("Your API key is stored in PLAIN TEXT, unencrypted, in:\n%1\n\n"
              "Lumen does not encrypt it — Qt has no built-in keychain, and "
              "local obfuscation would only look like protection. Anyone who "
              "can read that location, or your backups, can read the key.\n\n"
              "Use a scoped, revocable key with a spending limit, and revoke it "
              "if this machine is shared or lost. The key is sent only to the "
              "base URL you configured, only when you send a prompt, and is "
              "never logged or included in error messages.")
        .arg(m_credentials ? m_credentials->storageLocationDescription()
                           : settingsLocationDescription());
}

bool AiSettings::usesSecureStorage() const
{
    return m_credentials && m_credentials->isSecureStorage();
}

} // namespace lps
