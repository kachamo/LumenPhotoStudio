// ==============================================================================
// tests/tst_pixelbuffer.cpp
// The pipeline's entry and exit: sRGB QImage <-> linear-light float32 RGBA.
//
// Everything the renderer does happens between these two conversions, so a
// mistake here is invisible in review and catastrophic in output. Two failure
// modes in particular are completely silent:
//
//   * Channel order. ARGB32 is B,G,R,A *bytes*; RGBX64/RGBA64 are R,G,B,A
//     *halfwords*. Reading one with the other's indices trades red for blue
//     and still yields a perfectly plausible-looking image.
//   * Depth. Routing a 16-bit source through the 8-bit path throws away eight
//     bits per channel before the float pipeline ever sees the data, and
//     nothing downstream can tell that it happened.
//
// So: the transfer function is checked against an independent restatement of
// IEC 61966-2-1 rather than against the LUTs the implementation uses, and
// channel order is checked by reading raw bytes/halfwords rather than by
// trusting the same accessor the implementation trusts. Where a test's point
// is "this must not collapse to 8 bits", it measures the collapse it is
// guarding against, so the threshold is not a number pulled out of the air.
// ==============================================================================
#include "core/PixelBuffer.h"

#include <QtTest>

#include <QColorSpace>
#include <QRgba64>
#include <QSet>

#include <cmath>
#include <vector>

using namespace lps;

class TstPixelBuffer : public QObject
{
    Q_OBJECT

private slots:
    void srgb8IngestMatchesSpec();
    void srgb8IngestReadsBgraByteOrder();
    void nonArgb32SourcesAreNormalized();
    void deepIngestReadsRgbaHalfwordOrder();
    void deepIngestKeepsMoreThanEightBits();
    void premultipliedIsUndoneBeforeTransfer();
    void grayscale16ReplicatesIntoRgb();
    void toSrgb16ImageIsTaggedDeepRgba();
    void toSrgb16ImageMatchesSpec();
    void deepRoundTripIsNearLossless();
    void nullInputProducesNullBuffer();
    void resetRejectsMismatchedPixelCount();
};

// ---- Independent restatement of the spec ------------------------------------
// Written out longhand (and in double) so these tests do not simply replay the
// implementation's own tables back at it.
static double specSrgbToLinear(double v)
{
    return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}

static double specLinearToSrgb(double v)
{
    if (v <= 0.0) return 0.0;
    if (v >= 1.0) return 1.0;
    return v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
}

// ---- Small helpers ----------------------------------------------------------
// A 1x1 image of the requested 64-bit format, with the four halfwords written
// straight into memory in address order. Deliberately does NOT go through
// QRgba64 or qRgba64(): the point of several tests below is to pin the *memory
// layout*, and using the same helper the implementation uses would make the
// assertion circular.
static QImage deepPixel(QImage::Format format, quint16 h0, quint16 h1,
                        quint16 h2, quint16 h3)
{
    QImage img(1, 1, format);
    quint16* p = reinterpret_cast<quint16*>(img.bits());
    p[0] = h0; p[1] = h1; p[2] = h2; p[3] = h3;
    return img;
}

static void verifyChannel(float actual, double expected, double tolerance,
                          const char* what)
{
    QVERIFY2(std::abs(static_cast<double>(actual) - expected) < tolerance,
             qPrintable(QStringLiteral("%1: expected %2, got %3")
                            .arg(QLatin1String(what))
                            .arg(expected, 0, 'g', 9)
                            .arg(static_cast<double>(actual), 0, 'g', 9)));
}

