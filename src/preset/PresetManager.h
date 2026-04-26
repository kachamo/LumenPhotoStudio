// ==============================================================================
// preset/PresetManager.h
// Scans a folder for .lxp files, keeps them in memory for the UI to list
// and apply. No built-in presets bundled here — V1 expects the UI/app layer
// to decide what defaults to ship (so the engine library stays asset-free).
// ==============================================================================
#pragma once

#include "core/Look.h"

#include <QObject>
#include <QString>

#include <vector>

namespace lps {

class PresetManager : public QObject
{
    Q_OBJECT

public:
    explicit PresetManager(QObject* parent = nullptr);

    // Directory to scan for .lxp files. Call once at startup.
    void setPresetsDirectory(const QString& dir);

    // Re-read the directory. Emits presetsChanged() on completion.
    void rescan();

    // Loaded presets. Ordered alphabetically by name.
    const std::vector<Look>& presets() const { return m_presets; }

    // Save a Look as an .lxp file in the presets directory.
    // Returns the path on success; empty on failure (check errorOut).
    QString save(const Look& look, const QString& fileName, QString* errorOut = nullptr);

signals:
    void presetsChanged();

private:
    QString m_directory;
    std::vector<Look> m_presets;
};

} // namespace lps
