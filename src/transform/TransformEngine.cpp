// ==============================================================================
// transform/TransformEngine.cpp
// ==============================================================================
#include "transform/TransformEngine.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace lps {

namespace {

inline size_t pxIndex(int x, int y, int width)
{
    return (static_cast<size_t>(y) * static_cast<size_t>(width)
            + static_cast<size_t>(x)) * 4;
}

std::vector<float> copyPixels(const PixelBuffer& buffer)
{
    const size_t count = static_cast<size_t>(buffer.width())
                       * static_cast<size_t>(buffer.height()) * 4;
    return std::vector<float>(buffer.data(), buffer.data() + count);
}

void sampleBilinear(const std::vector<float>& src,
                    int width, int height,
                    float x, float y,
                    float* out)
{
    if (x < 0.0f || y < 0.0f ||
        x > static_cast<float>(width - 1) ||
        y > static_cast<float>(height - 1)) {
        out[0] = 0.0f;
        out[1] = 0.0f;
        out[2] = 0.0f;
        out[3] = 1.0f;
        return;
    }

    const int x0 = std::clamp(static_cast<int>(std::floor(x)), 0, width - 1);
    const int y0 = std::clamp(static_cast<int>(std::floor(y)), 0, height - 1);
    const int x1 = std::min(x0 + 1, width - 1);
    const int y1 = std::min(y0 + 1, height - 1);
    const float tx = x - static_cast<float>(x0);
    const float ty = y - static_cast<float>(y0);

    const size_t i00 = pxIndex(x0, y0, width);
    const size_t i10 = pxIndex(x1, y0, width);
    const size_t i01 = pxIndex(x0, y1, width);
    const size_t i11 = pxIndex(x1, y1, width);

    for (int c = 0; c < 4; ++c) {
        const float a = src[i00 + c] * (1.0f - tx) + src[i10 + c] * tx;
        const float b = src[i01 + c] * (1.0f - tx) + src[i11 + c] * tx;
        out[c] = a * (1.0f - ty) + b * ty;
    }
}

bool cropIsIdentity(const QRectF& crop)
{
    const QRectF r = crop.normalized();
    return std::fabs(r.x()) < 1e-4
        && std::fabs(r.y()) < 1e-4
        && std::fabs(r.width()  - 1.0) < 1e-4
        && std::fabs(r.height() - 1.0) < 1e-4;
}

void applyCrop(std::vector<float>& pixels, int& width, int& height,
               const QRectF& crop)
{
    if (cropIsIdentity(crop)) return;

    const QRectF r = crop.normalized();
    const int outW = std::max(1, static_cast<int>(std::lround(r.width() * width)));
    const int outH = std::max(1, static_cast<int>(std::lround(r.height() * height)));
    std::vector<float> out(static_cast<size_t>(outW) * outH * 4);

    const float srcX = static_cast<float>(r.x() * width);
    const float srcY = static_cast<float>(r.y() * height);
    const float srcW = static_cast<float>(r.width() * width);
    const float srcH = static_cast<float>(r.height() * height);

    for (int y = 0; y < outH; ++y) {
        for (int x = 0; x < outW; ++x) {
            const float sx = srcX + (static_cast<float>(x) + 0.5f)
                                   * srcW / static_cast<float>(outW) - 0.5f;
            const float sy = srcY + (static_cast<float>(y) + 0.5f)
                                   * srcH / static_cast<float>(outH) - 0.5f;
            sampleBilinear(pixels, width, height, sx, sy,
                           out.data() + pxIndex(x, y, outW));
        }
    }

    pixels = std::move(out);
    width = outW;
    height = outH;
}

void applyFlip(std::vector<float>& pixels, int width, int height,
               bool horizontal, bool vertical)
{
    if (!horizontal && !vertical) return;

    std::vector<float> out(pixels.size());
    for (int y = 0; y < height; ++y) {
        const int sy = vertical ? (height - 1 - y) : y;
        for (int x = 0; x < width; ++x) {
            const int sx = horizontal ? (width - 1 - x) : x;
            const size_t dst = pxIndex(x, y, width);
            const size_t src = pxIndex(sx, sy, width);
            out[dst + 0] = pixels[src + 0];
            out[dst + 1] = pixels[src + 1];
            out[dst + 2] = pixels[src + 2];
            out[dst + 3] = pixels[src + 3];
        }
    }
    pixels = std::move(out);
}

void applyRotation(std::vector<float>& pixels, int& width, int& height,
                   float degrees)
{
    if (std::fabs(degrees) < 1e-4f) return;

    constexpr float kPi = 3.14159265358979323846f;
    const float radians = degrees * kPi / 180.0f;
    float c = std::cos(radians);
    float s = std::sin(radians);
    if (std::fabs(c) < 1e-6f) c = 0.0f;
    if (std::fabs(s) < 1e-6f) s = 0.0f;

    const int outW = std::max(1, static_cast<int>(
        std::ceil(std::fabs(c) * width + std::fabs(s) * height)));
    const int outH = std::max(1, static_cast<int>(
        std::ceil(std::fabs(s) * width + std::fabs(c) * height)));

    std::vector<float> out(static_cast<size_t>(outW) * outH * 4);
    const float srcCx = (static_cast<float>(width)  - 1.0f) * 0.5f;
    const float srcCy = (static_cast<float>(height) - 1.0f) * 0.5f;
    const float dstCx = (static_cast<float>(outW) - 1.0f) * 0.5f;
    const float dstCy = (static_cast<float>(outH) - 1.0f) * 0.5f;

    for (int y = 0; y < outH; ++y) {
        for (int x = 0; x < outW; ++x) {
            const float dx = static_cast<float>(x) - dstCx;
            const float dy = static_cast<float>(y) - dstCy;
            const float sx =  c * dx + s * dy + srcCx;
            const float sy = -s * dx + c * dy + srcCy;
            sampleBilinear(pixels, width, height, sx, sy,
                           out.data() + pxIndex(x, y, outW));
        }
    }

    pixels = std::move(out);
    width = outW;
    height = outH;
}

} // namespace

void TransformEngine::apply(PixelBuffer& buffer, const TransformParams& params)
{
    if (params.isIdentity() || buffer.isNull()) return;

    int width = buffer.width();
    int height = buffer.height();
    std::vector<float> pixels = copyPixels(buffer);

    applyCrop(pixels, width, height, params.cropRect);
    applyFlip(pixels, width, height, params.flipHorizontal, params.flipVertical);
    applyRotation(pixels, width, height,
                  params.rotationDegrees + params.straightenAngle);

    buffer.reset(width, height, std::move(pixels));
}

} // namespace lps