// ==============================================================================
// 8-bit ingest — the original path, which must stay bit-for-bit what it was.
// ==============================================================================
void TstPixelBuffer::srgb8IngestMatchesSpec()
{
    // A spread that straddles the 0.04045 knee (codes 10 and 11 sit either
    // side of it), plus both endpoints and a couple of midtones. Alpha is
    // varied too, because alpha is the one channel that must NOT be
    // linearized — it is a coverage fraction, not a light measurement.
    struct Sample { int r, g, b, a; };
    const Sample samples[] = {
        {   0,   0,   0, 255 }, {  10,  11,  12, 255 }, {  64, 128, 192, 128 },
        { 255, 255, 255, 255 }, {   1, 254,   7,   0 }, { 250, 140,  10, 200 },
        { 128, 128, 128,  64 }, {  33,  99, 199, 255 },
    };
    constexpr int kCount = static_cast<int>(sizeof(samples) / sizeof(samples[0]));

    QImage img(kCount, 1, QImage::Format_ARGB32);
    uchar* row = img.scanLine(0);
    for (int i = 0; i < kCount; ++i) {
        // ARGB32 is B,G,R,A in memory on little-endian. Written as raw bytes
        // on purpose — see deepPixel() above for why.
        row[i * 4 + 0] = static_cast<uchar>(samples[i].b);
        row[i * 4 + 1] = static_cast<uchar>(samples[i].g);
        row[i * 4 + 2] = static_cast<uchar>(samples[i].r);
        row[i * 4 + 3] = static_cast<uchar>(samples[i].a);
    }

    const PixelBuffer buf = PixelBuffer::fromSrgbImage(img);
    QCOMPARE(buf.width(), kCount);
    QCOMPARE(buf.height(), 1);

    for (int i = 0; i < kCount; ++i) {
        const float* p = buf.scanline(0) + i * 4;
        verifyChannel(p[0], specSrgbToLinear(samples[i].r / 255.0), 1e-6, "R");
        verifyChannel(p[1], specSrgbToLinear(samples[i].g / 255.0), 1e-6, "G");
        verifyChannel(p[2], specSrgbToLinear(samples[i].b / 255.0), 1e-6, "B");
        // Alpha is a straight /255 — no transfer function.
        verifyChannel(p[3], samples[i].a / 255.0, 1e-6, "A");
    }
}

void TstPixelBuffer::srgb8IngestReadsBgraByteOrder()
{
    // One deliberately lopsided pixel: if R and B were swapped, every other
    // assertion in this file would still pass on grey test data. R is near the
    // top of the range and B near the bottom, so a swap moves the value by
    // more than 0.9 in linear light — nothing subtle about the failure.
    QImage img(1, 1, QImage::Format_ARGB32);
    img.setPixel(0, 0, qRgba(250, 140, 10, 200));

    // First pin the layout itself: Qt's documented ARGB32 memory order is
    // B,G,R,A on little-endian. If this ever stops holding, the ingest loop's
    // hard-coded sp[2]/sp[1]/sp[0] indices are wrong and we want to know here
    // rather than in a user's export.
    const uchar* raw = img.constScanLine(0);
    QCOMPARE(raw[0], uchar(10));    // B
    QCOMPARE(raw[1], uchar(140));   // G
    QCOMPARE(raw[2], uchar(250));   // R
    QCOMPARE(raw[3], uchar(200));   // A

    const PixelBuffer buf = PixelBuffer::fromSrgbImage(img);
    const float* p = buf.data();
    verifyChannel(p[0], specSrgbToLinear(250 / 255.0), 1e-6, "R");
    verifyChannel(p[1], specSrgbToLinear(140 / 255.0), 1e-6, "G");
    verifyChannel(p[2], specSrgbToLinear( 10 / 255.0), 1e-6, "B");
    // And state the swap explicitly, so the failure message names the bug.
    QVERIFY2(p[0] > p[2] + 0.9f, "R and B look swapped on the 8-bit path");
}

