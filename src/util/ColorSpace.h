// ==============================================================================
// util/ColorSpace.h
// sRGB <-> linear-light conversions. Header-only, inline.
//
// Uses the real piecewise IEC 61966-2-1 sRGB transfer function, NOT the
// pow(2.2) approximation:
//
//   sRGB -> Linear:   v' = v/12.92                         if v <= 0.04045
//                     v' = ((v + 0.055) / 1.055)^2.4       otherwise
//
//   Linear -> sRGB:   v' = v * 12.92                       if v <= 0.0031308
//                     v' = 1.055 * v^(1/2.4) - 0.055       otherwise
//
// pow(2.2) is wrong by up to ~5 counts in the shadow region — that's enough
// to cause visible banding and incorrect tonal rendering. The piecewise form
// is what every color-managed app uses.
//
// Performance: a 256-entry LUT for sRGB->linear is built at static init time
// (input is 8-bit so this is exact). A second, 65536-entry LUT covers 16-bit
// input for the deep-colour ingest path (RAW / 16-bit TIFF / PNG16); it is
// also exact, and it is built lazily so 8-bit-only sessions never pay for it.
// The reverse direction (linear->sRGB) takes a float in [0,1] so must be
// computed, not looked up; we amortize the cost with LUTs — a 4096-entry
// nearest-neighbour table for 8-bit output, and a separate 16384-entry
// interpolated table for 16-bit output. They are deliberately NOT the same
// table; see the comment above linearToSrgb16().
// ==============================================================================
#pragma once

#include <array>
#include <cmath>

