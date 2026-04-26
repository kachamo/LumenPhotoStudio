// ==============================================================================
// util/ScanlineParallel.h
// Parallelize per-scanline operations. Two overloads:
//
//   forEachScanline(PixelBuffer&, op)   — for the main pipeline working buffer
//   forEachScanline(QImage&, op)        — for boundary conversions / histogram
//
// Each row is independent, so writes don't race. Below a pixel-count
// threshold we run single-threaded (dispatch cost exceeds the win).
// ==============================================================================
#pragma once

#include "core/PixelBuffer.h"

#include <QImage>
#include <QtConcurrent>

#include <functional>

namespace lps {

namespace detail {
// Dispatch cost break-even point; tuned on typical desktop CPUs.
inline constexpr int kParallelThreshold = 200'000;
} // namespace detail

// ---- PixelBuffer overload (primary — used by every engine) ------------------
// `op` receives a pointer to the first float of row y, and y. Each row has
// buffer.width() pixels, 4 floats per pixel (RGBA).
inline void forEachScanline(PixelBuffer& buffer,
                            const std::function<void(float* /*row*/, int /*y*/)>& op)
{
    const int height = buffer.height();
    const int width  = buffer.width();
    if (height <= 0 || width <= 0) return;

    if (static_cast<qint64>(width) * height < detail::kParallelThreshold) {
        for (int y = 0; y < height; ++y)
            op(buffer.scanline(y), y);
        return;
    }

    QList<int> rows;
    rows.reserve(height);
    for (int y = 0; y < height; ++y) rows.append(y);

    QtConcurrent::blockingMap(rows, [&](int y) {
        op(buffer.scanline(y), y);
    });
}

// ---- QImage overload (for boundary converters and debug) --------------------
inline void forEachScanline(QImage& image,
                            const std::function<void(uchar* /*row*/, int /*y*/)>& op)
{
    const int height = image.height();
    const int width  = image.width();
    if (height <= 0 || width <= 0) return;

    if (static_cast<qint64>(width) * height < detail::kParallelThreshold) {
        for (int y = 0; y < height; ++y)
            op(image.scanLine(y), y);
        return;
    }

    QList<int> rows;
    rows.reserve(height);
    for (int y = 0; y < height; ++y) rows.append(y);

    QtConcurrent::blockingMap(rows, [&](int y) {
        op(image.scanLine(y), y);
    });
}

} // namespace lps