void TstPixelBuffer::nonArgb32SourcesAreNormalized()
{
    // Anything that is neither ARGB32 nor a deep format has to be converted
    // first. RGB888 is the interesting case because its memory order is the
    // opposite of ARGB32's, so a missing conversion would swap R and B.
    QImage rgb888(1, 1, QImage::Format_RGB888);
    uchar* raw = rgb888.scanLine(0);
    raw[0] = 250; raw[1] = 140; raw[2] = 10;   // R, G, B in that byte order

    const PixelBuffer buf = PixelBuffer::fromSrgbImage(rgb888);
    const float* p = buf.data();
    verifyChannel(p[0], specSrgbToLinear(250 / 255.0), 1e-6, "R");
    verifyChannel(p[1], specSrgbToLinear(140 / 255.0), 1e-6, "G");
    verifyChannel(p[2], specSrgbToLinear( 10 / 255.0), 1e-6, "B");
    // RGB888 has no alpha; normalization must supply an opaque one.
    QCOMPARE(p[3], 1.0f);
}

// ==============================================================================
// 16-bit ingest — the deep-colour path.
// ==============================================================================
void TstPixelBuffer::deepIngestReadsRgbaHalfwordOrder()
{
    // The single easiest thing to get silently wrong. RGBX64 is four
    // native-endian quint16 laid out R,G,B,A at increasing addresses — the
    // *opposite* component order from ARGB32's bytes. A distinctive pixel
    // makes a swap unmistakable: R and B differ by 55000 codes.
    const QImage img = deepPixel(QImage::Format_RGBX64, 60000, 30000, 5000, 65535);

    // Assert the layout two independent ways. (1) Through QRgba64, which is
    // what the implementation uses...
    const QRgba64* viaAccessor = reinterpret_cast<const QRgba64*>(img.constScanLine(0));
    QCOMPARE(viaAccessor->red(),   quint16(60000));
    QCOMPARE(viaAccessor->green(), quint16(30000));
    QCOMPARE(viaAccessor->blue(),  quint16(5000));
    QCOMPARE(viaAccessor->alpha(), quint16(65535));
    // ...and (2) straight out of memory, which is what the accessor is
    // *claiming*. If Qt ever flips its shift enum the wrong way, only this
    // second check catches it — the implementation and a QRgba64-based test
    // would agree with each other while both being wrong.
    const quint16* viaMemory = reinterpret_cast<const quint16*>(img.constScanLine(0));
    QCOMPARE(viaMemory[0], quint16(60000));
    QCOMPARE(viaMemory[1], quint16(30000));
    QCOMPARE(viaMemory[2], quint16(5000));
    QCOMPARE(viaMemory[3], quint16(65535));

    const PixelBuffer buf = PixelBuffer::fromSrgbImage(img);
    const float* p = buf.data();
    verifyChannel(p[0], specSrgbToLinear(60000 / 65535.0), 1e-6, "R");
    verifyChannel(p[1], specSrgbToLinear(30000 / 65535.0), 1e-6, "G");
    verifyChannel(p[2], specSrgbToLinear( 5000 / 65535.0), 1e-6, "B");
    // Format_RGBX64: Qt guarantees the X halfword reads back as 0xffff, so
    // alpha must come out at exactly 1.0, not 0.9999.
    QCOMPARE(p[3], 1.0f);
    QVERIFY2(p[0] > p[2] + 0.8f, "R and B look swapped on the deep path");

    // Format_RGBA64 shares the layout and must ingest identically, with the
    // alpha halfword now meaning something.
    const QImage withAlpha = deepPixel(QImage::Format_RGBA64, 60000, 30000, 5000, 32768);
    const PixelBuffer bufA = PixelBuffer::fromSrgbImage(withAlpha);
    const float* q = bufA.data();
    verifyChannel(q[0], specSrgbToLinear(60000 / 65535.0), 1e-6, "RGBA64 R");
    verifyChannel(q[1], specSrgbToLinear(30000 / 65535.0), 1e-6, "RGBA64 G");
    verifyChannel(q[2], specSrgbToLinear( 5000 / 65535.0), 1e-6, "RGBA64 B");
    verifyChannel(q[3], 32768 / 65535.0, 1e-6, "RGBA64 A");
}

