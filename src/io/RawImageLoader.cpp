// ==============================================================================
// io/RawImageLoader.cpp
// ==============================================================================
#include "io/RawImageLoader.h"

#include <QByteArray>
#include <QFileInfo>
#include <QStringList>

#ifdef LPS_HAS_LIBRAW
#include <libraw/libraw.h>
#endif

#include <algorithm>

namespace lps {

namespace {

constexpr const char* kRawSupportMessage = "RAW support requires LibRaw";

void setError(QString* error, const QString& message)
{
    if (error) *error = message;
}

} // namespace

bool RawImageLoader::isRawExtension(const QString& path)
{
    const QString ext = QFileInfo(path).suffix().toLower();
    static const QStringList exts = {
        QStringLiteral("cr2"),
        QStringLiteral("cr3"),
        QStringLiteral("nef"),
        QStringLiteral("arw"),
        QStringLiteral("dng"),
        QStringLiteral("raf"),
        QStringLiteral("orf"),
        QStringLiteral("rw2"),
    };
    return exts.contains(ext);
}

bool RawImageLoader::isRawSupportAvailable()
{
#ifdef LPS_HAS_LIBRAW
    return true;
#else
    return false;
#endif
}

QImage RawImageLoader::load(const QString& path,
                            QString* error,
                            const RawDevelopSettings& settings)
{
    Q_UNUSED(settings);

#ifndef LPS_HAS_LIBRAW
    Q_UNUSED(path);
    setError(error, QString::fromLatin1(kRawSupportMessage));
    return QImage();
#else
    LibRaw raw;

    const QByteArray fileName = QFileInfo(path).absoluteFilePath().toLocal8Bit();
    int rc = raw.open_file(fileName.constData());
    if (rc != LIBRAW_SUCCESS) {
        setError(error, QString::fromLatin1(libraw_strerror(rc)));
        return QImage();
    }

    raw.imgdata.params.output_bps = 8;
    raw.imgdata.params.output_color = 1;
    raw.imgdata.params.use_camera_wb = 1;
    raw.imgdata.params.no_auto_bright = 1;
    raw.imgdata.params.user_qual = std::clamp(settings.demosaicQuality, 0, 12);

    rc = raw.unpack();
    if (rc != LIBRAW_SUCCESS) {
        setError(error, QString::fromLatin1(libraw_strerror(rc)));
        return QImage();
    }

    rc = raw.dcraw_process();
    if (rc != LIBRAW_SUCCESS) {
        setError(error, QString::fromLatin1(libraw_strerror(rc)));
        return QImage();
    }

    libraw_processed_image_t* processed = raw.dcraw_make_mem_image(&rc);
    if (!processed || rc != LIBRAW_SUCCESS) {
        if (processed) LibRaw::dcraw_clear_mem(processed);
        setError(error, QString::fromLatin1(libraw_strerror(rc)));
        return QImage();
    }

    QImage image;
    if (processed->type == LIBRAW_IMAGE_BITMAP &&
        processed->colors >= 3 &&
        processed->bits == 8) {
        image = QImage(processed->width,
                       processed->height,
                       QImage::Format_RGB888);
        const int srcStride = processed->width * processed->colors;
        for (uint y = 0; y < processed->height; ++y) {
            const uchar* src = processed->data + y * srcStride;
            uchar* dst = image.scanLine(static_cast<int>(y));
            for (uint x = 0; x < processed->width; ++x) {
                dst[x * 3 + 0] = src[x * processed->colors + 0];
                dst[x * 3 + 1] = src[x * processed->colors + 1];
                dst[x * 3 + 2] = src[x * processed->colors + 2];
            }
        }
    } else {
        setError(error, QStringLiteral("Unsupported RAW output format"));
    }

    LibRaw::dcraw_clear_mem(processed);
    return image;
#endif
}

} // namespace lps
