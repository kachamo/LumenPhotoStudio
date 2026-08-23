// ==============================================================================
// tests/tst_imagepipeline.cpp
// The renderer's contract with everything that calls it.
//
// ImagePipeline has three entry points that must agree with each other:
// render() encodes to 8-bit for the preview, renderToBuffer() stops at the
// linear float buffer for export, and renderUpTo() stops part-way for
// debugging. They share runEngines() precisely so they cannot drift; these
// tests are what make "cannot" true rather than aspirational.
//
// The properties worth defending, and why:
//
//   * The identity fast path returns the input untouched, in its original
//     format. That is what lets an unedited 16-bit RAW export at true 16 bits
//     instead of being laundered through ARGB32 for no reason.
//   * renderToBuffer() and render() must produce the same pixels. If they
//     ever disagree, the shared-chain refactor is broken and an export no
//     longer matches the preview the user approved.
//   * renderToBuffer() must actually preserve depth. A 16-bit file carrying
//     8 bits of information is the exact failure it was added to fix, and it
//     is invisible without counting distinct codes.
//   * clampRanges() has to absorb whatever a corrupt preset hands over.
//     Engines are documented as assuming in-range input, so the pipeline is
//     the only thing standing between a bad number and the pixels.
// ==============================================================================
#include "core/ImagePipeline.h"

#include "core/Look.h"
#include "core/PixelBuffer.h"

#include <QtTest>

#include <QSet>

#include <cmath>
#include <limits>

using namespace lps;

class TstImagePipeline : public QObject
{
    Q_OBJECT

private slots:
    void identityLookReturnsInputUntouched();
    void identityLookStillFillsTheBuffer();
    void renderAndRenderToBufferAgree();
    void renderToBufferPreservesDepth();
    void exposureScalesLinearValues();
    void clampRangesTamesOutOfRangeParameters();
    void nanParametersDoNotCrash();
    void nullInputProducesNullResult();
    void renderUpToSeparatesStages();
};

// ---- Fixtures ---------------------------------------------------------------
// A small ARGB32 image with structure in all three channels, so an engine that
// only touches one of them still shows up.
static QImage patterned8Bit(int w = 37, int h = 23)
{
    QImage img(w, h, QImage::Format_ARGB32);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            img.setPixel(x, y, qRgb((x * 7) % 256, (y * 11) % 256,
                                    ((x + y) * 5) % 256));
        }
    }
    return img;
}

static QImage flat8Bit(int code, int w = 8, int h = 8)
{
    QImage img(w, h, QImage::Format_ARGB32);
    img.fill(QColor(code, code, code, 255));
    return img;
}

// A 16-bit grey ramp with `steps` distinct codes, spaced so that an 8-bit
// detour would merge them. Confined to the lower half of the range so a
// positive exposure does not clip the top and hide the collapse.
static QImage deepRamp(int steps, int codeStep)
{
    QImage img(steps, 1, QImage::Format_RGBX64);
    quint16* row = reinterpret_cast<quint16*>(img.scanLine(0));
    for (int i = 0; i < steps; ++i) {
        const quint16 v = static_cast<quint16>(i * codeStep);
        row[i * 4 + 0] = v; row[i * 4 + 1] = v; row[i * 4 + 2] = v;
        row[i * 4 + 3] = 65535;
    }
    return img;
}

// A Look that lights up several engines at once, for the tests whose point is
// "the chain ran", not "this particular slider is correct".
static Look busyLook()
{
    Look look;
    look.tone.exposure    = 0.5f;
    look.tone.contrast    = 30.0f;
    look.color.saturation = 25.0f;
    look.effects.vignette.amount = -40.0f;
    return look;
}

static bool allFinite(const PixelBuffer& buffer)
{
    const int count = buffer.width() * buffer.height() * 4;
    for (int i = 0; i < count; ++i)
        if (!std::isfinite(buffer.data()[i])) return false;
    return true;
}

// Count how many distinct values a single-row image carries in its red
// channel — the measurement that separates "16-bit" from "16-bit shaped".
static int distinctRedCodes16(const QImage& deep)
{
    QSet<int> codes;
    const quint16* row = reinterpret_cast<const quint16*>(deep.constScanLine(0));
    for (int x = 0; x < deep.width(); ++x) codes.insert(row[x * 4]);
    return codes.size();
}

