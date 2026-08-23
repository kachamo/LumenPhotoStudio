// ==============================================================================
// io/RawImageLoader.cpp
//
// Decodes a camera RAW file into a 16-bit-per-channel, sRGB-ENCODED QImage.
//
// The bit depth is the whole point of this file. A CR3 carries 12-14 bits of
// sensor data; handing the pipeline an 8-bit image throws that away before
// the first adjustment runs and leaves a RAW file with the editing latitude
// of a JPEG. We ask LibRaw for output_bps = 16 and build a
// QImage::Format_RGBX64, which PixelBuffer::fromSrgbImage() ingests at full
// depth.
//
// The encoding is the other half of the contract, and it is easy to get
// wrong — see the long comment on params.gamm[] in load().
// ==============================================================================
#include "io/RawImageLoader.h"

#include <QByteArray>
#include <QFileInfo>
#include <QRgba64>
#include <QStringList>

#ifdef LPS_HAS_LIBRAW
#include <libraw/libraw.h>
#endif

#include <algorithm>
#include <cstddef>

namespace lps {

namespace {

constexpr const char* kRawSupportMessage = "RAW support requires LibRaw";

void setError(QString* error, const QString& message)
{
    if (error) *error = message;
}

#ifdef LPS_HAS_LIBRAW

// ---- RawDevelopSettings -> LibRaw parameters --------------------------------
// These helpers deal only in plain ints so they stay independent of LibRaw's
// struct layout; load() does the actual assignment.

// LibRaw params.output_color: 0 = raw/camera, 1 = sRGB, 2 = Adobe RGB (1998),
// 3 = Wide Gamut RGB, 4 = ProPhoto RGB, 5 = XYZ, 6 = ACES.
//
// CAVEAT, stated plainly: only sRGB is truly supported end to end today. The
// rest of the pipeline (PixelBuffer, every engine, toSrgbImage) assumes
// Rec.709/sRGB primaries and the QImage we return carries no ICC profile, so
// picking Adobe RGB or ProPhoto here yields wider-gamut numbers that
// downstream code will interpret as if they were sRGB. That is a real,
// visible difference (over-saturated preview). Honouring the setting is
// still better than silently discarding it, but full wide-gamut support
// needs a colour-space tag on PixelBuffer and is follow-up work.
int outputColorFor(const QString& colorSpace)
{
    const QString name = colorSpace.trimmed().toLower();
    if (name == QLatin1String("adobe rgb") || name == QLatin1String("adobergb"))
        return 2;
    if (name == QLatin1String("wide gamut") || name == QLatin1String("widegamut"))
        return 3;
    if (name == QLatin1String("prophoto") || name == QLatin1String("prophoto rgb"))
        return 4;
    return 1;   // sRGB, and the fallback for anything unrecognized.
}

// LibRaw exposes white balance as two independent switches rather than a
// mode enum, so return both.
//
//   As Shot / Camera : use_camera_wb — the multipliers the camera recorded
//                      in the file. This is the sane default.
//   Auto             : use_auto_wb   — LibRaw's grey-world estimate.
//   Daylight         : neither switch. With both off LibRaw falls back to
//                      pre_mul[] from its built-in camera colour table,
//                      which is the daylight-illuminant preset — exactly
//                      what "Daylight" should mean.
struct WhiteBalanceFlags
{
    int useCameraWb = 1;
    int useAutoWb   = 0;
};

WhiteBalanceFlags whiteBalanceFlagsFor(const QString& mode)
{
    const QString name = mode.trimmed().toLower();
    WhiteBalanceFlags flags;
    if (name == QLatin1String("auto")) {
        flags.useCameraWb = 0;
        flags.useAutoWb   = 1;
    } else if (name == QLatin1String("daylight")) {
        flags.useCameraWb = 0;
        flags.useAutoWb   = 0;
    } else {
        // "As Shot", "Camera", and anything unrecognized.
        flags.useCameraWb = 1;
        flags.useAutoWb   = 0;
    }
    return flags;
}

// LibRaw params.highlight: 0 = clip to solid white (default), 1 = leave
// unclipped (blown areas go magenta), 2 = blend clipped and unclipped for a
// gradual fade, 3..9 = reconstruct with increasing aggressiveness.
//
// 2 is the conservative choice for a "highlight recovery" toggle: it
// recovers the detail still present in the channels that did not blow,
// without the colour invention the rebuild modes do.
int highlightModeFor(bool highlightRecovery)
{
    return highlightRecovery ? 2 : 0;
}

#endif // LPS_HAS_LIBRAW

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
#ifndef LPS_HAS_LIBRAW
    Q_UNUSED(path);
    Q_UNUSED(settings);
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

