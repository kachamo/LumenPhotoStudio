// ==============================================================================
// grading/ColorGrading.cpp
//
// Trilinear interpolation into a 3D LUT. LUTs are authored in sRGB, so we
// convert linear<->sRGB at the boundary.
//
// Caching: LUTData is large (a 33^3 LUT is ~140KB; a 65^3 LUT is ~1MB).
// We store parsed LUTs in a QHash keyed by file path, protected by a mutex
// for thread safety. Cache is not size-limited in V1 — users typically
// cycle through <10 LUTs per session, so memory is bounded in practice.
// ==============================================================================
#include "grading/ColorGrading.h"

#include "grading/FilmProfiles.h"
#include "grading/LUTLoader.h"
#include "util/ColorMath.h"
#include "util/ColorSpace.h"
#include "util/ScanlineParallel.h"

#include <QHash>
#include <QMutex>
#include <QVector3D>

#include <algorithm>
#include <cmath>

namespace lps {

namespace {

QHash<QString, LUTData>& lutCache()
{
    static QHash<QString, LUTData> cache;
    return cache;
}
QMutex& lutCacheMutex()
{
    static QMutex m;
    return m;
}

const LUTData* fetchLut(const QString& path)
{
    if (path.isEmpty()) return nullptr;
    QMutexLocker lock(&lutCacheMutex());
    auto& cache = lutCache();
    auto it = cache.find(path);
    if (it != cache.end()) return &it.value();

    LUTLoadResult r = LUTLoader::loadCube(path);
    if (!r.ok) return nullptr;

    auto inserted = cache.insert(path, std::move(r.lut));
    return &inserted.value();
}

// Trilinear sample. Inputs r/g/b in [0,1].
QVector3D sample3D(const LUTData& lut, float r, float g, float b)
{
    const int N = lut.size;
    const float scale = static_cast<float>(N - 1);
    const float rf = r * scale;
    const float gf = g * scale;
    const float bf = b * scale;

    const int r0 = std::clamp(static_cast<int>(std::floor(rf)), 0, N - 1);
    const int g0 = std::clamp(static_cast<int>(std::floor(gf)), 0, N - 1);
    const int b0 = std::clamp(static_cast<int>(std::floor(bf)), 0, N - 1);
    const int r1 = std::min(r0 + 1, N - 1);
    const int g1 = std::min(g0 + 1, N - 1);
    const int b1 = std::min(b0 + 1, N - 1);

    const float tr = rf - r0;
    const float tg = gf - g0;
    const float tb = bf - b0;

    auto at = [&](int ri, int gi, int bi) -> const QVector3D& {
        // .cube convention: red varies fastest, then green, then blue.
        return lut.entries[static_cast<size_t>(ri + gi * N + bi * N * N)];
    };

    const QVector3D c000 = at(r0, g0, b0);
    const QVector3D c100 = at(r1, g0, b0);
    const QVector3D c010 = at(r0, g1, b0);
    const QVector3D c110 = at(r1, g1, b0);
    const QVector3D c001 = at(r0, g0, b1);
    const QVector3D c101 = at(r1, g0, b1);
    const QVector3D c011 = at(r0, g1, b1);
    const QVector3D c111 = at(r1, g1, b1);

    const QVector3D c00 = c000 * (1.0f - tr) + c100 * tr;
    const QVector3D c10 = c010 * (1.0f - tr) + c110 * tr;
    const QVector3D c01 = c001 * (1.0f - tr) + c101 * tr;
    const QVector3D c11 = c011 * (1.0f - tr) + c111 * tr;
    const QVector3D c0 = c00 * (1.0f - tg) + c10 * tg;
    const QVector3D c1 = c01 * (1.0f - tg) + c11 * tg;
    return c0 * (1.0f - tb) + c1 * tb;
}

void applyLut(PixelBuffer& buffer, const LUTData& lut, float opacity)
{
    const float op = math::clamp01(opacity);
    if (op < 1e-3f || buffer.isNull()) return;

    const int width = buffer.width();

    if (lut.is3D) {
        forEachScanline(buffer, [&](float* row, int /*y*/) {
            for (int x = 0; x < width; ++x) {
                float* p = row + x * 4;
                // linear -> sRGB for lookup
                const float sr = colorspace::linearToSrgb(math::clamp01(p[0]));
                const float sg = colorspace::linearToSrgb(math::clamp01(p[1]));
                const float sb = colorspace::linearToSrgb(math::clamp01(p[2]));

                const QVector3D out = sample3D(lut, sr, sg, sb);

                // blend in sRGB, then convert back
                const float rOut = sr * (1.0f - op) + out.x() * op;
                const float gOut = sg * (1.0f - op) + out.y() * op;
                const float bOut = sb * (1.0f - op) + out.z() * op;

                p[0] = colorspace::srgbToLinear(math::clamp01(rOut));
                p[1] = colorspace::srgbToLinear(math::clamp01(gOut));
                p[2] = colorspace::srgbToLinear(math::clamp01(bOut));
            }
        });
    } else {
        // 1D LUT — sample per channel in sRGB space.
        forEachScanline(buffer, [&](float* row, int /*y*/) {
            for (int x = 0; x < width; ++x) {
                float* p = row + x * 4;
                for (int ch = 0; ch < 3; ++ch) {
                    const float lin = math::clamp01(p[ch]);
                    const float srgbIn = colorspace::linearToSrgb(lin);
                    const float idx = srgbIn * static_cast<float>(lut.size - 1);
                    const int i0 = std::clamp(static_cast<int>(std::floor(idx)), 0, lut.size - 1);
                    const int i1 = std::min(i0 + 1, lut.size - 1);
                    const float t = idx - i0;
                    // .cube 1D layout: each row is RGB but we only use the matching channel.
                    const float a = (ch == 0 ? lut.entries[i0].x()
                                   : ch == 1 ? lut.entries[i0].y()
                                             : lut.entries[i0].z());
                    const float b = (ch == 0 ? lut.entries[i1].x()
                                   : ch == 1 ? lut.entries[i1].y()
                                             : lut.entries[i1].z());
                    const float lutV = a * (1.0f - t) + b * t;
                    const float srgbOut = srgbIn * (1.0f - op) + lutV * op;
                    p[ch] = colorspace::srgbToLinear(math::clamp01(srgbOut));
                }
            }
        });
    }
}

// ============================================================================
// 3-way color grading (shadows / midtones / highlights / global)
//
// Classic film/cinema convention: split the tonal range into three regions
// via smooth weight functions of luminance, apply a per-region color tint,
// sum the contributions plus a global tint.
//
// Per pixel:
//   1. Compute Rec.709 linear luminance Y.
//   2. Evaluate three weights wS(Y), wM(Y), wH(Y) (≈ summing to 1) via
//      smoothstep transitions around two pivots derived from balance.
//   3. Sum tints: total = wS·tS + wM·tM + wH·tH + tGlobal.
//   4. out = max(in + total, 0). No upper clamp — we're in linear-light
//      and supra-1 values are fine until the final sRGB encode.
//
// Tints are precomputed once from (hue, saturation, strength) into linear
// RGB offset vectors, so the per-pixel loop has only mults + adds plus
// the luminance and smoothstep work. No QColor allocation, no HSV per
// pixel — matches the engine's per-pixel performance discipline.
//
// Identity short-circuit happens at the top of ColorGrading::apply() via
// GradingParams::isIdentity() — if all four wheels are at zero saturation
// or zero strength, we never enter this code path.
// ============================================================================
struct Tint { float r, g, b; };

inline Tint computeTint(float hueDeg, float saturationPct, float strengthPct)
{
    // Either zero kills the wheel — no tint, no work.
    if (saturationPct < 1e-4f || strengthPct < 1e-4f)
        return { 0.0f, 0.0f, 0.0f };

    // HSL→RGB at L=0.5 produces a pure hue at mid-luminance. Subtracting
    // 0.5 per channel re-centers on gray, giving an additive offset where
    // saturation=1 produces vectors of magnitude up to 0.5. The *2.0 is
    // chosen so saturation=1, strength=1 reaches a full ±0.5 channel
    // offset (visibly strong at 100/100, intentionally so).
    float r, g, b;
    math::hslToRgb(hueDeg / 360.0f,
                   saturationPct / 100.0f,
                   /*lightness=*/0.5f,
                   r, g, b);
    const float scale = (strengthPct / 100.0f) * 2.0f;
    return { (r - 0.5f) * scale,
             (g - 0.5f) * scale,
             (b - 0.5f) * scale };
}

void applyThreeWayGrading(PixelBuffer& buffer, const GradingParams& params)
{
    // Precompute all four tints once before the per-pixel loop.
    const Tint tShadows    = computeTint(params.shadowsHue,
                                          params.shadowsSaturation,
                                          params.shadowsStrength);
    const Tint tMidtones   = computeTint(params.midtonesHue,
                                          params.midtonesSaturation,
                                          params.midtonesStrength);
    const Tint tHighlights = computeTint(params.highlightsHue,
                                          params.highlightsSaturation,
                                          params.highlightsStrength);
    const Tint tGlobal     = computeTint(params.globalHue,
                                          params.globalSaturation,
                                          params.globalStrength);

    // Smoothstep edges. balance ∈ [-100, +100] shifts both pivots together
    // by up to ±0.15 in luminance space (negative pulls them down, more
    // pixels count as midtones/highlights; positive pushes up). blending
    // ∈ [0, 100] grows the half-width from 0.05 (near-hard) to 0.30
    // (very soft, regions overlap heavily).
    const float balanceShift = (params.balance / 100.0f) * 0.15f;
    const float pivotLo = 0.30f + balanceShift;
    const float pivotHi = 0.70f + balanceShift;
    const float halfW = 0.05f + (params.blending / 100.0f) * 0.25f;

    const float sLo0 = pivotLo - halfW;
    const float sLo1 = pivotLo + halfW;
    const float sHi0 = pivotHi - halfW;
    const float sHi1 = pivotHi + halfW;

    ScanlineParallel::run(buffer.height(),
        [&buffer, tShadows, tMidtones, tHighlights, tGlobal,
         sLo0, sLo1, sHi0, sHi1](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* row = buffer.row(y);
            const int W = buffer.width();
            for (int x = 0; x < W; ++x) {
                float* p = row + x * 4;

                // Luminance — Rec.709 linear, same convention as the rest
                // of the engine (we're in linear space at this stage).
                const float Y = math::luminance(p[0], p[1], p[2]);

                // Region weights. Sum is approximately 1; clamping the
                // midtone weight to non-negative prevents tiny FP errors
                // in mid-overlap from producing negative contributions.
                const float wHighlights = math::smoothstep(sHi0, sHi1, Y);
                const float wShadows    = 1.0f - math::smoothstep(sLo0, sLo1, Y);
                float wMidtones = 1.0f - wHighlights - wShadows;
                if (wMidtones < 0.0f) wMidtones = 0.0f;

                // Sum tinted offsets per channel. Global tint applies
                // uniformly (no luminance weighting) — matches the spec's
                // "Global affects the whole image."
                const float dr = wShadows    * tShadows.r
                               + wMidtones   * tMidtones.r
                               + wHighlights * tHighlights.r
                               + tGlobal.r;
                const float dg = wShadows    * tShadows.g
                               + wMidtones   * tMidtones.g
                               + wHighlights * tHighlights.g
                               + tGlobal.g;
                const float db = wShadows    * tShadows.b
                               + wMidtones   * tMidtones.b
                               + wHighlights * tHighlights.b
                               + tGlobal.b;

                // Additive offset, floor-clamp at 0 (no upper clamp —
                // we're in linear-light). The `!(x > 0)` form catches
                // both negatives and NaN, since NaN compares false to
                // anything.
                float r = p[0] + dr;
                float g = p[1] + dg;
                float b = p[2] + db;
                if (!(r > 0.0f)) r = 0.0f;
                if (!(g > 0.0f)) g = 0.0f;
                if (!(b > 0.0f)) b = 0.0f;
                p[0] = r;
                p[1] = g;
                p[2] = b;
                // alpha p[3] untouched
            }
        }
    });
}

} // namespace