static int distinctRedCodes8(const QImage& shallow)
{
    QSet<int> codes;
    for (int x = 0; x < shallow.width(); ++x)
        codes.insert(qRed(shallow.pixel(x, 0)));
    return codes.size();
}

// ==============================================================================
// Identity fast path.
// ==============================================================================
void TstImagePipeline::identityLookReturnsInputUntouched()
{
    const ImagePipeline pipeline;

    // The case that matters: a deep source with no edits. If the fast path
    // were removed, this would go 16-bit -> float -> ARGB32 and the export
    // would silently drop to 8 bits for an image nobody even edited.
    const QImage deep = deepRamp(16, 4000);
    const RenderResult deepResult = pipeline.render(deep, Look{});

    QVERIFY(deepResult.wasIdentity);
    QCOMPARE(deepResult.image.format(), QImage::Format_RGBX64);
    QCOMPARE(deepResult.image, deep);
    // It is a copy, not the caller's buffer handed back — the pipeline
    // promises the input is never aliased into the result.
    QVERIFY(deepResult.image.constBits() != deep.constBits());

    // And the ordinary 8-bit case, for the same reasons.
    const QImage shallow = patterned8Bit();
    const RenderResult shallowResult = pipeline.render(shallow, Look{});
    QVERIFY(shallowResult.wasIdentity);
    QCOMPARE(shallowResult.image.format(), QImage::Format_ARGB32);
    QCOMPARE(shallowResult.image, shallow);

    // A Look that is only *nearly* identity must not take the fast path.
    // 1e-4 is the isIdentity epsilon, so 1e-3 is comfortably a real edit.
    Look almost;
    almost.tone.exposure = 1e-3f;
    QVERIFY(!pipeline.render(shallow, almost).wasIdentity);
}

void TstImagePipeline::identityLookStillFillsTheBuffer()
{
    // renderToBuffer() deliberately does NOT short-circuit to an empty
    // result for an identity Look — the caller wants pixels to encode, not a
    // flag. It just skips the engines.
    const ImagePipeline pipeline;
    const QImage input = deepRamp(64, 900);

    const ImagePipeline::BufferResult result = pipeline.renderToBuffer(input, Look{});
    QVERIFY(result.wasIdentity);
    QVERIFY(!result.buffer.isNull());
    QCOMPARE(result.buffer.width(), input.width());
    QCOMPARE(result.buffer.height(), input.height());

    // With no engines run, the buffer must be exactly the ingest of the
    // input and nothing else.
    const PixelBuffer expected = PixelBuffer::fromSrgbImage(input);
    for (int i = 0; i < input.width() * 4; ++i)
        QCOMPARE(result.buffer.data()[i], expected.data()[i]);
}

// ==============================================================================
// The two full-render entry points must not drift apart.
// ==============================================================================
void TstImagePipeline::renderAndRenderToBufferAgree()
{
    const ImagePipeline pipeline;
    const QImage input = patterned8Bit();
    const Look look = busyLook();

    const RenderResult viaImage = pipeline.render(input, look);
    const ImagePipeline::BufferResult viaBuffer = pipeline.renderToBuffer(input, look);

    QCOMPARE(viaImage.wasIdentity, viaBuffer.wasIdentity);
    QVERIFY(!viaImage.wasIdentity);

    // Guard against a vacuous pass: if the Look stopped doing anything, both
    // sides would trivially equal the input and the comparison would prove
    // nothing.
    QVERIFY2(viaImage.image != input, "the test Look no longer changes pixels");

    // render() is renderToBuffer() plus the 8-bit encode, so encoding the
    // buffer must reproduce render()'s image bit for bit. Not "close" —
    // identical, because it is literally the same code path.
    const QImage encoded = viaBuffer.buffer.toSrgbImage();
    QCOMPARE(encoded.format(), viaImage.image.format());
    QCOMPARE(encoded, viaImage.image);
}

