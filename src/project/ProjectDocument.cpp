// ==============================================================================
// src/project/ProjectDocument.cpp
// ==============================================================================
#include "project/ProjectDocument.h"

namespace lps {

void ProjectDocument::normalizeDates()
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    if (!createdDate.isValid())
        createdDate = now;
    if (!modifiedDate.isValid())
        modifiedDate = now;
}

} // namespace lps
