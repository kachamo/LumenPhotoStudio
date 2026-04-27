// ==============================================================================
// src/project/ProjectDocument.h
// ==============================================================================
#pragma once

#include "core/Look.h"

#include <QDateTime>
#include <QString>

namespace lps {

struct ProjectDocument
{
    int schemaVersion = 1;
    QString projectName;
    QString projectPathReference;
    QString sourceImagePath;
    Look look;
    QString exportSettingsReference;
    QDateTime createdDate;
    QDateTime modifiedDate;

    void normalizeDates();
};

} // namespace lps