void TstImagePipeline::renderToBufferPreservesDepth()
{
    // 1024 distinct 16-bit codes, 32 apart — an eighth of an 8-bit LSB. Any
    // path that passes through ARGB32 cannot return more than 256 of them.
    const ImagePipeline pipeline;
    constexpr int kSteps = 1024;
    const QImage input = deepRamp(kSteps, 32);

    Look look;
    look.tone.exposure = 0.3f;   // a real edit, but not enough to clip

    const ImagePipeline::BufferResult deep = pipeline.renderToBuffer(input, look);
    QVERIFY(!deep.wasIdentity);
    const int deepCodes = distinctRedCodes16(deep.buffer.toSrgb16Image());

    // The control: the same input and the same Look through render().
    const int shallowCodes = distinctRedCodes8(pipeline.render(input, look).image);

    QVERIFY2(shallowCodes <= 256,
             qPrintable(QStringLiteral("8-bit control reported %1 levels; it "
                                       "cannot exceed 256, so something is "
                                       "wrong with the measurement")
                            .arg(shallowCodes)));
    QVERIFY2(deepCodes > 256,
             qPrintable(QStringLiteral("renderToBuffer() yielded only %1 distinct "
                                       "levels — no better than 8-bit")
                            .arg(deepCodes)));
    QVERIFY2(deepCodes > shallowCodes * 4,
             qPrintable(QStringLiteral("deep=%1 shallow=%2").arg(deepCodes).arg(shallowCodes)));
}

// ==============================================================================
// A non-identity Look has to actually do something, in the right direction.
// ==============================================================================
void TstImagePipeline::exposureScalesLinearValues()
{
    // Exposure is a physical stop: +1 doubles the linear value, -1 halves it.
    // Asserted as a ratio with slack rather than a constant, because the tone
    // stage reaches the result through a 4097-entry fused LUT and a perceptual
    // round trip — the shape is the contract, not the last decimal.
    const ImagePipeline pipeline;
    const QImage input = flat8Bit(100);   // mid-dark, so +1 stop cannot clip

    const PixelBuffer ingested = PixelBuffer::fromSrgbImage(input);
    const float base = ingested.data()[0];
    QVERIFY(base > 0.05f && base < 0.2f);

    Look up;
    up.tone.exposure = 1.0f;
    const float brighter = pipeline.renderToBuffer(input, up).buffer.data()[0];
    QVERIFY2(brighter > base, "+1 stop did not brighten the image");
    const float upRatio = brighter / base;
    QVERIFY2(upRatio > 1.95f && upRatio < 2.05f,
             qPrintable(QStringLiteral("+1 stop scaled by %1, expected ~2").arg(upRatio)));

    Look down;
    down.tone.exposure = -1.0f;
    const float darker = pipeline.renderToBuffer(input, down).buffer.data()[0];
    QVERIFY2(darker < base, "-1 stop did not darken the image");
    const float downRatio = darker / base;
    QVERIFY2(downRatio > 0.48f && downRatio < 0.52f,
             qPrintable(QStringLiteral("-1 stop scaled by %1, expected ~0.5").arg(downRatio)));

    // The same edit must be visible through render() too, not just in the
    // float buffer.
    const QImage rendered = pipeline.render(input, up).image;
    QVERIFY(qRed(rendered.pixel(0, 0)) > qRed(input.pixel(0, 0)));
}

