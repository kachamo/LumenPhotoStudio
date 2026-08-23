// ==============================================================================
// src/ai/AiProvider.h
//
// Provider-agnostic description of an AI endpoint. Lumen ships NO API key, NO
// bundled vendor and NO telemetry: the user pastes their own key and points the
// app at whatever service they like.
//
// "Any AI API" is tractable because the market has settled on two request /
// response shapes, modelled here by AiWireFormat:
//
//   OpenAiCompatible  POST {baseUrl}/chat/completions
//                     Authorization: Bearer <key>
//                     { "model", "messages":[{role,content}], "max_tokens" }
//                     -> choices[0].message.content
//                     Covers OpenAI, Groq, Together, Mistral, OpenRouter,
//                     DeepSeek, xAI, LM Studio and Ollama (its /v1 shim).
//
//   Anthropic         POST {baseUrl}/v1/messages
//                     x-api-key: <key>   +   anthropic-version: 2023-06-01
//                     { "model", "max_tokens" (REQUIRED),
//                       "system": top-level string (NOT a message role),
//                       "messages":[{role,content}] }
//                     -> content[] blocks of {type:"text", text}
//
// Header-only by design: this is a value type with no behaviour beyond
// serialization, so it lives entirely in the header like src/util/*.h.
// ==============================================================================
#pragma once

#include <QCoreApplication>
#include <QJsonObject>
#include <QJsonValue>
#include <QMap>
#include <QString>
#include <QUrl>
#include <QVector>

namespace lps {

// ------------------------------------------------------------------------------
// Wire format. Persisted as a stable string, never as its integer value, so
// reordering this enum can never silently repoint a user's saved configuration.
// ------------------------------------------------------------------------------
enum class AiWireFormat {
    OpenAiCompatible = 0,
    Anthropic        = 1,
};

struct AiProvider
{
    // Stable, untranslated identifier of the preset this config came from
    // ("openai", "anthropic", "openrouter", "local", "custom"). Used for
    // lookup and persistence; never shown to the user.
    QString id = QStringLiteral("custom");

    // Shown in the preset combo. Translated for the generic entries; brand
    // names are deliberately left untranslated.
    QString displayName;

    // Base URL WITHOUT the endpoint path. The wire format decides what gets
    // appended (see endpointUrl()). Trailing slashes are tolerated.
    QString baseUrl;

    AiWireFormat wireFormat = AiWireFormat::OpenAiCompatible;

    QString modelId;

    // Optional per-provider headers (e.g. OpenRouter's HTTP-Referer / X-Title).
    // Never a place for the API key: the key is injected by AiClient from the
    // credential store and is never persisted alongside the provider config.
    QMap<QString, QString> extraHeaders;

    // Anthropic REQUIRES max_tokens. Sent for both formats for symmetry.
    int maxTokens = 1024;

    int timeoutMs = 60000;

    // ---- Derived state ----------------------------------------------------

    bool isValid() const
    {
        return !resolvedBaseUrl().isEmpty() && !modelId.trimmed().isEmpty();
    }

    // Trimmed base URL with any trailing slashes removed.
    QString resolvedBaseUrl() const
    {
        QString url = baseUrl.trimmed();
        while (url.endsWith(QLatin1Char('/')))
            url.chop(1);
        return url;
    }

    // Full URL the request is POSTed to.
    QUrl endpointUrl() const
    {
        const QString base = resolvedBaseUrl();
        if (base.isEmpty()) return QUrl();
        const QString path = (wireFormat == AiWireFormat::Anthropic)
            ? QStringLiteral("/v1/messages")
            : QStringLiteral("/chat/completions");
        return QUrl(base + path);
    }

    // Loopback endpoints (Ollama, LM Studio, llama.cpp, vLLM) normally need no
    // key at all, and plain http:// to them is not a credential leak.
    bool isLocalEndpoint() const
    {
        const QString host = QUrl(resolvedBaseUrl()).host().toLower();
        return host == QStringLiteral("localhost")
            || host == QStringLiteral("127.0.0.1")
            || host == QStringLiteral("::1")
            || host == QStringLiteral("0.0.0.0")
            || host.endsWith(QStringLiteral(".localhost"));
    }

    // ---- Serialization ----------------------------------------------------

    static QString wireFormatToString(AiWireFormat format)
    {
        return format == AiWireFormat::Anthropic
            ? QStringLiteral("anthropic")
            : QStringLiteral("openai-compatible");
    }

    static AiWireFormat wireFormatFromString(const QString& text)
    {
        return text.trimmed().compare(QStringLiteral("anthropic"),
                                      Qt::CaseInsensitive) == 0
            ? AiWireFormat::Anthropic
            : AiWireFormat::OpenAiCompatible;
    }

