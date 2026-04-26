// ==============================================================================
// grading/LUTLoader.cpp
//
// Parser tolerates:
//   - Windows and Unix line endings
//   - Blank lines
//   - '#' comments (strip after #)
//   - Leading/trailing whitespace
//   - TITLE "Foo Bar" (quotes required per spec; we're lenient)
// ==============================================================================
#include "grading/LUTLoader.h"

#include <QFile>
#include <QObject>
#include <QTextStream>

namespace lps {

namespace {

// Strip comment and trim.
QString cleanLine(QString line)
{
    const int hash = line.indexOf('#');
    if (hash >= 0) line = line.left(hash);
    return line.trimmed();
}

// Parse a "TITLE" line, returning the title string. Handles both
// `TITLE "Foo Bar"` and `TITLE Foo Bar`.
QString parseTitle(const QString& line)
{
    QString rest = line.mid(5).trimmed();  // skip "TITLE"
    if (rest.startsWith('"') && rest.endsWith('"') && rest.size() >= 2)
        rest = rest.mid(1, rest.size() - 2);
    return rest;
}

} // namespace

LUTLoadResult LUTLoader::loadCube(const QString& filePath)
{
    LUTLoadResult result;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        result.errorMessage = QObject::tr("Cannot open LUT file: %1").arg(file.errorString());
        return result;
    }

    QTextStream ts(&file);
    LUTData lut;

    int expectedEntries = 0;

    while (!ts.atEnd()) {
        const QString raw = ts.readLine();
        const QString line = cleanLine(raw);
        if (line.isEmpty()) continue;

        // Header keywords
        if (line.startsWith("TITLE", Qt::CaseInsensitive)) {
            lut.title = parseTitle(line);
            continue;
        }
        if (line.startsWith("LUT_3D_SIZE", Qt::CaseInsensitive)) {
            const QStringList parts = line.split(' ', Qt::SkipEmptyParts);
            if (parts.size() < 2) {
                result.errorMessage = QObject::tr("Malformed LUT_3D_SIZE line");
                return result;
            }
            lut.is3D = true;
            lut.size = parts[1].toInt();
            expectedEntries = lut.size * lut.size * lut.size;
            lut.entries.reserve(static_cast<size_t>(expectedEntries));
            continue;
        }
        if (line.startsWith("LUT_1D_SIZE", Qt::CaseInsensitive)) {
            const QStringList parts = line.split(' ', Qt::SkipEmptyParts);
            if (parts.size() < 2) {
                result.errorMessage = QObject::tr("Malformed LUT_1D_SIZE line");
                return result;
            }
            lut.is3D = false;
            lut.size = parts[1].toInt();
            expectedEntries = lut.size;
            lut.entries.reserve(static_cast<size_t>(expectedEntries));
            continue;
        }
        if (line.startsWith("DOMAIN_MIN", Qt::CaseInsensitive)) {
            const QStringList parts = line.split(' ', Qt::SkipEmptyParts);
            if (parts.size() >= 4) {
                lut.domainMin = QVector3D(
                    parts[1].toFloat(), parts[2].toFloat(), parts[3].toFloat());
            }
            continue;
        }
        if (line.startsWith("DOMAIN_MAX", Qt::CaseInsensitive)) {
            const QStringList parts = line.split(' ', Qt::SkipEmptyParts);
            if (parts.size() >= 4) {
                lut.domainMax = QVector3D(
                    parts[1].toFloat(), parts[2].toFloat(), parts[3].toFloat());
            }
            continue;
        }

        // Data row: three floats.
        const QStringList parts = line.split(' ', Qt::SkipEmptyParts);
        if (parts.size() < 3) {
            // Unknown line — skip rather than fail.
            continue;
        }
        bool okR = false, okG = false, okB = false;
        const float r = parts[0].toFloat(&okR);
        const float g = parts[1].toFloat(&okG);
        const float b = parts[2].toFloat(&okB);
        if (!okR || !okG || !okB) continue;
        lut.entries.emplace_back(r, g, b);
    }

    if (lut.size == 0) {
        result.errorMessage = QObject::tr("LUT file is missing LUT_3D_SIZE or LUT_1D_SIZE");
        return result;
    }
    if (static_cast<int>(lut.entries.size()) != expectedEntries) {
        result.errorMessage = QObject::tr("LUT file expected %1 entries, got %2")
            .arg(expectedEntries).arg(lut.entries.size());
        return result;
    }

    result.ok = true;
    result.lut = std::move(lut);
    return result;
}

} // namespace lps
