// ==============================================================================
// core/PixelBuffer.h
// The canonical working representation inside the pipeline.
//
// Layout:  float32 RGBA, linear-light (Rec. 709 primaries), planar-per-pixel
//          [R, G, B, A, R, G, B, A, ...] — same row-major scanline layout
//          as QImage for easy iteration.
//
// Why float:
//   - Adjustments can overshoot [0,1] and recover (e.g. push +1 stop exposure,
//     then pull -1 stop — 8-bit would clip and destroy information).
//   - Linear-light math is mandatory for correct color blending. 8-bit linear
//     is worse than 8-bit sRGB in shadows.
//
// Why one canonical buffer:
//   - Matches the "one deep copy, engines share the buffer" rule.
//   - Avoids repeated conversions between stages.
//   - Engines don't know or care about the source/destination sRGB encoding;
//     conversion happens exactly once at the pipeline boundaries.
//
// Memory:
//   A 24 MP image uses 24M * 4 * 4 bytes = 384 MB of float RGBA. For the
//   preview path (~2 MP) this is ~32 MB — fine. For full-res export it's
//   significant; future optimization can switch export to a streaming
//   scanline-at-a-time mode. Not in this pass.
// ==============================================================================
#pragma once

#include <QImage>

#include <cstddef>
#include <vector>

namespace lps {

class PixelBuffer
{
public:
    PixelBuffer() = default;

    // Construct from an sRGB-encoded QImage. Allocates the float buffer and
    // converts sRGB->linear through an exact, integer-indexed LUT.
    //
    // Two ingest paths, selected by the source format:
    //
    //   16 bits/channel — Format_RGBX64, Format_RGBA64,
    //       Format_RGBA64_Premultiplied, Format_Grayscale16. Read at full
    //       depth through the 65536-entry LUT. No down-conversion. This is
    //       what lets a RAW file keep RAW editing latitude; routing it
    //       through ARGB32 first would throw away 8 bits per channel before
    //       the pipeline ever saw the data.
    //
    //   everything else — normalized to ARGB32 and read through the
    //       256-entry LUT. Unchanged from the original implementation.
    //
    // Either way the result is float32 linear-light RGBA. Note that the
    // *input* is expected to be sRGB-ENCODED at whatever depth it carries;
    // 16-bit does not mean linear here. Producers of 16-bit data (see
    // io/RawImageLoader) must encode to sRGB before handing an image over.
    static PixelBuffer fromSrgbImage(const QImage& src);

    // Convert back to an sRGB-encoded ARGB32 QImage. The internal linear
    // buffer is unchanged — safe to call multiple times.
    QImage toSrgbImage() const;

    // Direct access for engines. Caller must respect width()/height()/stride.
    float*       data()       { return m_pixels.data(); }
    const float* data() const { return m_pixels.data(); }

    int width()  const { return m_width; }
    int height() const { return m_height; }

    void reset(int width, int height, std::vector<float>&& pixels);

    // Scanline accessors. `y` is a row index; returned pointer points to the
    // first float (R) of that row. 4 floats per pixel (RGBA), width() pixels.
    float*       scanline(int y)       { return m_pixels.data() + static_cast<size_t>(y) * m_width * 4; }
    const float* scanline(int y) const { return m_pixels.data() + static_cast<size_t>(y) * m_width * 4; }

    bool isNull() const { return m_pixels.empty(); }

private:
    // The two ingest paths behind fromSrgbImage(). Split into separate
    // functions deliberately: the 8-bit path stays byte-for-byte the loop it
    // always was, with no added per-pixel branch and no added format test
    // inside the parallel region.
    static PixelBuffer fromSrgb8Image(const QImage& src);
    static PixelBuffer fromSrgb16Image(const QImage& src);

    std::vector<float> m_pixels;   // size = width * height * 4
    int m_width  = 0;
    int m_height = 0;
};

} // namespace lps
