// ==============================================================================
// io/ImageMetadataReader.cpp
// ==============================================================================
#include "io/ImageMetadataReader.h"

#include "io/RawImageLoader.h"

#include <QByteArray>
#include <QDateTime>
#include <QFileInfo>
#include <QImageReader>
#include <QLocale>
#include <QStringList>

#ifdef LPS_HAS_LIBRAW
#include <libraw/libraw.h>
#endif

#include <cmath>
#include <initializer_list>

namespace {

QString clean(QString value)
{
    value = value.trimmed();
    if ((value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"'))) ||
        (value.startsWith(QLatin1Char('\'')) && value.endsWith(QLatin1Char('\'')))) {
        value = value.mid(1, value.size() - 2).trimmed();
    }
    return value;
}

QString normalizeKey(QString key)
{
    key = key.toLower();
    key.remove(QLatin1Char('.'));
    key.remove(QLatin1Char('_'));
    key.remove(QLatin1Char('-'));
    key.remove(QLatin1Char(' '));
    key.remove(QLatin1Char(':'));
    return key;
}

bool matchesAny(const QString& normalizedKey, std::initializer_list<const char*> aliases)
{
    for (const char* alias : aliases) {
        const QString normalizedAlias = normalizeKey(QString::fromLatin1(alias));
        if (normalizedKey == normalizedAlias || normalizedKey.endsWith(normalizedAlias))
            return true;
    }
    return false;
}

QString readText(QImageReader& reader, std::initializer_list<const char*> aliases)
{
    const QStringList keys = reader.textKeys();
    for (const QString& key : keys) {
        const QString normalized = normalizeKey(key);
        if (!matchesAny(normalized, aliases))
            continue;

        const QString value = clean(reader.text(key));
        if (!value.isEmpty())
            return value;
    }
    return {};
}

bool parseDouble(const QString& text, double& out)
{
    QString value = clean(text);
    value.remove(QStringLiteral("mm"), Qt::CaseInsensitive);
    value.remove(QStringLiteral("sec"), Qt::CaseInsensitive);
    value.remove(QStringLiteral("s"), Qt::CaseInsensitive);
    value.remove(QStringLiteral("f"), Qt::CaseInsensitive);
    value.remove(QLatin1Char('/'));
    value = value.trimmed();

    bool ok = false;
    out = QLocale::c().toDouble(value, &ok);
    if (ok && std::isfinite(out))
        return true;

    out = QLocale().toDouble(value, &ok);
    return ok && std::isfinite(out);
}

bool parseRational(const QString& text, double& out)
{
    const QString value = clean(text);
    const int slash = value.indexOf(QLatin1Char('/'));
    if (slash <= 0)
        return parseDouble(value, out);

    bool okNum = false;
    bool okDen = false;
    const double num = QLocale::c().toDouble(value.left(slash).trimmed(), &okNum);
    const double den = QLocale::c().toDouble(value.mid(slash + 1).trimmed(), &okDen);
    if (!okNum || !okDen || den == 0.0)
        return false;
    out = num / den;
    return std::isfinite(out);
}

QString formatDecimal(double value, int decimals = 1)
{
    if (!std::isfinite(value))
        return {};
    QString text = QString::number(value, 'f', decimals);
    while (text.contains(QLatin1Char('.')) && text.endsWith(QLatin1Char('0')))
        text.chop(1);
    if (text.endsWith(QLatin1Char('.')))
        text.chop(1);
    return text;
}

QString formatAperture(QString value)
{
    value = clean(value);
    if (value.isEmpty())
        return {};
    if (value.startsWith(QStringLiteral("f/"), Qt::CaseInsensitive))
        return value;

    double aperture = 0.0;
    if (!parseRational(value, aperture) || aperture <= 0.0)
        return value;
    return QStringLiteral("f/%1").arg(formatDecimal(aperture, 1));
}

QString formatFocalLength(QString value)
{
    value = clean(value);
    if (value.isEmpty())
        return {};
    if (value.contains(QStringLiteral("mm"), Qt::CaseInsensitive))
        return value;

    double focal = 0.0;
    if (!parseRational(value, focal) || focal <= 0.0)
        return value;
    return QStringLiteral("%1 mm").arg(formatDecimal(focal, 1));
}

QString formatShutter(QString value)
{
    value = clean(value);
    if (value.isEmpty())
        return {};

    double seconds = 0.0;
    if (!parseRational(value, seconds) || seconds <= 0.0)
        return value;
    if (seconds < 1.0) {
        const int denom = static_cast<int>(std::round(1.0 / seconds));
        if (denom > 0)
            return QStringLiteral("1/%1 s").arg(denom);
    }
    return QStringLiteral("%1 s").arg(formatDecimal(seconds, 2));
}

QString formatIso(QString value)
{
    value = clean(value);
    if (value.startsWith(QStringLiteral("ISO "), Qt::CaseInsensitive))
        value = value.mid(4).trimmed();
    return value;
}

QString formatDateTime(QString value)
{
    value = clean(value);
    if (value.isEmpty())
        return {};

    QDateTime dt = QDateTime::fromString(value, QStringLiteral("yyyy:MM:dd HH:mm:ss"));
    if (!dt.isValid())
        dt = QDateTime::fromString(value, Qt::ISODate);
    if (!dt.isValid())
        dt = QDateTime::fromString(value, Qt::ISODateWithMs);
    return dt.isValid()
        ? dt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
        : value;
}

#ifdef LPS_HAS_LIBRAW
QString fromRawString(const char* value)
{
    return clean(QString::fromLocal8Bit(value));
}

void mergeRawMetadata(const QString& path, lps::ImageMetadata& out)
{
    if (!lps::RawImageLoader::isRawExtension(path))
        return;

    LibRaw raw;
    const QByteArray fileName = QFileInfo(path).absoluteFilePath().toLocal8Bit();
    const int rc = raw.open_file(fileName.constData());
    if (rc != LIBRAW_SUCCESS)
        return;

    if (out.cameraModel.isEmpty()) {
        const QString model = fromRawString(raw.imgdata.idata.model);
        const QString make = fromRawString(raw.imgdata.idata.make);
        out.cameraModel = make.isEmpty() || model.startsWith(make, Qt::CaseInsensitive)
            ? model
            : make + QLatin1Char(' ') + model;
    }
    if (out.lensModel.isEmpty())
        out.lensModel = fromRawString(raw.imgdata.lens.Lens);
    if (out.iso.isEmpty() && raw.imgdata.other.iso_speed > 0.0f)
        out.iso = QString::number(static_cast<int>(std::round(raw.imgdata.other.iso_speed)));
    if (out.aperture.isEmpty() && raw.imgdata.other.aperture > 0.0f)
        out.aperture = QStringLiteral("f/%1").arg(formatDecimal(raw.imgdata.other.aperture, 1));
    if (out.shutterSpeed.isEmpty() && raw.imgdata.other.shutter > 0.0f)
        out.shutterSpeed = formatShutter(QString::number(raw.imgdata.other.shutter, 'f', 8));
    if (out.focalLength.isEmpty() && raw.imgdata.other.focal_len > 0.0f)
        out.focalLength = QStringLiteral("%1 mm").arg(formatDecimal(raw.imgdata.other.focal_len, 1));
    if (out.captureDateTime.isEmpty() && raw.imgdata.other.timestamp > 0) {
        out.captureDateTime = QDateTime::fromSecsSinceEpoch(
            static_cast<qint64>(raw.imgdata.other.timestamp)).toString(
                QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    }
}
#else
void mergeRawMetadata(const QString&, lps::ImageMetadata&)
{
}
#endif

} // namespace

namespace lps {

ImageMetadata ImageMetadataReader::read(const QString& path)
{
    ImageMetadata out;

    QImageReader reader(path);
    out.cameraModel = readText(reader, {
        "Model", "CameraModel", "Exif.Image.Model", "Exif.Photo.CameraModel"
    });
    out.lensModel = readText(reader, {
        "Lens", "LensModel", "Exif.Photo.LensModel", "Exif.Photo.LensSpecification"
    });
    out.iso = formatIso(readText(reader, {
        "ISO", "ISOSpeedRatings", "PhotographicSensitivity",
        "Exif.Photo.ISOSpeedRatings", "Exif.Photo.PhotographicSensitivity"
    }));
    out.aperture = formatAperture(readText(reader, {
        "FNumber", "ApertureValue", "Exif.Photo.FNumber", "Exif.Photo.ApertureValue"
    }));
    out.shutterSpeed = formatShutter(readText(reader, {
        "ExposureTime", "ShutterSpeedValue", "Exif.Photo.ExposureTime",
        "Exif.Photo.ShutterSpeedValue"
    }));
    out.focalLength = formatFocalLength(readText(reader, {
        "FocalLength", "Exif.Photo.FocalLength"
    }));
    out.captureDateTime = formatDateTime(readText(reader, {
        "DateTimeOriginal", "DateTimeDigitized", "DateTime",
        "Exif.Photo.DateTimeOriginal", "Exif.Photo.DateTimeDigitized",
        "Exif.Image.DateTime"
    }));

    mergeRawMetadata(path, out);
    return out;
}

} // namespace lps