namespace lps::colorspace {

// ---- Scalar exact implementations -------------------------------------------
inline float srgbToLinear(float c)
{
    if (c <= 0.04045f) return c / 12.92f;
    return std::pow((c + 0.055f) / 1.055f, 2.4f);
}

inline float linearToSrgb(float c)
{
    if (c <= 0.0031308f) return c * 12.92f;
    return 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

// ---- 8-bit to linear LUT (exact, 256 entries) -------------------------------
struct SrgbToLinearLut
{
    std::array<float, 256> table{};
    SrgbToLinearLut()
    {
        for (int i = 0; i < 256; ++i)
            table[static_cast<size_t>(i)] = srgbToLinear(i / 255.0f);
    }
};

inline const std::array<float, 256>& srgb8ToLinearLut()
{
    static const SrgbToLinearLut lut;
    return lut.table;
}

// Convert an 8-bit sRGB channel value to a linear float in [0,1].
// This is EXACT (no interpolation) because input domain is integer.
inline float srgb8ToLinear(unsigned char v)
{
    return srgb8ToLinearLut()[v];
}

// ---- 16-bit to linear LUT (exact, 65536 entries) ----------------------------
// This is the deep-colour ingest path: RAW output at output_bps=16, 16-bit
// TIFF/PNG, anything QImage reports as Format_RGBX64 / Format_RGBA64 /
// Format_RGBA64_Premultiplied / Format_Grayscale16.
//
// Why a full table rather than calling the transfer function per sample:
//
//   1. Exactness. The input domain is a 16-bit integer, so a table with one
//      entry per code value is EXACT — no interpolation error at all. That is
//      the same guarantee the 8-bit table gives, and the reason neither of
//      them needs the interpolation the linear->sRGB table needs.
//
//   2. Speed. std::pow() costs on the order of 100 cycles. A 45 MP frame is
//      ~135 million channel conversions; computing the transfer function
//      inline would add seconds of wall time to every image open, on the one
//      code path whose whole purpose is to make large files usable. A table
//      lookup is a single load.
//
//   3. Cost is bounded and small. 65536 * sizeof(float) = 256 KB, once, for
//      the life of the process. For scale: one 45 MP RGBA64 frame is ~360 MB,
//      so the table is under 0.1% of the data it serves, and it fits
//      comfortably in L2 on any machine that can open such a file.
//
//   4. It is lazy. Function-local static => constructed on first use (and
//      thread-safe since C++11, which matters because the ingest loop is
//      parallel). A session that only ever opens JPEGs allocates nothing and
//      runs no pow() calls here.
struct Srgb16ToLinearLut
{
    static constexpr int kSize = 65536;
    std::array<float, kSize> table{};
    Srgb16ToLinearLut()
    {
        for (int i = 0; i < kSize; ++i)
            table[static_cast<size_t>(i)] = srgbToLinear(static_cast<float>(i) / 65535.0f);
    }
};

inline const std::array<float, Srgb16ToLinearLut::kSize>& srgb16ToLinearLut()
{
    static const Srgb16ToLinearLut lut;
    return lut.table;
}

// Convert a 16-bit sRGB channel value to a linear float in [0,1].
// EXACT (no interpolation) — the input domain is integer.
//
// Callers in a hot loop should hoist srgb16ToLinearLut() out of the loop and
// index the array directly, exactly as the 8-bit ingest path does; this
// wrapper is for one-off conversions.
inline float srgb16ToLinear(unsigned short v)
{
    return srgb16ToLinearLut()[v];
}

// ---- Linear float to 8-bit sRGB (LUT + saturating cast) ---------------------
// Input: linear value, typically in [0,1] but may exceed (clamp before encode).
// Output: 8-bit sRGB-encoded byte.
//
// Uses a 4096-entry LUT indexed by linear value * 4095. Error bound is
// below 0.5 LSB at 8-bit output — imperceptible — and it's ~10x faster than
// calling pow() per pixel on typical CPUs.
struct LinearToSrgbLut
{
    static constexpr int kSize = 4096;
    std::array<unsigned char, kSize> table{};
    LinearToSrgbLut()
    {
        for (int i = 0; i < kSize; ++i) {
            const float lin = static_cast<float>(i) / (kSize - 1);
            float s = linearToSrgb(lin);
            if (s < 0.0f) s = 0.0f;
            if (s > 1.0f) s = 1.0f;
            table[static_cast<size_t>(i)] = static_cast<unsigned char>(s * 255.0f + 0.5f);
        }
    }
};

inline const std::array<unsigned char, 4096>& linearToSrgb8Lut()
{
    static const LinearToSrgbLut lut;
    return lut.table;
}

inline unsigned char linearToSrgb8(float linear)
{
    // Saturating clamp to avoid out-of-range LUT access.
    if (!(linear > 0.0f)) return 0;      // also catches NaN
    if (linear >= 1.0f) return 255;
    const int idx = static_cast<int>(linear * 4095.0f + 0.5f);
    return linearToSrgb8Lut()[static_cast<size_t>(idx)];
}

// ---- Linear float to 16-bit sRGB (fine LUT + linear interpolation) ----------
// The export path. Same transfer function as linearToSrgb8() above, but the
// 8-bit table cannot be reused, and the reason is the whole point of 16-bit
// export:
//
//   linearToSrgb8()'s table is indexed by the LINEAR value and read with
//   nearest-neighbour rounding. It can therefore only ever emit 4096 distinct
//   results. At 8-bit output (256 codes) that is invisible; at 16-bit output
//   (65536 codes) it would hand back a file whose 16-bit containers hold
//   about 12 bits of information — the exact failure this path exists to fix.
//
// Two things make the 16-bit table different: it is finer, and it is read
// with linear interpolation rather than rounded to the nearest entry. Both
// are needed. Interpolation is what removes the "only N distinct outputs"
// ceiling; the extra entries are what keep the interpolation error small
// where the sRGB curve bends hardest, just above the 0.0031308 knee.
//
// Why not just call linearToSrgb() per sample? It is exact, and it was
// measured. On this toolchain a full RGBA encode costs 221 ms per megapixel
// against 7.5 ms for the interpolated table — 30x. A 45 MP export would
// spend 10 seconds single-threaded inside pow() alone. That is not a price
// worth paying for the last 0.004 codes of RMS error (see the table below).
//
// Measured (MinGW 13.1 -O3, x86-64) against a correctly-rounded double
// reference over 5.78M samples: a uniform sweep of [0,1], the quarter/half/
// three-quarter point of every table interval (worst case for interpolation),
// a log-spaced sweep of the shadows down to 1e-6, and 2M random values.
// Errors are in 16-bit output codes.
//
//   encoder                      max   RMS      interpolation   ms/megapixel
//   -------------------------------------------------------------------------
//   4096 entries, interpolated     2   0.085        1.06             6.8
//   8192 entries, interpolated     1   0.054        0.48             6.9
//   16384 entries, interpolated    1   0.042        0.19             7.5   <--
//   65536 entries, interpolated    1   0.041        0.02             7.9
//   direct linearToSrgb()          1   0.038        0.00           221.3
//
// The "max" column is 1 for everything except the 4096 table because 1 code
// is the floor: single-precision arithmetic alone disagrees with a correctly
// rounded double result on ~0.14% of samples, purely through values landing
// either side of a .5 rounding boundary. The 16384 table sits at 0.19%, i.e.
// within 0.05 percentage points of what exact computation achieves, and its
// interpolation error of 0.19 codes is well under the half-code that would
// be needed to change a correctly-rounded result on its own.
//
// The table also round-trips exactly: feeding all 65536 outputs of
// srgb16ToLinear() back through linearToSrgb16() reproduces every code with
// zero mismatches. (The 4096-entry table gets 841 of them wrong, which is
// the concrete reason it was not reused.)
//
// Cost: 16384 * sizeof(float) = 64 KB, once, built lazily on first use like
// the ingest table. That is 0.018% of a single 45 MP RGBA64 frame, and it
// fits in L2 on any machine that can open one.
struct LinearToSrgb16Lut
{
    static constexpr int kSize = 16384;

    // Entries are the sRGB-encoded value ALREADY scaled to the 16-bit output
    // range, so the hot path is one lerp and one rounded cast — no extra
    // multiply by 65535 per sample.
    std::array<float, kSize> table{};

    LinearToSrgb16Lut()
    {
        for (int i = 0; i < kSize; ++i) {
            const float lin = static_cast<float>(i) / (kSize - 1);
            float s = linearToSrgb(lin);
            if (s < 0.0f) s = 0.0f;
            if (s > 1.0f) s = 1.0f;
            table[static_cast<size_t>(i)] = s * 65535.0f;
        }
    }
};

inline const std::array<float, LinearToSrgb16Lut::kSize>& linearToSrgb16Lut()
{
    static const LinearToSrgb16Lut lut;
    return lut.table;
}

// Input: linear value, typically in [0,1] but may exceed (saturating).
// Output: 16-bit sRGB-encoded code point.
//
// Callers in a hot loop should hoist linearToSrgb16Lut() out of the loop and
// interpolate the array directly if they need the last few percent; this
// wrapper already hoists nothing but the function-local static guard, which
// the compiler folds to a single predictable load.
inline unsigned short linearToSrgb16(float linear)
{
    // Saturating clamp to avoid out-of-range LUT access.
    if (!(linear > 0.0f)) return 0;          // also catches NaN
    if (linear >= 1.0f) return 65535;

    constexpr int kLast = LinearToSrgb16Lut::kSize - 1;
    const std::array<float, LinearToSrgb16Lut::kSize>& lut = linearToSrgb16Lut();

    const float pos   = linear * static_cast<float>(kLast);
    const int   floor = static_cast<int>(pos);
    // linear < 1 already guarantees floor <= kLast - 1 for kSize = 16384, but
    // clamp anyway so changing kSize can never turn into an out-of-bounds
    // read on the last interval. It costs one predictable compare.
    const size_t idx  = static_cast<size_t>(floor < kLast ? floor : kLast - 1);
    const float  frac = pos - static_cast<float>(idx);

    const float lo = lut[idx];
    const float hi = lut[idx + 1];
    return static_cast<unsigned short>(lo + (hi - lo) * frac + 0.5f);
}

} // namespace lps::colorspace
