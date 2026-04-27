// ==============================================================================
// io/ImageMetadataReader.h
// ==============================================================================
#pragma once

#include <QString>

namespace lps {

struct ImageMetadata
{
    QString cameraModel;
    QString lensModel;
    QString iso;
    QString aperture;
    QString shutterSpeed;
    QString focalLength;
    QString captureDateTime;
};

class ImageMetadataReader
{
public:
    static ImageMetadata read(const QString& path);
};

} // namespace lps
