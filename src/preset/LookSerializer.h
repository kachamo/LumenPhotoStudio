// ==============================================================================
// preset/LookSerializer.h
// Read / write .lxp (Lumen Preset) files. JSON format.
//
// When extending Look, extend this in lockstep. The loader is lenient about
// missing fields (they default to identity), so older .lxp files remain
// compatible when new fields are added. Bump Look::schemaVersion if a
// breaking change is made.
// ==============================================================================
#pragma once

#include "core/Look.h"

#include <QJsonObject>
#include <QString>

namespace lps {

struct SaveResult  { bool ok = false; QString errorMessage; };
struct LoadResult  { bool ok = false; QString errorMessage; Look look; };

class LookSerializer
{
public:
    // In-memory conversions.
    static QJsonObject toJson(const Look& look);
    static bool        fromJson(const QJsonObject& obj, Look& out, QString* errorOut = nullptr);

    // File I/O (.lxp extension is a convention, not enforced).
    static SaveResult saveToFile(const Look& look, const QString& filePath);
    static LoadResult loadFromFile(const QString& filePath);
};

} // namespace lps