    // ---- Bit depth ----------------------------------------------------------
    // 16 bits per channel. Everything below assumes this succeeded, but we
    // still branch on processed->bits at the end because LibRaw can fall
    // back to 8 for some paths.
    raw.imgdata.params.output_bps = 16;

    // ---- Transfer function — READ THIS BEFORE CHANGING ---------------------
    // We emit sRGB-ENCODED 16-bit data, NOT linear-light 16-bit data.
    //
    // Why it matters: the QImage returned here is handed to
    // PixelBuffer::fromSrgbImage(), which applies the sRGB->linear transfer.
    // Give it linear data and the transfer is applied to values that are
    // already linear, and the picture comes out crushed and far too dark.
    // The alternative — emitting linear and skipping the decode — would mean
    // a second, differently-behaving ingest entry point, i.e. a pipeline API
    // change; not worth it, and RAW is not the only 16-bit source.
    //
    // Why 16-bit is not automatically linear: output_bps only picks the
    // width of the output samples. LibRaw runs every output sample through
    // imgdata.color.curve[], which gamma_curve() builds from params.gamm[]
    // when the memory image is produced; the 8-bit and 16-bit writers differ
    // only in `curve[v] >> 8` versus `curve[v]`. The widely repeated
    // "16-bit output is linear" comes from dcraw's -4 switch, which is
    // documented shorthand for "-6 -W -g 1 1" — it is the explicit "-g 1 1"
    // that makes it linear, not the bit depth.
    //
    // LibRaw's documented meaning for the two fields we set:
    //     gamm[0] — inverted gamma value, i.e. the ENCODING exponent
    //     gamm[1] — slope of the linear segment near black ("toe slope")
    //     neutral (linear) is gamm[0] = 1.0, gamm[1] = 1.0
    // LibRaw's own default is gamm[0] = 1/2.222, gamm[1] = 4.5 — that is
    // BT.709, not sRGB. Leaving it alone would give a subtly wrong shadow
    // region that PixelBuffer's exact IEC 61966-2-1 decode cannot undo.
    //
    // sRGB encode is 1.055 * v^(1/2.4) - 0.055 above the toe and 12.92 * v
    // below it, so the encoding exponent is 1/2.4 and the toe slope is 12.92
    // (equivalently dcraw's "-g 2.4 12.92"; its CLI stores the reciprocal of
    // the first argument into gamm[0]). This is exactly the piecewise
    // function implemented in util/ColorSpace.h, so encode here and decode
    // in PixelBuffer round-trip cleanly.
    raw.imgdata.params.gamm[0] = 1.0 / 2.4;
    raw.imgdata.params.gamm[1] = 12.92;
    //
    // Known, measured residual. LibRaw only lets us specify (exponent, toe
    // slope) and solves the offset and knee itself, and it picks the curve
    // that is both value- AND slope-continuous at the knee: offset 0.0550107,
    // knee 0.0030413. The IEC spec fixes offset 0.055 and knee 0.0031308,
    // which is value-continuous only (its slope jumps 12.92 -> 12.70). So
    // LibRaw's curve and ColorSpace.h's decode are two very slightly
    // different curves. Simulated over all 65536 codes, the worst-case error
    // of the full encode->decode round trip is 3.45e-5 in linear terms —
    // 0.56 of a 14-bit LSB, RMS 1.0e-5, and only 3.4e-6 in the shadows where
    // it would actually be visible. For comparison, leaving LibRaw's BT.709
    // default in place costs 4.7e-2 (1377x worse) and emitting linear 16-bit
    // without telling PixelBuffer costs 2.9e-1 (mid grey 0.5 -> 0.214).
    // Getting rid of the last 3.45e-5 would mean setting gamm to 1/1 and
    // doing the sRGB encode ourselves — a second full-frame pass and another
    // 128 KB table to buy half a 14-bit LSB. Not worth it; documented
    // instead.

    // No auto-brightening: exposure is the user's decision, made downstream
    // on linear float data where it is reversible.
    raw.imgdata.params.no_auto_bright = 1;

    // ---- Settings that used to be silently ignored --------------------------
    raw.imgdata.params.output_color = outputColorFor(settings.colorSpace);
    raw.imgdata.params.highlight    = highlightModeFor(settings.highlightRecovery);