void TstPixelBuffer::deepIngestKeepsMoreThanEightBits()
{
    // A ramp of 1024 16-bit codes spaced 64 apart. Sixty-four codes is a
    // quarter of an 8-bit LSB, so an ARGB32 detour maps the whole ramp onto
    // 256 codes and three quarters of the levels vanish. Reaching the float
    // buffer with all 1024 intact is the entire justification for the deep
    // ingest path existing.
    constexpr int kSteps = 1024;
    QImage ramp(kSteps, 1, QImage::Format_RGBX64);
    quint16* row = reinterpret_cast<quint16*>(ramp.scanLine(0));
    for (int i = 0; i < kSteps; ++i) {
        const quint16 v = static_cast<quint16>(i * 64);
        row[i * 4 + 0] = v; row[i * 4 + 1] = v; row[i * 4 + 2] = v;
        row[i * 4 + 3] = 65535;
    }

    const PixelBuffer deep = PixelBuffer::fromSrgbImage(ramp);
    QSet<float> deepLevels;
    for (int i = 0; i < kSteps; ++i) deepLevels.insert(deep.data()[i * 4]);

    // Measure the collapse rather than assuming it: the same ramp forced
    // through ARGB32 is exactly what the 8-bit path would have produced.
    const PixelBuffer shallow =
        PixelBuffer::fromSrgbImage(ramp.convertToFormat(QImage::Format_ARGB32));
    QSet<float> shallowLevels;
    for (int i = 0; i < kSteps; ++i) shallowLevels.insert(shallow.data()[i * 4]);

    QVERIFY2(shallowLevels.size() <= 256,
             qPrintable(QStringLiteral("8-bit control produced %1 levels; the "
                                       "comparison below is meaningless unless "
                                       "this is capped at 256")
                            .arg(shallowLevels.size())));
    QCOMPARE(deepLevels.size(), kSteps);
    QVERIFY(deepLevels.size() > shallowLevels.size());
}

void TstPixelBuffer::premultipliedIsUndoneBeforeTransfer()
{
    // sRGB's transfer function is non-linear, so applying it to a
    // premultiplied value is not "slightly off" — it is a different number
    // entirely. At half alpha the two answers here are 0.82 and 0.18.
    const QImage straight = deepPixel(QImage::Format_RGBA64, 60000, 30000, 5000, 32768);
    const QImage premul   = straight.convertToFormat(QImage::Format_RGBA64_Premultiplied);

    // Sanity-check the fixture: the stored halfwords really are scaled down.
    const quint16* raw = reinterpret_cast<const quint16*>(premul.constScanLine(0));
    QVERIFY2(raw[0] < 35000, "fixture is not actually premultiplied");

    const PixelBuffer buf = PixelBuffer::fromSrgbImage(premul);
    const float* p = buf.data();

    const double correct = specSrgbToLinear(60000 / 65535.0);   // ~0.8185
    const double wrong   = specSrgbToLinear(30000 / 65535.0);   // ~0.1770

    // Tolerance covers the one-code loss of the premultiply/unpremultiply
    // round trip (about 3e-5 in linear light here), and nothing more.
    verifyChannel(p[0], correct, 2e-4, "un-premultiplied R");
    QVERIFY2(std::abs(static_cast<double>(p[0]) - wrong) > 0.5,
             "transfer function was applied to premultiplied values");

    verifyChannel(p[3], 32768 / 65535.0, 1e-5, "A");
}

void TstPixelBuffer::grayscale16ReplicatesIntoRgb()
{
    QImage grey(1, 1, QImage::Format_Grayscale16);
    *reinterpret_cast<quint16*>(grey.bits()) = 40000;

    const PixelBuffer buf = PixelBuffer::fromSrgbImage(grey);
    const float* p = buf.data();
    const double expected = specSrgbToLinear(40000 / 65535.0);

    verifyChannel(p[0], expected, 1e-6, "grey R");
    verifyChannel(p[1], expected, 1e-6, "grey G");
    verifyChannel(p[2], expected, 1e-6, "grey B");
    // Identical, not merely close: the grey is replicated, not recomputed.
    QCOMPARE(p[0], p[1]);
    QCOMPARE(p[1], p[2]);
    QCOMPARE(p[3], 1.0f);
}

