// ==============================================================================
// details/DetailsEngine.cpp
// ==============================================================================
#include "details/DetailsEngine.h"

#include "util/ColorMath.h"

#include <algorithm>
#include <cmath>
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

std::vector<float> blurRgb(const std::vector<float>& src,
                           int width, int height, float radius)
{
    const int r = std::clamp(static_cast<int>(std::ceil(radius)), 1, 3);
    std::vector<float> tmp(src.size());
    std::vector<float> out(src.size());

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t dst = pxIndex(x, y, width);
            int weightSum = 0;
            float sum[3] = { 0.0f, 0.0f, 0.0f };
            for (int dx = -r; dx <= r; ++dx) {
                const int sx = std::clamp(x + dx, 0, width - 1);
                const int w = r + 1 - std::abs(dx);
                const size_t si = pxIndex(sx, y, width);
                sum[0] += src[si + 0] * static_cast<float>(w);
                sum[1] += src[si + 1] * static_cast<float>(w);
                sum[2] += src[si + 2] * static_cast<float>(w);
                weightSum += w;
            }
            const float inv = 1.0f / static_cast<float>(weightSum);
            tmp[dst + 0] = sum[0] * inv;
            tmp[dst + 1] = sum[1] * inv;
            tmp[dst + 2] = sum[2] * inv;
            tmp[dst + 3] = src[dst + 3];
        }
    }

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t dst = pxIndex(x, y, width);
            int weightSum = 0;
            float sum[3] = { 0.0f, 0.0f, 0.0f };
            for (int dy = -r; dy <= r; ++dy) {
                const int sy = std::clamp(y + dy, 0, height - 1);
                const int w = r + 1 - std::abs(dy);
                const size_t si = pxIndex(x, sy, width);
                sum[0] += tmp[si + 0] * static_cast<float>(w);
                sum[1] += tmp[si + 1] * static_cast<float>(w);
                sum[2] += tmp[si + 2] * static_cast<float>(w);
                weightSum += w;
            }
            const float inv = 1.0f / static_cast<float>(weightSum);
            out[dst + 0] = sum[0] * inv;
            out[dst + 1] = sum[1] * inv;
            out[dst + 2] = sum[2] * inv;
            out[dst + 3] = src[dst + 3];
        }
    }

    return out;
}

inline float nonNegative(float v)
{
    return (v > 0.0f) ? v : 0.0f;
}

void applyLuminanceNoiseReduction(PixelBuffer& buffer,
                                  const DetailsParams& params)
{
    if (params.luminanceNR <= 1e-4f) return;

    const int width = buffer.width();
    const int height = buffer.height();
    std::vector<float> src = copyPixels(buffer);
    const float strength = params.luminanceNR / 100.0f;
    const float detail = params.luminanceDetail / 100.0f;
    const float blend = strength * (1.0f - 0.65f * detail);
    const std::vector<float> blurred = blurRgb(src, width, height,
                                               1.0f + strength * 2.0f);

    for (int y = 0; y < height; ++y) {
        float* row = buffer.scanline(y);
        for (int x = 0; x < width; ++x) {
            const size_t i = pxIndex(x, y, width);
            float* px = row + x * 4;

            const float y0 = math::luminance(src[i + 0], src[i + 1], src[i + 2]);
            const float yb = math::luminance(blurred[i + 0],
                                             blurred[i + 1],
                                             blurred[i + 2]);
            const float yt = math::lerp(y0, yb, blend);
            if (y0 > 1e-6f) {
                const float scale = yt / y0;
                px[0] = nonNegative(src[i + 0] * scale);
                px[1] = nonNegative(src[i + 1] * scale);
                px[2] = nonNegative(src[i + 2] * scale);
            } else {
                px[0] = nonNegative(math::lerp(src[i + 0], blurred[i + 0], blend));
                px[1] = nonNegative(math::lerp(src[i + 1], blurred[i + 1], blend));
                px[2] = nonNegative(math::lerp(src[i + 2], blurred[i + 2], blend));
            }
        }
    }
}

void applyColorNoiseReduction(PixelBuffer& buffer,
                              const DetailsParams& params)
{
    if (params.colorNR <= 1e-4f) return;

    const int width = buffer.width();
    const int height = buffer.height();
    std::vector<float> src = copyPixels(buffer);
    const float strength = params.colorNR / 100.0f;
    const float detail = params.colorDetail / 100.0f;
    const float blend = strength * (1.0f - 0.65f * detail);
    const std::vector<float> blurred = blurRgb(src, width, height,
                                               1.0f + strength);

    for (int y = 0; y < height; ++y) {
        float* row = buffer.scanline(y);
        for (int x = 0; x < width; ++x) {
            const size_t i = pxIndex(x, y, width);
            float* px = row + x * 4;

            const float y0 = math::luminance(src[i + 0], src[i + 1], src[i + 2]);
            const float yb = math::luminance(blurred[i + 0],
                                             blurred[i + 1],
                                             blurred[i + 2]);
            for (int c = 0; c < 3; ++c) {
                const float chroma = src[i + c] - y0;
                const float blurredChroma = blurred[i + c] - yb;
                px[c] = nonNegative(y0 + math::lerp(chroma, blurredChroma, blend));
            }
        }
    }
}

void applySharpening(PixelBuffer& buffer, const DetailsParams& params)
{
    if (params.sharpeningAmount <= 1e-4f) return;

    const int width = buffer.width();
    const int height = buffer.height();
    std::vector<float> src = copyPixels(buffer);
    const std::vector<float> blurred = blurRgb(src, width, height,
                                               params.sharpeningRadius);

    const float amount = params.sharpeningAmount / 100.0f;
    const float detailGain = 0.35f + 0.65f * (params.sharpeningDetail / 100.0f);
    const float strength = amount * detailGain;
    const float masking = params.sharpeningMasking / 100.0f;

    for (int y = 0; y < height; ++y) {
        float* row = buffer.scanline(y);
        for (int x = 0; x < width; ++x) {
            const size_t i = pxIndex(x, y, width);
            float* px = row + x * 4;

            const float y0 = math::luminance(src[i + 0], src[i + 1], src[i + 2]);
            const float yb = math::luminance(blurred[i + 0],
                                             blurred[i + 1],
                                             blurred[i + 2]);
            const float edge = std::fabs(y0 - yb);
            float edgeMask = 1.0f;
            if (masking > 1e-4f) {
                const float threshold = masking * 0.08f;
                edgeMask = math::smoothstep(threshold, threshold + 0.04f, edge);
            }

            const float k = strength * edgeMask;
            px[0] = nonNegative(src[i + 0] + (src[i + 0] - blurred[i + 0]) * k);
            px[1] = nonNegative(src[i + 1] + (src[i + 1] - blurred[i + 1]) * k);
            px[2] = nonNegative(src[i + 2] + (src[i + 2] - blurred[i + 2]) * k);
        }
    }
}

} // namespace

void DetailsEngine::apply(PixelBuffer& buffer, const DetailsParams& params)
{
    if (params.isIdentity() || buffer.isNull()) return;

    applyLuminanceNoiseReduction(buffer, params);
    applyColorNoiseReduction(buffer, params);
    applySharpening(buffer, params);
}

} // namespace lps