// ==============================================================================
// Parameter hygiene.
// ==============================================================================
void TstImagePipeline::clampRangesTamesOutOfRangeParameters()
{
    const ImagePipeline pipeline;

    // Include a pure black pixel: it is the one that turns 0 * infinity into
    // NaN if the exposure clamp is missing, and it is exactly the pixel a
    // "did anything break?" eyeball test skips over.
    QImage input = flat8Bit(120, 4, 4);
    input.setPixel(0, 0, qRgb(0, 0, 0));
    input.setPixel(1, 0, qRgb(255, 255, 255));

    // Documented range is [-10, +10] stops. 1e9 stops is 2^1e9 = infinity.
    Look absurdHigh;
    absurdHigh.tone.exposure = 1.0e9f;
    Look atLimitHigh;
    atLimitHigh.tone.exposure = 10.0f;

    const ImagePipeline::BufferResult high = pipeline.renderToBuffer(input, absurdHigh);
    const ImagePipeline::BufferResult limitHigh = pipeline.renderToBuffer(input, atLimitHigh);
    QVERIFY2(allFinite(high.buffer), "+1e9 stops produced non-finite pixels");
    for (int i = 0; i < 4 * 4 * 4; ++i) {
        QVERIFY(high.buffer.data()[i] >= 0.0f && high.buffer.data()[i] <= 1.0f);
        // Clamping means an absurd value is indistinguishable from the limit.
        QCOMPARE(high.buffer.data()[i], limitHigh.buffer.data()[i]);
    }

    // The other end. Unclamped, 2^-1e9 is exactly zero and the whole frame
    // collapses to black; clamped to -10 stops it is merely very dark, which
    // is what makes this assertion able to fail.
    Look absurdLow;
    absurdLow.tone.exposure = -1.0e9f;
    Look atLimitLow;
    atLimitLow.tone.exposure = -10.0f;

    const ImagePipeline::BufferResult low = pipeline.renderToBuffer(input, absurdLow);
    const ImagePipeline::BufferResult limitLow = pipeline.renderToBuffer(input, atLimitLow);
    QVERIFY(allFinite(low.buffer));
    QVERIFY2(low.buffer.data()[4] > 0.0f,
             "-1e9 stops crushed the frame to zero: the exposure clamp did not run");
    for (int i = 0; i < 4 * 4 * 4; ++i)
        QCOMPARE(low.buffer.data()[i], limitLow.buffer.data()[i]);

    // Out-of-range values elsewhere in the Look must be equally harmless.
    Look wild;
    wild.tone.contrast           = 5000.0f;
    wild.tone.shadows            = -9999.0f;
    wild.color.saturation        = 12345.0f;
    wild.effects.vignette.amount = -8000.0f;
    wild.details.sharpeningAmount = 1.0e6f;
    wild.lens.enabled            = true;
    wild.lens.vignetting         = 1.0e6f;
    const ImagePipeline::BufferResult wildResult = pipeline.renderToBuffer(input, wild);
    QVERIFY(allFinite(wildResult.buffer));
    QVERIFY(!pipeline.render(input, wild).image.isNull());

    // The Look is taken by value so clampRanges() can run without editing the
    // caller's copy. Callers rely on that: a slider at 1e9 stays at 1e9 in the
    // UI model even after a render.
    QCOMPARE(absurdHigh.tone.exposure, 1.0e9f);
    QCOMPARE(wild.color.saturation, 12345.0f);
}

void TstImagePipeline::nanParametersDoNotCrash()
{
    // NaN reaches a Look the same way any other garbage does — a truncated or
    // hand-edited preset. util/ColorSpace.h already treats NaN as a case worth
    // handling at the encoders, so the pipeline must not fall over either.
    const ImagePipeline pipeline;
    const QImage input = flat8Bit(128, 4, 4);
    const float kNan = std::numeric_limits<float>::quiet_NaN();

    Look look;
    look.tone.exposure = kNan;

    // clampRanges() sanitises non-finite input, so a NaN exposure is replaced
    // by the neutral value and the render is a no-op rather than an edit. The
    // pixel therefore comes back unchanged.
    //
    // This previously asserted the opposite — that a NaN parameter silently
    // blackened the frame. That was the *symptom* of a real defect: NaN passed
    // through clampRanges() untouched and only looked survivable because both
    // encoders map NaN to zero. renderToBuffer() had no such accident and
    // handed NaN straight to the caller in its float buffer.
    //
    // Every pixel is checked rather than one: if a NaN ever reaches the tone
    // LUT index it produces per-pixel rubbish that a spot check would miss.
    const RenderResult rendered = pipeline.render(input, look);
    QVERIFY(!rendered.image.isNull());
    QCOMPARE(rendered.image.format(), QImage::Format_ARGB32);
    for (int y = 0; y < input.height(); ++y) {
        for (int x = 0; x < input.width(); ++x) {
            const QRgb px = rendered.image.pixel(x, y);
            QCOMPARE(qRed(px),   128);
            QCOMPARE(qGreen(px), 128);
            QCOMPARE(qBlue(px),  128);
            QCOMPARE(qAlpha(px), 255);
        }
    }

    // The buffer path is the one that had no accidental safety net, so assert
    // the whole thing is finite rather than only checking the encoded output.
    const ImagePipeline::BufferResult buffered = pipeline.renderToBuffer(input, look);
    QVERIFY(!buffered.buffer.isNull());
    QVERIFY2(allFinite(buffered.buffer),
             "NaN parameter leaked into the exported float buffer");

    // Infinity takes the same path through clampRanges() and must behave the
    // same way; it is a distinct IEEE class and a fix that handles only NaN
    // would pass the assertions above while still poisoning the buffer.
    Look infLook;
    infLook.tone.contrast = std::numeric_limits<float>::infinity();
    const ImagePipeline::BufferResult infBuffered = pipeline.renderToBuffer(input, infLook);
    QVERIFY(!infBuffered.buffer.isNull());
    QVERIFY2(allFinite(infBuffered.buffer),
             "infinite parameter leaked into the exported float buffer");
}

