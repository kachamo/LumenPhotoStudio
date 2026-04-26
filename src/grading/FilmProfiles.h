// ==============================================================================
// grading/FilmProfiles.h
// Registry of named "film look" profiles. A profile is currently just a path
// to a .cube LUT — enough to prove the architecture.
//
// Future (step 12+): profiles become bundles of {LUT, tone curve, color
// response} rather than a single LUT. Users load profiles from disk; we
// ship 2-3 as resources (Kodak Portra, Fuji Astia, Cinestill).
// ==============================================================================
#pragma once

#include <QString>
#include <QStringList>

namespace lps {

struct FilmProfile
{
    QString id;         // stable identifier used in Look.grading.filmProfileId
    QString name;       // display name
    QString lutPath;    // absolute path to the .cube file
};

class FilmProfiles
{
public:
    // Register a profile at runtime. Typically called at app startup from
    // the UI layer after scanning a "film profiles" folder.
    static void registerProfile(const FilmProfile& profile);

    // Remove all registered profiles. Useful for reloading or testing.
    static void clear();

    // List all registered profile ids.
    static QStringList availableIds();

    // Get a profile by id, or nullptr if not found.
    static const FilmProfile* find(const QString& id);

    // Convenience for ColorGrading::apply.
    static QString resolveLutPath(const QString& id);
};

} // namespace lps
