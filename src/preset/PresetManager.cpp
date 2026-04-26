// ==============================================================================
// preset/PresetManager.cpp
// ==============================================================================
#include "preset/PresetManager.h"

#include "preset/LookSerializer.h"

#include <QDir>

#include <algorithm>

namespace lps {

PresetManager::PresetManager(QObject* parent)
    : QObject(parent)
{}

void PresetManager::setPresetsDirectory(const QString& dir)
{
    m_directory = dir;
    rescan();
}

void PresetManager::rescan()
{
    m_presets.clear();
    if (m_directory.isEmpty()) {
        emit presetsChanged();
        return;
    }

    QDir dir(m_directory);
    if (!dir.exists()) {
        emit presetsChanged();
        return;
    }

    const QStringList files = dir.entryList(QStringList() << "*.lxp", QDir::Files);
    for (const QString& f : files) {
        const LoadResult r = LookSerializer::loadFromFile(dir.absoluteFilePath(f));
        if (r.ok) m_presets.push_back(r.look);
        // Silent skip on failure. A UI layer might want to surface these,
        // but the engine stays quiet by design.
    }

    std::sort(m_presets.begin(), m_presets.end(),
              [](const Look& a, const Look& b) { return a.name < b.name; });

    emit presetsChanged();
}

QString PresetManager::save(const Look& look, const QString& fileName, QString* errorOut)
{
    if (m_directory.isEmpty()) {
        if (errorOut) *errorOut = tr("No presets directory set");
        return {};
    }
    const QString ensured = fileName.endsWith(".lxp") ? fileName : fileName + ".lxp";
    const QString path = QDir(m_directory).absoluteFilePath(ensured);

    const SaveResult r = LookSerializer::saveToFile(look, path);
    if (!r.ok) {
        if (errorOut) *errorOut = r.errorMessage;
        return {};
    }
    rescan();
    return path;
}

} // namespace lps