    static QString wireFormatLabel(AiWireFormat format)
    {
        return format == AiWireFormat::Anthropic
            ? QCoreApplication::translate("lps::AiProvider",
                                          "Anthropic (/v1/messages)")
            : QCoreApplication::translate("lps::AiProvider",
                                          "OpenAI-compatible (/chat/completions)");
    }

    QJsonObject toJson() const
    {
        QJsonObject headers;
        for (auto it = extraHeaders.constBegin(); it != extraHeaders.constEnd(); ++it)
            headers.insert(it.key(), it.value());

        QJsonObject obj;
        obj.insert(QStringLiteral("id"),           id);
        obj.insert(QStringLiteral("displayName"),  displayName);
        obj.insert(QStringLiteral("baseUrl"),      baseUrl);
        obj.insert(QStringLiteral("wireFormat"),   wireFormatToString(wireFormat));
        obj.insert(QStringLiteral("modelId"),      modelId);
        obj.insert(QStringLiteral("extraHeaders"), headers);
        obj.insert(QStringLiteral("maxTokens"),    maxTokens);
        obj.insert(QStringLiteral("timeoutMs"),    timeoutMs);
        return obj;
    }

    static AiProvider fromJson(const QJsonObject& obj)
    {
        AiProvider p;
        p.id          = obj.value(QStringLiteral("id")).toString(p.id);
        p.displayName = obj.value(QStringLiteral("displayName")).toString();
        p.baseUrl     = obj.value(QStringLiteral("baseUrl")).toString();
        p.wireFormat  = wireFormatFromString(
            obj.value(QStringLiteral("wireFormat")).toString());
        p.modelId     = obj.value(QStringLiteral("modelId")).toString();
        p.maxTokens   = obj.value(QStringLiteral("maxTokens")).toInt(p.maxTokens);
        p.timeoutMs   = obj.value(QStringLiteral("timeoutMs")).toInt(p.timeoutMs);

        const QJsonObject headers =
            obj.value(QStringLiteral("extraHeaders")).toObject();
        for (auto it = headers.constBegin(); it != headers.constEnd(); ++it)
            p.extraHeaders.insert(it.key(), it.value().toString());

        return p;
    }

    // ---- Built-in presets -------------------------------------------------
    //
    // Prefill only. Every one of these carries an empty key, and the model ids
    // are editable defaults rather than a claim about what any given account
    // can actually reach.
    // ------------------------------------------------------------------------
    static QVector<AiProvider> builtInPresets()
    {
        QVector<AiProvider> presets;

        AiProvider openai;
        openai.id          = QStringLiteral("openai");
        openai.displayName = QStringLiteral("OpenAI");
        openai.baseUrl     = QStringLiteral("https://api.openai.com/v1");
        openai.wireFormat  = AiWireFormat::OpenAiCompatible;
        openai.modelId     = QStringLiteral("gpt-4o-mini");
        presets.append(openai);

        AiProvider anthropic;
        anthropic.id          = QStringLiteral("anthropic");
        anthropic.displayName = QStringLiteral("Anthropic");
        anthropic.baseUrl     = QStringLiteral("https://api.anthropic.com");
        anthropic.wireFormat  = AiWireFormat::Anthropic;
        anthropic.modelId     = QStringLiteral("claude-opus-5");
        presets.append(anthropic);

        AiProvider openrouter;
        openrouter.id          = QStringLiteral("openrouter");
        openrouter.displayName = QStringLiteral("OpenRouter");
        openrouter.baseUrl     = QStringLiteral("https://openrouter.ai/api/v1");
        openrouter.wireFormat  = AiWireFormat::OpenAiCompatible;
        openrouter.modelId     = QStringLiteral("openai/gpt-4o-mini");
        presets.append(openrouter);

        AiProvider local;
        local.id          = QStringLiteral("local");
        local.displayName = QCoreApplication::translate(
            "lps::AiProvider", "Local (Ollama / LM Studio)");
        local.baseUrl     = QStringLiteral("http://localhost:11434/v1");
        local.wireFormat  = AiWireFormat::OpenAiCompatible;
        local.modelId     = QStringLiteral("llama3.2");
        presets.append(local);

        AiProvider custom;
        custom.id          = QStringLiteral("custom");
        custom.displayName = QCoreApplication::translate(
            "lps::AiProvider", "Custom (any compatible endpoint)");
        custom.baseUrl     = QString();
        custom.wireFormat  = AiWireFormat::OpenAiCompatible;
        custom.modelId     = QString();
        presets.append(custom);

        return presets;
    }

    // Returns the "custom" preset when the id is unknown, so a settings file
    // written by a newer build degrades into an editable custom entry rather
    // than an empty combo.
    static AiProvider presetById(const QString& id)
    {
        const QVector<AiProvider> presets = builtInPresets();
        for (const AiProvider& preset : presets) {
            if (preset.id.compare(id, Qt::CaseInsensitive) == 0)
                return preset;
        }
        return presets.isEmpty() ? AiProvider() : presets.last();
    }
};

} // namespace lps