// ==============================================================================
// 16-bit egress — the export path.
// ==============================================================================
void TstPixelBuffer::toSrgb16ImageIsTaggedDeepRgba()
{
    PixelBuffer opaque;
    opaque.reset(1, 1, { 0.8f, 0.2f, 0.02f, 1.0f });
    const QImage out = opaque.toSrgb16Image();

    QCOMPARE(out.depth(), 64);
    // Fully opaque frames are reinterpreted as RGBX64 — same bytes, no alpha
    // plane in the exported file. This is the ordinary-photograph case.
    QCOMPARE(out.format(), QImage::Format_RGBX64);
    // Untagged, QImage::convertToColorSpace() is a silent no-op downstream, so
    // the tag is load-bearing rather than decorative.
    QVERIFY(out.colorSpace().isValid());
    QCOMPARE(out.colorSpace(), QColorSpace(QColorSpace::SRgb));

    // Channel order, read out of memory rather than through QRgba64 — same
    // reasoning as on ingest. R was the largest linear value, so it must be
    // the largest code, and it must be the *first* halfword.
    const quint16* raw = reinterpret_cast<const quint16*>(out.constScanLine(0));
    QVERIFY2(raw[0] > raw[1] && raw[1] > raw[2],
             "toSrgb16Image() halfwords are not in R,G,B order");
    QCOMPARE(raw[3], quint16(65535));

    // A single non-opaque pixel must keep the alpha plane.
    PixelBuffer translucent;
    translucent.reset(1, 1, { 0.8f, 0.2f, 0.02f, 0.5f });
    const QImage outA = translucent.toSrgb16Image();
    QCOMPARE(outA.format(), QImage::Format_RGBA64);
    QCOMPARE(outA.depth(), 64);
    const quint16* rawA = reinterpret_cast<const quint16*>(outA.constScanLine(0));
    QCOMPARE(rawA[3], quint16(std::lround(0.5 * 65535.0)));
}

void TstPixelBuffer::toSrgb16ImageMatchesSpec()
{
    // Encoded codes are compared against the exact transfer function, not
    // against the interpolated table the encoder uses. The implementation
    // documents a worst case of one code against a correctly-rounded double
    // reference, so that is the budget allowed here.
    const std::vector<float> values = {
        0.0f, 1e-4f, 0.0031308f, 0.01f, 0.05f, 0.18f, 0.2158f,
        0.5f, 0.75f, 0.9f, 0.99f, 1.0f,
    };

    std::vector<float> pixels;
    pixels.reserve(values.size() * 4);
    for (const float v : values) {
        pixels.push_back(v); pixels.push_back(v); pixels.push_back(v);
        pixels.push_back(1.0f);
    }

    PixelBuffer buf;
    buf.reset(static_cast<int>(values.size()), 1, std::move(pixels));
    const QImage out = buf.toSrgb16Image();
    const quint16* raw = reinterpret_cast<const quint16*>(out.constScanLine(0));

    for (size_t i = 0; i < values.size(); ++i) {
        const long expected =
            std::lround(specLinearToSrgb(static_cast<double>(values[i])) * 65535.0);
        const long actual = raw[i * 4];
        QVERIFY2(std::abs(actual - expected) <= 1,
                 qPrintable(QStringLiteral("linear %1: expected code %2, got %3")
                                .arg(static_cast<double>(values[i]))
                                .arg(expected).arg(actual)));
    }
    // The endpoints must be exact, not merely within a code — a white that
    // exports as 65534 is a bug report waiting to happen.
    QCOMPARE(raw[0], quint16(0));
    QCOMPARE(raw[(values.size() - 1) * 4], quint16(65535));
}

