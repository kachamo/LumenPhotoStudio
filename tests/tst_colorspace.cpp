// ==============================================================================
// tests/tst_colorspace.cpp
// The sRGB transfer function and its lookup tables.
//
// This is the foundation the whole renderer sits on: if these conversions are
// wrong, every adjustment is wrong in a way that is very hard to see and very
// easy to argue about. The tests below pin the exact behaviour the pipeline
// relies on, including the 16-bit round trip that the export path needs.
// ==============================================================================
#include "util/ColorMath.h"
#include "util/ColorSpace.h"

#include <QtTest>

#include <cmath>

using namespace lps;

class TstColorSpace : public QObject
{
    Q_OBJECT

private slots:
    void transferFunctionMatchesSpec();
    void transferFunctionRoundTrips();
    void lut8IsExact();
    void lut16RoundTripsEveryCode();
    void handlesOutOfRangeInput();
    void clampIsNaNSafe();
    void toByteNeverInvokesUndefinedBehaviour();
};

// The piecewise IEC 61966-2-1 definition, written out longhand so the test does
// not simply restate the implementation.
static double specSrgbToLinear(double v)
{
    return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}

void TstColorSpace::transferFunctionMatchesSpec()
{
    // Including both sides of the 0.04045 knee, which is where a pow(2.2)
    // approximation diverges most and where an off-by-one in the branch hides.
    for (const double v : {0.0, 0.01, 0.04, 0.04045, 0.05, 0.2, 0.5, 0.8, 1.0}) {
        const double expected = specSrgbToLinear(v);
        const double actual   = colorspace::srgbToLinear(static_cast<float>(v));
        QVERIFY2(std::abs(actual - expected) < 1e-6,
                 qPrintable(QStringLiteral("v=%1 expected=%2 actual=%3")
                                .arg(v).arg(expected).arg(actual)));
    }
}

void TstColorSpace::transferFunctionRoundTrips()
{
    for (int i = 0; i <= 1000; ++i) {
        const float v      = static_cast<float>(i) / 1000.0f;
        const float linear = colorspace::srgbToLinear(v);
        const float back   = colorspace::linearToSrgb(linear);
        QVERIFY2(std::abs(back - v) < 1e-5f,
                 qPrintable(QStringLiteral("v=%1 back=%2").arg(v).arg(back)));
    }
}

void TstColorSpace::lut8IsExact()
{
    // The 8-bit table is a plain lookup, so it must agree with the function
    // exactly at every one of its 256 entries — no interpolation error is
    // acceptable here.
    const auto& lut = colorspace::srgb8ToLinearLut();
    for (int code = 0; code < 256; ++code) {
        const float expected = colorspace::srgbToLinear(static_cast<float>(code) / 255.0f);
        QVERIFY2(std::abs(lut[static_cast<size_t>(code)] - expected) < 1e-6f,
                 qPrintable(QStringLiteral("code=%1").arg(code)));
    }
}

void TstColorSpace::lut16RoundTripsEveryCode()
{
    // Every 16-bit code must survive decode -> encode unchanged. This is the
    // property the export path depends on, and it is why the 4096-entry table
    // could not be reused: it got 841 of these wrong.
    int mismatches = 0;
    for (int code = 0; code <= 65535; ++code) {
        const float linear = colorspace::srgb16ToLinear(static_cast<unsigned short>(code));
        if (colorspace::linearToSrgb16(linear) != static_cast<unsigned short>(code))
            ++mismatches;
    }
    QCOMPARE(mismatches, 0);
}

void TstColorSpace::handlesOutOfRangeInput()
{
    // Engines can push values outside [0,1] before the encode clamps them, and
    // NaN can reach here through a corrupt preset. None of it may wrap around.
    QCOMPARE(colorspace::linearToSrgb16(-1.0f), static_cast<unsigned short>(0));
    QCOMPARE(colorspace::linearToSrgb16(2.0f),  static_cast<unsigned short>(65535));
    QCOMPARE(colorspace::linearToSrgb16(std::numeric_limits<float>::quiet_NaN()),
             static_cast<unsigned short>(0));
    QCOMPARE(colorspace::linearToSrgb8(-1.0f), static_cast<unsigned char>(0));
    QCOMPARE(colorspace::linearToSrgb8(2.0f),  static_cast<unsigned char>(255));
}

void TstColorSpace::clampIsNaNSafe()
{
    // math::clamp is the clamp every per-pixel loop uses. Written the obvious
    // way — `(v < lo) ? lo : (v > hi) ? hi : v` — both comparisons are false
    // for NaN and it returns NaN unchanged, so one NaN parameter poisons a
    // whole frame. The `>=`/`<=` form costs the same and lands NaN on `lo`.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    QCOMPARE(math::clamp(nan, -10.0f, 10.0f), -10.0f);
    QCOMPARE(math::clamp01(nan), 0.0f);

    // Infinity is a separate IEEE class: a fix that special-cases only NaN
    // would pass the two assertions above and still let +/-inf through.
    QCOMPARE(math::clamp(inf,  -10.0f, 10.0f),  10.0f);
    QCOMPARE(math::clamp(-inf, -10.0f, 10.0f), -10.0f);

    // The ordinary cases must be untouched by the reformulation — including
    // both boundaries, where `<` versus `<=` actually differ.
    QCOMPARE(math::clamp(5.0f,   -10.0f, 10.0f),   5.0f);
    QCOMPARE(math::clamp(-20.0f, -10.0f, 10.0f), -10.0f);
    QCOMPARE(math::clamp(20.0f,  -10.0f, 10.0f),  10.0f);
    QCOMPARE(math::clamp(-10.0f, -10.0f, 10.0f), -10.0f);
    QCOMPARE(math::clamp(10.0f,  -10.0f, 10.0f),  10.0f);
}

void TstColorSpace::toByteNeverInvokesUndefinedBehaviour()
{
    // toByte() casts `clamp01(v) * 255 + 0.5` to unsigned char. A float->int
    // cast whose value is NaN or out of range is undefined behaviour, not
    // merely a surprising number, so this depends on clamp01 having already
    // squashed it. The header used to claim that guarantee without holding it.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    QCOMPARE(math::toByte(nan), static_cast<unsigned char>(0));
    QCOMPARE(math::toByte(std::numeric_limits<float>::infinity()),
             static_cast<unsigned char>(255));
    QCOMPARE(math::toByte(-1.0f), static_cast<unsigned char>(0));
    QCOMPARE(math::toByte(0.0f),  static_cast<unsigned char>(0));
    QCOMPARE(math::toByte(1.0f),  static_cast<unsigned char>(255));
}

QTEST_MAIN(TstColorSpace)
#include "tst_colorspace.moc"
