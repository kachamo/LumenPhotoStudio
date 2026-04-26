// ==============================================================================
// grading/FilmProfiles.cpp
// ==============================================================================
#include "grading/FilmProfiles.h"

#include <QHash>
#include <QMutex>

namespace lps {

namespace {

QHash<QString, FilmProfile>& registry()
{
    static QHash<QString, FilmProfile> r;
    return r;
}

QMutex& registryMutex()
{
    static QMutex m;
    return m;
}

} // namespace

void FilmProfiles::registerProfile(const FilmProfile& profile)
{
    if (profile.id.isEmpty()) return;
    QMutexLocker lock(&registryMutex());
    registry()[profile.id] = profile;
}

void FilmProfiles::clear()
{
    QMutexLocker lock(&registryMutex());
    registry().clear();
}

QStringList FilmProfiles::availableIds()
{
    QMutexLocker lock(&registryMutex());
    return registry().keys();
}

const FilmProfile* FilmProfiles::find(const QString& id)
{
    QMutexLocker lock(&registryMutex());
    auto it = registry().find(id);
    return (it != registry().end()) ? &it.value() : nullptr;
}

QString FilmProfiles::resolveLutPath(const QString& id)
{
    if (const FilmProfile* p = find(id))
        return p->lutPath;
    return {};
}

} // namespace lps
