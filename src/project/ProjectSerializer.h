// ==============================================================================
// src/project/ProjectSerializer.h
// ==============================================================================
#pragma once

#include "project/ProjectDocument.h"

#include <QString>

namespace lps {

struct ProjectSaveResult
{
    bool ok = false;
    QString errorMessage;
};

struct ProjectLoadResult
{
    bool ok = false;
    QString errorMessage;
    ProjectDocument document;
};

class ProjectSerializer
{
public:
    static constexpr int kCurrentSchemaVersion = 1;

    static ProjectSaveResult saveToFile(const ProjectDocument& project,
                                        const QString& filePath);
    static ProjectLoadResult loadFromFile(const QString& filePath);
};

} // namespace lps
