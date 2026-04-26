// ==============================================================================
// effects/Grain.cpp
//
// Why perceptual space: physical film grain has roughly constant visual
// amplitude across the tonal range (it's a property of silver halide density,
// which correlates with perception). Adding noise directly to linear values
// gives grain that's invisible in shadows and overwhelming in highlights.
// Adding it in sRGB-encoded space matches photographic expectation.
//
// We round-trip per-pixel: linear -> sRGB -> add noise -> linear. The LUT-
// based sRGB encode/decode keeps this fast.
// ==============================================================================
#include "effects/Grain.h"

#include "util/ColorMath.h"
#include "util/ColorSpace.h"
#include "util/ScanlineParallel.h"

#include <algorithm>

namespace lps {

namespace {

inline float hash01(unsigned int x, unsigned int y)
{
    unsigned int n = x * 0x27d4eb2du ^ (y + 0x165667b1u);
    n ^= n >> 16; n *= 0x7feb352du;
    n ^= n >> 15; n *= 0x846ca68bu;
    n ^= n >> 16;
    return (n & 0xFFFFFF) / 16777215.0f;
}

} // namespace

void Grain::apply(PixelBuffer& buffer, const GrainParams& params)
{
    if (params.isIdentity() || buffer.isNull()) return;

    const float amount = math::clamp01(params.amount / 100.0f);
    // size 0..100 -> 1..~8 pixel block for chunkier grain.
    const int blockSize = std::max(1, static_cast<int>(1.0f + params.size / 14.0f));

    const int width = buffer.width();

    forEachScanline(buffer, [&](float* row, int y) {
        const unsigned int by = static_cast<unsigned int>(y / blockSize);
        for (int x = 0; x < width; ++x) {
            const unsigned int bx = static_cast<unsigned int>(x / blockSize);
            // Noise in [-0.5, +0.5], scaled by amount, capped at ~0.12 sRGB units.
            const float n = (hash01(bx, by) - 0.5f) * amount * 0.25f;

            float* p = row + x * 4;
            for (int ch = 0; ch < 3; ++ch) {
                const float lin = math::clamp01(p[ch]);
                const float srgb = colorspace::linearToSrgb(lin);
                const float noisy = math::clamp01(srgb + n);
                p[ch] = colorspace::srgbToLinear(noisy);
            }
        }
    });
}

} // namespace lps
