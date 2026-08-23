// ==============================================================================
// core/PixelBuffer.cpp
//
// Construction and conversion are parallelized per-scanline. The input QImage
// must outlive the construction call (we read from it), but the resulting
// buffer is independent — it owns its float memory and is safe to mutate.
//
// There are two ingest paths. The 8-bit one is the original and is on the
// hot path for JPEG/PNG8 previews, so it is left exactly as it was. The
// 16-bit one exists so deep-colour sources (RAW, 16-bit TIFF/PNG) reach the
// float buffer without a lossy trip through ARGB32 — a 14-bit sensor
// truncated to 8 bits has the editing latitude of a JPEG, which defeats the
// point of the whole float32 pipeline.
// ==============================================================================
#include "core/PixelBuffer.h"

#include "util/ColorSpace.h"
#include "util/ScanlineParallel.h"

#include <QRgba64>

#include <utility>

namespace lps {

namespace {

// True for the QImage formats that carry 16 bits per colour channel.
//
// Deliberately NOT including the Qt 6.2+ half/full-float formats
// (Format_RGBA16FPx4, Format_RGBA32FPx4, ...): those need a different
// treatment (they are not integer-indexable, and their encoding convention
// is ambiguous), so they keep falling through to the 8-bit path for now.
// Handling them properly is follow-up work.
bool isDeepFormat(QImage::Format format)
{
    switch (format) {
    case QImage::Format_RGBX64:
    case QImage::Format_RGBA64:
    case QImage::Format_RGBA64_Premultiplied:
    case QImage::Format_Grayscale16:
        return true;
    default:
        return false;
    }
}

} // namespace

PixelBuffer PixelBuffer::fromSrgbImage(const QImage& src)
{
    if (src.isNull()) return PixelBuffer();

    return isDeepFormat(src.format()) ? fromSrgb16Image(src)
                                      : fromSrgb8Image(src);
}

// ---- 8-bit ingest (unchanged) -----------------------------------------------
PixelBuffer PixelBuffer::fromSrgb8Image(const QImage& src)
{
    PixelBuffer buf;
    if (src.isNull()) return buf;

    // Normalize input format. ARGB32 guarantees 4 bytes/pixel BGRA (little-
    // endian byte order). One conversion at the boundary — the only format
    // change in the pipeline.
    const QImage normalized = (src.format() == QImage::Format_ARGB32)
                              ? src
                              : src.convertToFormat(QImage::Format_ARGB32);

    buf.m_width  = normalized.width();
    buf.m_height = normalized.height();
    buf.m_pixels.resize(static_cast<size_t>(buf.m_width) * buf.m_height * 4);

    const auto& lut = colorspace::srgb8ToLinearLut();
    const int width = buf.m_width;
    const int height = buf.m_height;

    // Parallelize the conversion.
    QList<int> rows;
    rows.reserve(height);
    for (int y = 0; y < height; ++y) rows.append(y);

    QtConcurrent::blockingMap(rows, [&](int y) {
        const uchar* srcRow = normalized.constScanLine(y);
        float* dstRow = buf.scanline(y);
        for (int x = 0; x < width; ++x) {
            const uchar* sp = srcRow + x * 4;
            float* dp = dstRow + x * 4;
            // ARGB32 on little-endian: [B, G, R, A]
            dp[0] = lut[sp[2]];                 // R (linear)
            dp[1] = lut[sp[1]];                 // G (linear)
            dp[2] = lut[sp[0]];                 // B (linear)
            dp[3] = static_cast<float>(sp[3]) / 255.0f;  // A (linear-space-independent)
        }
    });

    return buf;
}

// ---- 16-bit ingest (deep colour) --------------------------------------------
PixelBuffer PixelBuffer::fromSrgb16Image(const QImage& src)
{
    PixelBuffer buf;
    if (src.isNull()) return buf;

    // Normalize to a straight-alpha 64-bit RGBA layout. Both conversions
    // below stay at 16 bits per channel, so nothing is lost:
    //
    //   Format_Grayscale16          -> RGBA64 (gray replicated into R=G=B)
    //   Format_RGBA64_Premultiplied -> RGBA64 (alpha divided back out; doing
    //                                  the sRGB->linear transfer on
    //                                  premultiplied values would be wrong,
    //                                  the transfer function is non-linear)
    //
    // Format_RGBX64 and Format_RGBA64 are already what we want and are taken
    // as-is — QImage is implicitly shared, so that is a refcount bump, not a
    // copy of 360 MB.
    const QImage::Format format = src.format();
    const QImage normalized = (format == QImage::Format_RGBX64 ||
                               format == QImage::Format_RGBA64)
                              ? src
                              : src.convertToFormat(QImage::Format_RGBA64);
    if (normalized.isNull()) return buf;   // conversion OOM

    buf.m_width  = normalized.width();
    buf.m_height = normalized.height();
    buf.m_pixels.resize(static_cast<size_t>(buf.m_width) * buf.m_height * 4);

    const auto& lut = colorspace::srgb16ToLinearLut();
    const int width = buf.m_width;
    const int height = buf.m_height;

    // Parallelize the conversion. Same per-scanline structure as the 8-bit
    // path — rows are independent, so no synchronization is needed.
    QList<int> rows;
    rows.reserve(height);
    for (int y = 0; y < height; ++y) rows.append(y);

    QtConcurrent::blockingMap(rows, [&](int y) {
        // Channel order: RGBX64/RGBA64 are "halfword-ordered", i.e. four
        // native-endian quint16 laid out [R, G, B, A] at increasing
        // addresses. That is NOT the ARGB32 [B, G, R, A] byte order the
        // 8-bit path above deals with; reading it as BGRA would silently
        // swap red and blue. Rather than depend on that reasoning we go
        // through QRgba64's accessors, whose Red/Green/Blue/AlphaShift enum
        // is flipped per byte order specifically so the component order is
        // R,G,B,A on both big- and little-endian (see Qt's qrgba64.h).
        const QRgba64* srcRow =
            reinterpret_cast<const QRgba64*>(normalized.constScanLine(y));
        float* dstRow = buf.scanline(y);
        for (int x = 0; x < width; ++x) {
            const QRgba64 sp = srcRow[x];
            float* dp = dstRow + x * 4;
            dp[0] = lut[sp.red()];              // R (linear)
            dp[1] = lut[sp.green()];            // G (linear)
            dp[2] = lut[sp.blue()];             // B (linear)
            // A is linear-space-independent. For Format_RGBX64 Qt guarantees
            // the X halfword is 0xffff, so this yields exactly 1.0.
            dp[3] = static_cast<float>(sp.alpha()) / 65535.0f;
        }
    });

    return buf;
}

QImage PixelBuffer::toSrgbImage() const
{
    if (isNull()) return {};

    QImage out(m_width, m_height, QImage::Format_ARGB32);
    const int width = m_width;
    const int height = m_height;

    QList<int> rows;
    rows.reserve(height);
    for (int y = 0; y < height; ++y) rows.append(y);

    QtConcurrent::blockingMap(rows, [&](int y) {
        const float* srcRow = scanline(y);
        uchar* dstRow = out.scanLine(y);
        for (int x = 0; x < width; ++x) {
            const float* sp = srcRow + x * 4;
            uchar* dp = dstRow + x * 4;
            // Linear -> sRGB encoding with saturating LUT.
            dp[2] = colorspace::linearToSrgb8(sp[0]);   // R
            dp[1] = colorspace::linearToSrgb8(sp[1]);   // G
            dp[0] = colorspace::linearToSrgb8(sp[2]);   // B
            // Alpha: saturating byte clamp without gamma conversion.
            const float a = sp[3];
            dp[3] = (a <= 0.0f) ? 0 : (a >= 1.0f) ? 255
                                                 : static_cast<uchar>(a * 255.0f + 0.5f);
        }
    });

    return out;
}

void PixelBuffer::reset(int width, int height, std::vector<float>&& pixels)
{
    const size_t expected = (width > 0 && height > 0)
                          ? static_cast<size_t>(width) * static_cast<size_t>(height) * 4
                          : 0;
    if (expected == 0 || pixels.size() != expected) {
        m_width = 0;
        m_height = 0;
        m_pixels.clear();
        return;
    }

    m_width = width;
    m_height = height;
    m_pixels = std::move(pixels);
}

} // namespace lps
