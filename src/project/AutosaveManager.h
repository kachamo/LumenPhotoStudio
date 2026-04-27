// ==============================================================================
// src/project/AutosaveManager.h
// ==============================================================================
#pragma once

#include "project/ProjectDocument.h"

#include <QObject>
#include <QString>
#include <QTimer>

namespace lps {

class AutosaveManager final : public QObject
{
public:
    explicit AutosaveManager(QObject* parent = nullptr);

    QString autosaveDirPath() const;
    QString latestAutosavePath() const;
    bool hasAutosave() const;

    void schedule(const ProjectDocument& document);
    bool saveNow(QString* error = nullptr);
    void clearPending();

    bool deleteAutosave(const QString& path = QString(), QString* error = nullptr);
    void deleteAllAutosaves();

private:
    QString makeAutosavePath() const;
    void pruneOldAutosaves(int keepCount);

    ProjectDocument m_pendingDocument;
    bool m_hasPendingDocument = false;
    QTimer m_timer;
    QString m_lastAutosavePath;
};

} // namespace lps