void TstImagePipeline::nullInputProducesNullResult()
{
    const ImagePipeline pipeline;
    const Look look = busyLook();

    const RenderResult rendered = pipeline.render(QImage(), look);
    QVERIFY(rendered.image.isNull());
    QVERIFY(!rendered.wasIdentity);
    QCOMPARE(rendered.elapsedMs, 0.0);

    // The null check has to come before clampRanges()/isIdentity(), so an
    // identity Look must not turn a null input into a non-null result either.
    QVERIFY(pipeline.render(QImage(), Look{}).image.isNull());
    QVERIFY(pipeline.renderUpTo(QImage(), look, ImagePipeline::Stage::AfterTone)
                .image.isNull());

    const ImagePipeline::BufferResult buffered = pipeline.renderToBuffer(QImage(), look);
    QVERIFY(buffered.buffer.isNull());
    QVERIFY(!buffered.wasIdentity);
}

// ==============================================================================
// Stage-granular rendering.
// ==============================================================================
void TstImagePipeline::renderUpToSeparatesStages()
{
    // One Look that gives every stage under test something real to do, so
    // "these two stages produced the same image" means the chain stopped in
    // the wrong place rather than that the slider was a no-op.
    Look look;
    look.lens.enabled     = true;
    look.lens.vignetting  = 50.0f;
    look.tone.exposure    = 0.7f;
    look.color.saturation = 40.0f;
    look.curves.master.points = { {0.0, 0.0}, {0.5, 0.35}, {1.0, 1.0} };
    look.grading.globalHue        = 30.0f;
    look.grading.globalSaturation = 60.0f;
    look.grading.globalStrength   = 50.0f;
    look.details.sharpeningAmount = 80.0f;
    look.effects.vignette.amount  = -60.0f;

    const ImagePipeline pipeline;
    QImage input(40, 40, QImage::Format_ARGB32);
    for (int y = 0; y < 40; ++y)
        for (int x = 0; x < 40; ++x)
            input.setPixel(x, y, qRgb((x * 6) % 256, (y * 6) % 256, 128));

    using Stage = ImagePipeline::Stage;
    const QImage atInput    = pipeline.renderUpTo(input, look, Stage::Input).image;
    const QImage afterLens  = pipeline.renderUpTo(input, look, Stage::AfterLens).image;
    const QImage afterXform = pipeline.renderUpTo(input, look, Stage::AfterTransform).image;
    const QImage afterTone  = pipeline.renderUpTo(input, look, Stage::AfterTone).image;
    const QImage afterColor = pipeline.renderUpTo(input, look, Stage::AfterColor).image;
    const QImage afterCurve = pipeline.renderUpTo(input, look, Stage::AfterCurve).image;
    const QImage afterGrade = pipeline.renderUpTo(input, look, Stage::AfterGrading).image;
    const QImage afterDetail= pipeline.renderUpTo(input, look, Stage::AfterDetails).image;
    const QImage afterFx    = pipeline.renderUpTo(input, look, Stage::AfterEffects).image;

    // Stage::Input is the untouched source copy, in the source's own format.
    QCOMPARE(atInput.format(), input.format());
    QCOMPARE(atInput, input);

    // Every stage whose engine is active must move the pixels.
    QVERIFY2(afterLens  != atInput,    "lens correction did nothing");
    QVERIFY2(afterTone  != afterLens,  "tone did nothing");
    QVERIFY2(afterColor != afterTone,  "color did nothing");
    QVERIFY2(afterCurve != afterColor, "curves did nothing");
    QVERIFY2(afterGrade != afterCurve, "grading did nothing");
    QVERIFY2(afterDetail!= afterGrade, "details did nothing");
    QVERIFY2(afterFx    != afterDetail,"effects did nothing");

    // The converse keeps the test honest: transform is left at its neutral
    // default, so AfterTransform must be byte-identical to AfterLens. If
    // stopping "one stage later" changed the image anyway, the stage switch
    // would be falling through and every assertion above would pass for the
    // wrong reason.
    QCOMPARE(afterXform, afterLens);

    // The last stage is what render() returns.
    QCOMPARE(afterFx, pipeline.render(input, look).image);
}

QTEST_MAIN(TstImagePipeline)
#include "tst_imagepipeline.moc"