void ColorGrading::apply(PixelBuffer& buffer, const GradingParams& params)
{
    if (params.isIdentity() || buffer.isNull()) return;

    // Film profile first (base look), then user LUT on top.
    if (!params.filmProfileId.isEmpty() && params.filmProfileOpacity > 1e-3f) {
        const QString path = FilmProfiles::resolveLutPath(params.filmProfileId);
        if (!path.isEmpty()) {
            if (const LUTData* lut = fetchLut(path))
                applyLut(buffer, *lut, params.filmProfileOpacity);
        }
    }

    if (!params.lutPath.isEmpty() && params.lutOpacity > 1e-3f) {
        if (const LUTData* lut = fetchLut(params.lutPath))
            applyLut(buffer, *lut, params.lutOpacity);
    }

    // 3-way color grading runs AFTER LUT processing per spec — the wheels
    // operate on the LUT-transformed pixels, not the pre-LUT linear ones.
    // Per-wheel activity check inside applyThreeWayGrading is implicit:
    // if all tints come out zero, the per-pixel offsets are zero. But
    // we guard at the call site too, to skip the entire pass when there
    // are no active wheels. (isIdentity() may have returned false because
    // a LUT is loaded, even if all wheels are at zero — so we still need
    // a separate check here.)
    auto wheelActive = [](float sat, float str) {
        return sat > 1e-4f && str > 1e-4f;
    };
    const bool anyWheel =
        wheelActive(params.shadowsSaturation,    params.shadowsStrength)    ||
        wheelActive(params.midtonesSaturation,   params.midtonesStrength)   ||
        wheelActive(params.highlightsSaturation, params.highlightsStrength) ||
        wheelActive(params.globalSaturation,     params.globalStrength);
    if (anyWheel) applyThreeWayGrading(buffer, params);
}

} // namespace lps