    const WhiteBalanceFlags wb = whiteBalanceFlagsFor(settings.whiteBalanceMode);
    raw.imgdata.params.use_camera_wb = wb.useCameraWb;
    raw.imgdata.params.use_auto_wb   = wb.useAutoWb;

    // LibRaw demosaic algorithms are 0..12 (0 linear, 1 VNG, 2 PPG, 3 AHD,
    // 4 DCB, 11 DHT, 12 AAHD; some require build flags and LibRaw falls back
    // on its own if one is unavailable).
    raw.imgdata.params.user_qual = std::clamp(settings.demosaicQuality, 0, 12);

    // settings.cameraProfile is NOT applied. Honouring it means loading a DCP
    // or ICC profile and replacing LibRaw's camera->working-space matrix with
    // the profile's forward matrix plus its HueSatMap/LookTable — that is a
    // colour-management subsystem, not a parameter. Deliberate future work;
    // it is called out here rather than dropped silently so nobody assumes
    // the field does something.

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
    const bool usableBitmap = processed->type == LIBRAW_IMAGE_BITMAP &&
                              processed->colors >= 3 &&
                              processed->width > 0 &&
                              processed->height > 0;

    // LibRaw's memory image is a tightly packed array of `colors` components
    // per pixel in R, G, B (, G2) order, native endianness, no row padding.
    const int width  = usableBitmap ? static_cast<int>(processed->width)  : 0;
    const int height = usableBitmap ? static_cast<int>(processed->height) : 0;
    const int comps  = usableBitmap ? static_cast<int>(processed->colors) : 0;

    if (usableBitmap && processed->bits == 16) {
        const std::size_t expected = static_cast<std::size_t>(width) *
                                     static_cast<std::size_t>(height) *
                                     static_cast<std::size_t>(comps) * 2u;
        if (processed->data_size < expected) {
            setError(error, QStringLiteral("Truncated RAW output buffer"));
        } else {
            image = QImage(width, height, QImage::Format_RGBX64);
            if (image.isNull()) {
                setError(error, QStringLiteral("Out of memory allocating RAW image"));
            } else {
                const quint16* base = reinterpret_cast<const quint16*>(processed->data);
                for (int y = 0; y < height; ++y) {
                    const quint16* srcRow = base + static_cast<std::size_t>(y) *
                                                   static_cast<std::size_t>(width) *
                                                   static_cast<std::size_t>(comps);
                    // Format_RGBX64 is four native-endian quint16 per pixel
                    // laid out R, G, B, X. QRgba64 encodes exactly that
                    // layout on both endiannesses, so build through it
                    // rather than poking halfwords by index.
                    QRgba64* dstRow = reinterpret_cast<QRgba64*>(image.scanLine(y));
                    for (int x = 0; x < width; ++x) {
                        const quint16* sp = srcRow + static_cast<std::size_t>(x) *
                                                     static_cast<std::size_t>(comps);
                        // RAW has no alpha, and Format_RGBX64 requires the
                        // X halfword to be 0xffff.
                        dstRow[x] = qRgba64(sp[0], sp[1], sp[2],
                                            static_cast<quint16>(0xffff));
                    }
                }
            }
        }
    } else if (usableBitmap && processed->bits == 8) {
        // Fallback. Should not happen with output_bps = 16, but LibRaw can
        // return 8-bit for some builds/paths and a working 8-bit image beats
        // an error dialog.
        const std::size_t expected = static_cast<std::size_t>(width) *
                                     static_cast<std::size_t>(height) *
                                     static_cast<std::size_t>(comps);
        if (processed->data_size < expected) {
            setError(error, QStringLiteral("Truncated RAW output buffer"));
        } else {
            image = QImage(width, height, QImage::Format_RGB888);
            if (image.isNull()) {
                setError(error, QStringLiteral("Out of memory allocating RAW image"));
            } else {
                for (int y = 0; y < height; ++y) {
                    const uchar* src = processed->data +
                                       static_cast<std::size_t>(y) *
                                       static_cast<std::size_t>(width) *
                                       static_cast<std::size_t>(comps);
                    uchar* dst = image.scanLine(y);
                    for (int x = 0; x < width; ++x) {
                        dst[x * 3 + 0] = src[x * comps + 0];
                        dst[x * 3 + 1] = src[x * comps + 1];
                        dst[x * 3 + 2] = src[x * comps + 2];
                    }
                }
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