void TstPixelBuffer::deepRoundTripIsNearLossless()
{
    // fromSrgbImage(toSrgb16Image(b)) has to give back what went in, because
    // that is the loop an export-then-reimport performs and the property the
    // whole deep path is sold on.
    constexpr int kCount = 64;
    std::vector<float> pixels;
    pixels.reserve(kCount * 4);
    for (int i = 0; i < kCount; ++i) {
        const float v = static_cast<float>(i) / (kCount - 1);
        pixels.push_back(v);
        pixels.push_back(1.0f - v);
        pixels.push_back(v * 0.05f);   // deliberately shadow-heavy
        pixels.push_back(1.0f);
    }
    PixelBuffer original;
    original.reset(kCount, 1, std::vector<float>(pixels));

    const PixelBuffer deepRt = PixelBuffer::fromSrgbImage(original.toSrgb16Image());
    QCOMPARE(deepRt.width(), kCount);

    float deepWorst = 0.0f;
    for (int i = 0; i < kCount * 4; ++i)
        deepWorst = std::max(deepWorst, std::abs(deepRt.data()[i] - pixels[static_cast<size_t>(i)]));

    // Budget: two 16-bit codes. The steepest the sRGB decode gets is
    // d(linear)/d(encoded) = 2.4/1.055 ~= 2.28 at white, so two codes is
    // 2 * 2.28 / 65535 ~= 7e-5.
    QVERIFY2(deepWorst < 7e-5f,
             qPrintable(QStringLiteral("16-bit round trip lost %1").arg(deepWorst)));

    // The tolerance above is only meaningful next to what the 8-bit path
    // costs on the same data, so measure that too. If someone quietly
    // redirects toSrgb16Image() through an 8-bit encode, this ratio collapses
    // and the assertion above fires.
    const PixelBuffer shallowRt = PixelBuffer::fromSrgbImage(original.toSrgbImage());
    float shallowWorst = 0.0f;
    for (int i = 0; i < kCount * 4; ++i)
        shallowWorst = std::max(shallowWorst,
                                std::abs(shallowRt.data()[i] - pixels[static_cast<size_t>(i)]));
    QVERIFY2(shallowWorst > deepWorst * 20.0f,
             qPrintable(QStringLiteral("8-bit round trip lost %1, 16-bit lost %2 — "
                                       "the deep path is not buying anything")
                            .arg(shallowWorst).arg(deepWorst)));
}

// ==============================================================================
// Degenerate input.
// ==============================================================================
void TstPixelBuffer::nullInputProducesNullBuffer()
{
    const PixelBuffer fromNull = PixelBuffer::fromSrgbImage(QImage());
    QVERIFY(fromNull.isNull());
    QCOMPARE(fromNull.width(), 0);
    QCOMPARE(fromNull.height(), 0);

    // Both egress paths must survive being called on it — a null buffer is
    // what every early-out in the pipeline hands back.
    QVERIFY(fromNull.toSrgbImage().isNull());
    QVERIFY(fromNull.toSrgb16Image().isNull());

    // A default-constructed QImage of zero size is null too, and the deep
    // branch has its own isNull() check that this exercises.
    QVERIFY(PixelBuffer::fromSrgbImage(QImage(0, 0, QImage::Format_RGBA64)).isNull());
}

void TstPixelBuffer::resetRejectsMismatchedPixelCount()
{
    // reset() is the one way engines can resize the buffer. If it accepted a
    // vector that does not match width*height*4, every scanline() afterwards
    // would read out of bounds, so it must refuse and null itself out.
    PixelBuffer buf;
    buf.reset(2, 2, std::vector<float>(2 * 2 * 4, 0.5f));
    QCOMPARE(buf.width(), 2);
    QVERIFY(!buf.isNull());

    buf.reset(2, 2, std::vector<float>(7, 0.5f));   // one float short
    QVERIFY(buf.isNull());
    QCOMPARE(buf.width(), 0);
    QCOMPARE(buf.height(), 0);

    buf.reset(0, 5, std::vector<float>());
    QVERIFY(buf.isNull());
}

QTEST_MAIN(TstPixelBuffer)
#include "tst_pixelbuffer.moc"
