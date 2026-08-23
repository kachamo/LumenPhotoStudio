// ==============================================================================
// src/ai/AiSettings.h
//
// Persistence for the bring-your-own-key AI configuration. Follows the
// SettingsManager convention exactly: plain QSettings, one flat key per value,
// no cached state, so a value written here is immediately visible to every
// other reader in the process.
//
// SECURITY — read this before changing anything below.
//
// The API key is stored in QSettings in PLAIN TEXT. It is NOT encrypted. Qt has
// no built-in keychain/credential-store API, and inventing a local obfuscation
// scheme would only let us pretend the key is protected while an attacker with
// read access to the settings file (or the binary) recovers it trivially. So we
// do the honest thing: store it in the clear, tell the user exactly where it
// lands, and recommend a scoped, revocable key.
//
// TODO(security): add a QtKeychain-backed AiCredentialStore
//       (https://github.com/frankosterfeld/qtkeychain) so the key can live in
//       the Windows Credential Manager / macOS Keychain / Freedesktop Secret
//       Service instead. That is why credential access sits behind the
//       AiCredentialStore interface below rather than being inlined into
//       AiSettings: dropping in a KeychainCredentialStore should not require
//       touching any caller.
// ==============================================================================
#pragma once

#include "ai/AiProvider.h"

#include <QCoreApplication>
#include <QString>

#include <memory>

namespace lps {

// ------------------------------------------------------------------------------
// Pluggable credential backend.
//
// Implementations must never log, print or embed the key in an error string.
// ------------------------------------------------------------------------------
class AiCredentialStore
{
public:
    virtual ~AiCredentialStore();

    // Empty string means "no key stored". Never throws, never logs.
    virtual QString apiKey() const = 0;
    virtual void setApiKey(const QString& key) = 0;
    virtual void clearApiKey() = 0;

    // True only for a backend that actually protects the secret at rest.
    // The QSettings backend returns false, and the UI is required to surface
    // that honestly.
    virtual bool isSecureStorage() const = 0;

    // Human-readable description of where the secret physically lives, for the
    // warning shown in the settings dialog. Must not contain the key.
    virtual QString storageLocationDescription() const = 0;
};

// ------------------------------------------------------------------------------
// Default backend: QSettings, plain text. See the security note at the top.
// ------------------------------------------------------------------------------
class QSettingsCredentialStore final : public AiCredentialStore
{
public:
    QString apiKey() const override;
    void setApiKey(const QString& key) override;
    void clearApiKey() override;
    bool isSecureStorage() const override { return false; }
    QString storageLocationDescription() const override;
};

// ------------------------------------------------------------------------------
// AiSettings
//
// Provider configuration + credential access. Cheap to construct; construct one
// where you need it rather than passing it around, exactly like SettingsManager.
// ------------------------------------------------------------------------------
class AiSettings
{
    Q_DECLARE_TR_FUNCTIONS(lps::AiSettings)

public:
    // Uses the QSettings credential store when no backend is supplied.
    AiSettings();
    explicit AiSettings(std::shared_ptr<AiCredentialStore> credentialStore);

    // ---- Provider ---------------------------------------------------------

    // Returns the saved provider, or the "custom" preset when nothing has been
    // configured yet. Never returns a provider carrying a key.
    AiProvider provider() const;
    void setProvider(const AiProvider& provider);

    // ---- Credential -------------------------------------------------------

    QString apiKey() const;
    void setApiKey(const QString& key);
    void clearApiKey();
    bool hasApiKey() const;

    // ---- Readiness --------------------------------------------------------

    // A provider is usable when it has a base URL and a model, plus a key
    // unless it points at a loopback endpoint (local runtimes need no key).
    bool isConfigured() const;

    // Non-empty explanation of what is still missing, for the panel's empty
    // state. Empty when isConfigured() is true.
    QString configurationHint() const;

    // ---- Assistant behaviour ----------------------------------------------

    // System prompt sent with every chat turn. Editable so a user can retarget
    // the assistant; defaults to the photo-assistant persona.
    QString systemPrompt() const;
    void setSystemPrompt(const QString& prompt);
    static QString defaultSystemPrompt();

    // ---- Storage disclosure -----------------------------------------------

    // Absolute path of the file QSettings writes to (or the registry path on
    // Windows). Shown verbatim in the settings dialog.
    static QString settingsLocationDescription();

    // The plain-text-storage warning shown in the UI. Honest by contract: if
    // you ever add real encryption, change this text in the same commit.
    QString storageWarningText() const;

    bool usesSecureStorage() const;

private:
    std::shared_ptr<AiCredentialStore> m_credentials;
};

} // namespace lps
