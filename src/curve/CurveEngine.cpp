// ==============================================================================
// curve/CurveEngine.cpp
//
// The user-facing curve editor operates in perceptual (sRGB-encoded) space.
// So: for each channel, convert linear -> sRGB, sample curve, convert back.
// The LUT is therefore a map sRGB input -> sRGB output; we linearize the
// result before writing to the float buffer.
//
// We still fuse master + per-channel into a single sRGB->sRGB LUT per channel,
// so applying all four curves is one pass with one lookup per channel.
// ==============================================================================
#include "curve/CurveEngine.h"

#include "curve/ToneCurve.h"
#include "util/ColorMath.h"
#include "util/ColorSpace.h"
#include "util/ScanlineParallel.h"

namespace lps {

namespace {

void fillIdentity(ToneCurve::FloatLut& lut)
{
    for (int i = 0; i < ToneCurve::kLutSize; ++i)
        lut[static_cast<size_t>(i)] = static_cast<float>(i) / (ToneCurve::kLutSize - 1);
}

// Compose LUTs: out[i] = second[first[i] * scale]
void compose(const ToneCurve::FloatLut& first,
             const ToneCurve::FloatLut& second,
             ToneCurve::FloatLut&       out)
{
    for (int i = 0; i < ToneCurve::kLutSize; ++i) {
        const float v1 = first[static_cast<size_t>(i)];
        // Map v1 (possibly outside [0,1] after extrapolation) into LUT index.
        const float idx = math::clamp01(v1) * (ToneCurve::kLutSize - 1);
        const int   i0  = static_cast<int>(idx);
        const int   i1  = (i0 + 1 < ToneCurve::kLutSize) ? i0 + 1 : i0;
        const float t   = idx - static_cast<float>(i0);
        out[static_cast<size_t>(i)] =
            second[static_cast<size_t>(i0)] * (1.0f - t)
          + second[static_cast<size_t>(i1)] * t;
    }
}

// Sample a perceptual-space LUT for a linear input.
// In:  v is linear-light float (can exceed [0,1]).
// Out: linear-light float (can exceed [0,1] if curve maps > 1).
inline float sampleThroughSrgb(const ToneCurve::FloatLut& lut, float v)
{
    // Linearize out-of-range handling: values > 1 pass through the curve via
    // perceptual encoding, which saturates at 1 for the LUT — then we take
    // whatever the LUT says and re-linearize.
    const float srgbIn = colorspace::linearToSrgb(math::clamp01(v));
    const float idx    = srgbIn * (ToneCurve::kLutSize - 1);
    const int   i0     = static_cast<int>(idx);
    const int   i1     = (i0 + 1 < ToneCurve::kLutSize) ? i0 + 1 : i0;
    const float t      = idx - static_cast<float>(i0);
    const float srgbOut = lut[static_cast<size_t>(i0)] * (1.0f - t)
                        + lut[static_cast<size_t>(i1)] * t;
    return colorspace::srgbToLinear(math::clamp01(srgbOut));
}

} // namespace

void CurveEngine::apply(PixelBuffer& buffer, const CurveParams& params)
{
    if (params.isIdentity() || buffer.isNull()) return;

    ToneCurve::FloatLut masterLut{};
    const bool masterActive = ToneCurve::buildLut(params.master, masterLut);

    auto buildFused = [&](const CurvePoints& channel, ToneCurve::FloatLut& out) {
        ToneCurve::FloatLut channelLut{};
        const bool channelActive = ToneCurve::buildLut(channel, channelLut);
        if (masterActive && channelActive) {
            compose(masterLut, channelLut, out);
        } else if (masterActive) {
            out = masterLut;
        } else if (channelActive) {
            out = channelLut;
        } else {
            fillIdentity(out);
        }
    };

    ToneCurve::FloatLut lutR{}, lutG{}, lutB{};
    buildFused(params.red,   lutR);
    buildFused(params.green, lutG);
    buildFused(params.blue,  lutB);

    const int width = buffer.width();
    forEachScanline(buffer, [&](float* row, int /*y*/) {
        for (int x = 0; x < width; ++x) {
            float* p = row + x * 4;
            p[0] = sampleThroughSrgb(lutR, p[0]);
            p[1] = sampleThroughSrgb(lutG, p[1]);
            p[2] = sampleThroughSrgb(lutB, p[2]);
        }
    });
}

} // namespace lps
