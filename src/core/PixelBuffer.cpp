// ==============================================================================
// core/PixelBuffer.cpp
//
// Construction and conversion are parallelized per-scanline. The input QImage
// must outlive the construction call (we read from it), but the resulting
// buffer is independent — it owns its float memory and is safe to mutate.
// ==============================================================================
#include "core/PixelBuffer.h"

#include "util/ColorSpace.h"
#include "util/ScanlineParallel.h"

namespace lps {

PixelBuffer PixelBuffer::fromSrgbImage(const QImage& src)
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

} // namespace lps
