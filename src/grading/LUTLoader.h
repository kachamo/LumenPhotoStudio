// ==============================================================================
// grading/LUTLoader.h
// Parse Adobe .cube LUT files.
//
// Format supported:
//   - LUT_3D_SIZE N         (N^3 entries)
//   - LUT_1D_SIZE N         (N entries)
//   - DOMAIN_MIN / DOMAIN_MAX
//   - TITLE "..."           (ignored)
//   - '#' comment lines
//   - Data rows: three floats per line (R G B), in [0, 1] after domain scaling
//
// Format NOT yet supported: 3D shaper LUTs, 1D shaper + 3D combined. Step 12.
// ==============================================================================
#pragma once

#include <QString>
#include <QVector3D>

#include <vector>

namespace lps {

struct LUTData
{
    // Exactly one of these is populated depending on LUT type.
    bool is3D = false;
    int  size = 0;                     // N for 3D means N×N×N entries

    // For 3D LUTs: size^3 entries, indexed by [r*size*size + g*size + b].
    // For 1D LUTs: `size` entries, indexed by input value.
    std::vector<QVector3D> entries;

    // Domain range (typically 0..1; rare LUTs use 0..16 for HDR).
    QVector3D domainMin = QVector3D(0.0f, 0.0f, 0.0f);
    QVector3D domainMax = QVector3D(1.0f, 1.0f, 1.0f);

    // Human-readable title if the file specified one.
    QString   title;
};

struct LUTLoadResult
{
    bool    ok = false;
    QString errorMessage;
    LUTData lut;
};

class LUTLoader
{
public:
    // Parse a .cube file from disk.
    static LUTLoadResult loadCube(const QString& filePath);
};

} // namespace lps
