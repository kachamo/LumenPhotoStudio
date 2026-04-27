// ==============================================================================
// io/RawImageLoader.h
// ==============================================================================
#pragma once

#include <QImage>
#include <QString>

namespace lps {

struct RawDevelopSettings
{
    QString cameraProfile;
    QString whiteBalanceMode = QStringLiteral("As Shot");
    bool    highlightRecovery = false;
    int     demosaicQuality = 3;
    QString colorSpace = QStringLiteral("sRGB");
};

class RawImageLoader
{
public:
    static bool isRawExtension(const QString& path);
    static bool isRawSupportAvailable();
    static QImage load(const QString& path,
                       QString* error = nullptr,
                       const RawDevelopSettings& settings = {});
};

} // namespace lps
