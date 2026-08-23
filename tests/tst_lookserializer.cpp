// ==============================================================================
// tests/tst_lookserializer.cpp
// The Look model and the .lxp preset serializer.
//
// Look *is* the edit — every slider, curve, mask and layer the user touched.
// LookSerializer is the only thing standing between that edit and the disk, so
// a regression here does not crash and does not show up in a screenshot: it
// quietly drops or corrupts fields, and the user finds out weeks later when
// they reopen a photo and their work is gone. That is the worst failure mode an
// editor has, so this file is deliberately paranoid.
//
// Four mechanisms, each covering a hole the others leave:
//
//   1. flattenLook() enumerates every field of the model *independently of the
//      serializer*, so the round trip is compared field by field rather than
//      JSON-blob to JSON-blob. (Comparing JSON to JSON would pass a serializer
//      that consistently wrote tone.highlights into "shadows" — both trips
//      would be wrong in the same way.)
//   2. Every field of the "everything set" Look gets a *distinct* value, so a
//      copy-paste bug that reads one field into its neighbour cannot hide
//      behind two equal numbers.
//   3. everySerializedKeyDiffersFromDefault() proves that Look actually differs
//      from a default one in every key the writer emits — without it, "it round
//      trips" would largely be a statement about zeros, and a field added to
//      both Look and the serializer but not to this test would go unchecked.
//   4. addingAFieldToLookMustReachThisFile() pins sizeof() for every struct in
//      the model. Look.h calls adding a field a three-file change (header,
//      Look.cpp, serializer); this file is the fourth, and the canary is what
//      makes someone notice.
//
// Anything reached from a file is treated as hostile: missing keys, wrong
// types, non-finite numbers, out-of-range values, malformed JSON, an array
// where an object belongs.
// ==============================================================================
// GCC 13 raises -Wuninitialized against the initializer_list backing array
// behind CurvePoints whenever it emits one of Look's implicit default
// constructors out of line: a known false positive around the std::vector
// initializer_list constructor (the array is a temporary the optimiser loses
// track of, not a real read of uninitialised memory). A test file constructs
// these types in far more places than production code does, which is why this
// file trips it and src/ does not. The pragma has to precede the include so
// that it covers the header the constructors are defined in.
#if defined(__GNUC__) && !defined(__clang__)
#  pragma GCC diagnostic ignored "-Wuninitialized"
#endif

#include "core/Look.h"
#include "preset/LookSerializer.h"

#include <QtTest>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

using namespace lps;

class TstLookSerializer : public QObject
{
    Q_OBJECT

private slots:
    // ---- Round trip ---------------------------------------------------------
    void defaultLookRoundTrips();
    void fullyPopulatedLookRoundTrips();
    void everySerializedKeyDiffersFromDefault();
    void addingAFieldToLookMustReachThisFile();
    void fileRoundTripMatchesInMemoryRoundTrip();

    // ---- Curves -------------------------------------------------------------
    void curveControlPointsSurviveRoundTrip();
    void curveWithManyPointsSurvivesRoundTrip();
    void emptyOrDegenerateCurveLoadsAsIdentity();
    void curveOrderAndBoundsAreNormalisedOnLoad();

    // ---- Hardening ----------------------------------------------------------
    void emptyObjectLoadsAsDefaultLook();
    void missingFieldsDefaultToIdentity();
    void olderFileWithoutNewerSectionsStillLoads();
    void unknownFieldsAreIgnored();
    void wrongTypedContainersAreIgnored();
    void wrongTypedLeavesAreIgnored();
    void nonFiniteValuesAreRejected();
    void nonFiniteValuesCannotBeWrittenToDisk();
    void hugeExponentsInAFileCannotProduceInfinities();
    void maskGeometryRejectsNonFiniteValues();
    void unknownEnumValuesFallBackToDefaults();
    void schemaVersionIsWrittenAndRead();
    void outOfRangeValuesAreClampedOnLoad();
    void malformedJsonFailsCleanly();
    void emptyFileFailsCleanly();
    void arrayRootFailsCleanly();
    void missingFileFailsCleanly();
    void unwritablePathFailsCleanly();

    // ---- The Look model itself ---------------------------------------------
    void defaultLookIsIdentity();
    void anySingleEditBreaksIdentity();
    void documentedNonEditsKeepIdentity();
    void resetReturnsToIdentity();
    void clampRangesEnforcesDocumentedBounds();
};

// ==============================================================================
// Test data: a Look with every field set to its own distinctive value
// ==============================================================================
namespace {

// Yields a fresh value inside [lo, hi] on every call.
//
// The fraction of the span is always an ODD multiple of 1/512, which is what
// makes this useful rather than merely convenient:
//   - an odd numerator over 512 can never reduce to a dyadic rational with a
//     smaller denominator, so a generated value never lands on 0, 1/4, 1/2 or
//     the whole span - i.e. it can never accidentally reproduce a struct
//     default (0, 0.5, 1.0, 25, 50, ...) and make the "differs from default"
//     check pass for the wrong reason;
//   - consecutive calls never repeat inside a 256-field window, which is wider
//     than any struct in the model, so a serializer that reads one field into
//     another is always caught by a value mismatch.
class Distinct
{
public:
    float in(float lo, float hi) { return lo + (hi - lo) * frac(); }

    // Curve control points are QPointF (double) from end to end - nothing in
    // the curve path narrows them to float. These values are deliberately a
    // hair off the float grid so that a future refactor which does narrow them
    // shows up as a round-trip failure instead of passing silently.
    double inDouble(double lo, double hi)
    {
        return lo + (hi - lo) * static_cast<double>(frac()) * (1.0 + 1e-12);
    }

private:
    float frac()
    {
        const int k = m_n++ % 256;
        return static_cast<float>(2 * k + 1) / 512.0f;
    }
    int m_n = 0;
};

HDRParams makeHdr(Distinct& g)
{
    HDRParams s;
    s.enabled              = true;                    // default false
    s.exposureBias         = g.in(-5.0f, 5.0f);
    s.highlightCompression = g.in(0.0f, 100.0f);
    s.shoulderStrength     = g.in(0.0f, 100.0f);
    s.midtonePivot         = g.in(0.05f, 1.0f);
    s.saturationPreserve   = g.in(0.0f, 100.0f);
    return s;
}

ToneParams makeTone(Distinct& g)
{
    ToneParams s;
    s.exposure   = g.in(-10.0f, 10.0f);
    s.contrast   = g.in(-100.0f, 100.0f);
    s.highlights = g.in(-100.0f, 100.0f);
    s.shadows    = g.in(-100.0f, 100.0f);
    s.whites     = g.in(-100.0f, 100.0f);
    s.blacks     = g.in(-100.0f, 100.0f);
    s.brightness = g.in(-100.0f, 100.0f);
    return s;
}

HSLChannel makeHslChannel(Distinct& g)
{
    HSLChannel s;
    s.hue        = g.in(-100.0f, 100.0f);
    s.saturation = g.in(-100.0f, 100.0f);
    s.luminance  = g.in(-100.0f, 100.0f);
    return s;
}

ColorParams makeColor(Distinct& g)
{
    ColorParams s;
    s.whiteBalance.temperature = g.in(-100.0f, 100.0f);
    s.whiteBalance.tint        = g.in(-100.0f, 100.0f);
    s.vibrance                 = g.in(-100.0f, 100.0f);
    s.saturation               = g.in(-100.0f, 100.0f);

    // All eight hue bands, each with its own three values - this is where a
    // loop-index bug in the loader (loading "orange" into "yellow") shows up.
    s.hsl.red     = makeHslChannel(g);
    s.hsl.orange  = makeHslChannel(g);
    s.hsl.yellow  = makeHslChannel(g);
    s.hsl.green   = makeHslChannel(g);
    s.hsl.aqua    = makeHslChannel(g);
    s.hsl.blue    = makeHslChannel(g);
    s.hsl.purple  = makeHslChannel(g);
    s.hsl.magenta = makeHslChannel(g);

    auto row = [&g]() {
        RGBMixerParams::Row r;
        r.r = g.in(-2.0f, 2.0f);
        r.g = g.in(-2.0f, 2.0f);
        r.b = g.in(-2.0f, 2.0f);
        return r;
    };
    s.rgbMixer.redOutput   = row();
    s.rgbMixer.greenOutput = row();
    s.rgbMixer.blueOutput  = row();
    return s;
}

// x is spread evenly and strictly increasing so CurvePoints::clampRanges()
// (which sorts, then pins the endpoints to x=0 / x=1) is a no-op on this data.
CurvePoints makeCurve(Distinct& g, int pointCount)
{
    CurvePoints s;
    s.points.clear();
    s.points.reserve(static_cast<size_t>(pointCount));
    for (int i = 0; i < pointCount; ++i) {
        const double x = static_cast<double>(i) / static_cast<double>(pointCount - 1);
        s.points.emplace_back(x, g.inDouble(0.0, 1.0));
    }
    return s;
}

CurveParams makeCurves(Distinct& g)
{
    CurveParams s;
    s.master = makeCurve(g, 9);
    s.red    = makeCurve(g, 2);
    s.green  = makeCurve(g, 3);
    s.blue   = makeCurve(g, 5);
    return s;
}

GradingParams makeGrading(Distinct& g, const QString& tag)
{
    GradingParams s;
    s.lutPath            = tag + "/luts/kodak-2383.cube";
    s.lutOpacity         = g.in(0.0f, 1.0f);
    s.lutEnabled         = false;                    // default true
    s.filmProfileId      = tag + ":portra-400";
    s.filmProfileOpacity = g.in(0.0f, 1.0f);

    // Hues live in [0, 360): clampRanges() wraps rather than clamps them, so
    // values inside the range must come back untouched.
    s.shadowsHue           = g.in(0.0f, 360.0f);
    s.shadowsSaturation    = g.in(0.0f, 100.0f);
    s.shadowsStrength      = g.in(0.0f, 100.0f);
    s.shadowsLuminance     = g.in(-100.0f, 100.0f);
    s.midtonesHue          = g.in(0.0f, 360.0f);
    s.midtonesSaturation   = g.in(0.0f, 100.0f);
    s.midtonesStrength     = g.in(0.0f, 100.0f);
    s.midtonesLuminance    = g.in(-100.0f, 100.0f);
    s.highlightsHue        = g.in(0.0f, 360.0f);
    s.highlightsSaturation = g.in(0.0f, 100.0f);
    s.highlightsStrength   = g.in(0.0f, 100.0f);
    s.highlightsLuminance  = g.in(-100.0f, 100.0f);
    s.globalHue            = g.in(0.0f, 360.0f);
    s.globalSaturation     = g.in(0.0f, 100.0f);
    s.globalStrength       = g.in(0.0f, 100.0f);
    s.globalLuminance      = g.in(-100.0f, 100.0f);
    s.balance              = g.in(-100.0f, 100.0f);
    s.blending             = g.in(0.0f, 100.0f);

    // Lift/gamma/gain/offset and the filmic block have no engine yet. They
    // must still persist: that is the entire promise made to users who author
    // with them today, so they are tested exactly like everything else.
    s.lift             = g.in(-100.0f, 100.0f);
    s.gamma            = g.in(-100.0f, 100.0f);
    s.gain             = g.in(-100.0f, 100.0f);
    s.offset           = g.in(-100.0f, 100.0f);
    s.filmicContrast   = g.in(-100.0f, 100.0f);
    s.highlightRolloff = g.in(-100.0f, 100.0f);
    s.shadowLift       = g.in(-100.0f, 100.0f);
    s.fadeBlacks       = g.in(-100.0f, 100.0f);
    s.colorSeparation  = g.in(-100.0f, 100.0f);
    return s;
}

DetailsParams makeDetails(Distinct& g)
{
    DetailsParams s;
    s.sharpeningAmount  = g.in(0.0f, 150.0f);
    s.sharpeningRadius  = g.in(0.5f, 3.0f);
    s.sharpeningDetail  = g.in(0.0f, 100.0f);
    s.sharpeningMasking = g.in(0.0f, 100.0f);
    s.luminanceNR       = g.in(0.0f, 100.0f);
    s.luminanceDetail   = g.in(0.0f, 100.0f);
    s.colorNR           = g.in(0.0f, 100.0f);
    s.colorDetail       = g.in(0.0f, 100.0f);
    return s;
}

EffectsParams makeEffects(Distinct& g)
{
    EffectsParams s;
    s.vignette.amount    = g.in(-100.0f, 100.0f);
    s.vignette.midpoint  = g.in(0.0f, 100.0f);
    s.vignette.feather   = g.in(0.0f, 100.0f);
    s.vignette.roundness = g.in(-100.0f, 100.0f);
    s.grain.amount       = g.in(0.0f, 100.0f);
    s.grain.size         = g.in(0.0f, 100.0f);
    s.clarity.amount     = g.in(-100.0f, 100.0f);
    return s;
}

LensParams makeLens(Distinct& g)
{
    LensParams s;
    s.enabled                   = true;              // default false
    s.removeChromaticAberration = true;              // default false
    s.distortion                = g.in(-100.0f, 100.0f);
    s.vignetting                = g.in(-100.0f, 100.0f);
    s.purpleFringe              = g.in(0.0f, 100.0f);
    s.greenFringe               = g.in(0.0f, 100.0f);
    return s;
}

TransformParams makeTransform(Distinct& g)
{
    TransformParams s;
    // Inside (-180, +180] so the normalisation in clampRanges() is a no-op.
    s.rotationDegrees = g.in(-179.0f, 179.0f);
    s.flipHorizontal  = true;                        // default false
    s.flipVertical    = true;                        // default false
    s.straightenAngle = g.in(-10.0f, 10.0f);
    // Dyadic fractions: cropRect is stored as double but reloaded through a
    // float, so only float-exact values can round-trip bit for bit.
    s.cropRect        = QRectF(0.125, 0.0625, 0.5, 0.75);
    return s;
}

BrushStroke makeStroke(Distinct& g, int pointCount)
{
    BrushStroke s;
    s.size    = g.in(0.01f, 1.0f);
    s.feather = g.in(0.0f, 1.0f);
    s.flow    = g.in(0.0f, 1.0f);
    s.density = g.in(0.0f, 1.0f);
    s.erase   = true;                                // default false
    for (int i = 0; i < pointCount; ++i)
        s.points.append(QPointF(g.in(-1.0f, 1.0f), g.in(-1.0f, 1.0f)));
    return s;
}

LocalAdjustment makeMask(Distinct& g, MaskType type, const QString& name, bool enabled)
{
    LocalAdjustment s;
    s.name           = name;
    s.enabled        = enabled;
    s.type           = type;
    s.startPoint     = QPointF(g.in(-1.0f, 1.0f), g.in(-1.0f, 1.0f));
    s.endPoint       = QPointF(g.in(-1.0f, 1.0f), g.in(-1.0f, 1.0f));
    s.center         = QPointF(g.in(-1.0f, 1.0f), g.in(-1.0f, 1.0f));
    s.radius         = g.in(0.01f, 9.0f);
    s.feather        = g.in(0.0f, 1.0f);
    s.invert         = true;                         // default false
    s.density        = g.in(0.0f, 1.0f);
    s.flow           = g.in(0.0f, 1.0f);
    s.brushSize      = g.in(0.01f, 1.0f);
    s.brushEraseMode = true;                         // default false
    s.brushStrokes.append(makeStroke(g, 3));
    s.brushStrokes.append(makeStroke(g, 1));
    s.exposure    = g.in(-10.0f, 10.0f);
    s.brightness  = g.in(-100.0f, 100.0f);
    s.contrast    = g.in(-100.0f, 100.0f);
    s.saturation  = g.in(-100.0f, 100.0f);
    s.temperature = g.in(-100.0f, 100.0f);
    s.tint        = g.in(-100.0f, 100.0f);
    return s;
}

AdjustmentLayer makeLayer(Distinct& g, const QString& name, bool enabled,
                          BlendMode blendMode, const QString& tag)
{
    AdjustmentLayer s;
    s.name      = name;
    s.enabled   = enabled;
    s.opacity   = g.in(0.0f, 1.0f);
    s.blendMode = blendMode;
    s.maskRef   = tag + "-mask-ref";
    s.adjustmentData.tone    = makeTone(g);
    s.adjustmentData.color   = makeColor(g);
    s.adjustmentData.curves  = makeCurves(g);
    s.adjustmentData.grading = makeGrading(g, tag);
    s.adjustmentData.effects = makeEffects(g);
    return s;
}

// Every field of Look, non-default, all values distinct. Nothing here is
// pre-clamped on purpose: the round-trip test asserts that clampRanges()
// leaves this data alone, which is what makes "loaded == original" an honest
// comparison instead of a comparison between two clamped things.
Look makeFullyPopulatedLook()
{
    Distinct g;
    Look look;
    look.name          = QStringLiteral("Every field set - handle with care");
    look.schemaVersion = 1;
    look.hdr       = makeHdr(g);
    look.tone      = makeTone(g);
    look.color     = makeColor(g);
    look.curves    = makeCurves(g);
    look.grading   = makeGrading(g, QStringLiteral("base"));
    look.details   = makeDetails(g);
    look.effects   = makeEffects(g);
    look.lens      = makeLens(g);
    look.transform = makeTransform(g);
    // Mask 0 is disabled and mask 1 enabled: `enabled` defaults to true, so
    // mask 0 has to be false for the differs-from-default check to mean
    // anything, and mask 1 keeps the common (enabled) path covered.
    look.localAdjustments.push_back(
        makeMask(g, MaskType::Brush, QStringLiteral("Sky"), false));
    look.localAdjustments.push_back(
        makeMask(g, MaskType::RadialGradient, QStringLiteral("Face"), true));
    look.adjustmentLayers.push_back(
        makeLayer(g, QStringLiteral("Base grade"), false, BlendMode::Overlay,
                  QStringLiteral("L0")));
    look.adjustmentLayers.push_back(
        makeLayer(g, QStringLiteral("Bloom"), true, BlendMode::Difference,
                  QStringLiteral("L1")));
    return look;
}

// ==============================================================================
// Flattening
//
// One enumeration of the model, written independently of the serializer, used
// for round-trip comparison, finiteness checks and clamping checks. Keeping it
// separate from LookSerializer is the point: if the round trip were checked by
// comparing toJson(a) with toJson(b), a writer that consistently emitted the
// wrong source field would produce two identically-wrong documents and pass.
// ==============================================================================
QVariant toVar(float v)  { return QVariant(static_cast<double>(v)); }
QVariant toVar(double v) { return QVariant(v); }
QVariant toVar(bool v)   { return QVariant(v); }
QVariant toVar(int v)    { return QVariant(v); }
QVariant toVar(const QString& v) { return QVariant(v); }

// Zero-padded so the sorted key order of the map is also the natural order,
// which makes a failure diff readable.
QString idx(const QString& p, size_t i)
{
    return p + QString("[%1]").arg(static_cast<int>(i), 3, 10, QLatin1Char('0'));
}

#define PUT(f) m[p + "." #f] = toVar(s.f)

void flattenHdr(QVariantMap& m, const QString& p, const HDRParams& s)
{
    PUT(enabled); PUT(exposureBias); PUT(highlightCompression);
    PUT(shoulderStrength); PUT(midtonePivot); PUT(saturationPreserve);
}

void flattenTone(QVariantMap& m, const QString& p, const ToneParams& s)
{
    PUT(exposure); PUT(contrast); PUT(highlights); PUT(shadows);
    PUT(whites); PUT(blacks); PUT(brightness);
}

void flattenHslChannel(QVariantMap& m, const QString& p, const HSLChannel& s)
{
    PUT(hue); PUT(saturation); PUT(luminance);
}

void flattenMixerRow(QVariantMap& m, const QString& p, const RGBMixerParams::Row& s)
{
    PUT(r); PUT(g); PUT(b);
}

void flattenColor(QVariantMap& m, const QString& p, const ColorParams& s)
{
    m[p + ".whiteBalance.temperature"] = toVar(s.whiteBalance.temperature);
    m[p + ".whiteBalance.tint"]        = toVar(s.whiteBalance.tint);
    PUT(vibrance); PUT(saturation);
    flattenHslChannel(m, p + ".hsl.red",     s.hsl.red);
    flattenHslChannel(m, p + ".hsl.orange",  s.hsl.orange);
    flattenHslChannel(m, p + ".hsl.yellow",  s.hsl.yellow);
    flattenHslChannel(m, p + ".hsl.green",   s.hsl.green);
    flattenHslChannel(m, p + ".hsl.aqua",    s.hsl.aqua);
    flattenHslChannel(m, p + ".hsl.blue",    s.hsl.blue);
    flattenHslChannel(m, p + ".hsl.purple",  s.hsl.purple);
    flattenHslChannel(m, p + ".hsl.magenta", s.hsl.magenta);
    flattenMixerRow(m, p + ".rgbMixer.redOutput",   s.rgbMixer.redOutput);
    flattenMixerRow(m, p + ".rgbMixer.greenOutput", s.rgbMixer.greenOutput);
    flattenMixerRow(m, p + ".rgbMixer.blueOutput",  s.rgbMixer.blueOutput);
}

// The point count is part of the value: a curve is a list, and dropping or
// duplicating a control point is exactly the kind of damage that would
// otherwise hide behind a per-point comparison of the points that remain.
void flattenCurve(QVariantMap& m, const QString& p, const CurvePoints& s)
{
    m[p + ".count"] = toVar(static_cast<int>(s.points.size()));
    for (size_t i = 0; i < s.points.size(); ++i) {
        m[idx(p, i) + ".x"] = toVar(s.points[i].x());
        m[idx(p, i) + ".y"] = toVar(s.points[i].y());
    }
}

void flattenCurves(QVariantMap& m, const QString& p, const CurveParams& s)
{
    flattenCurve(m, p + ".master", s.master);
    flattenCurve(m, p + ".red",    s.red);
    flattenCurve(m, p + ".green",  s.green);
    flattenCurve(m, p + ".blue",   s.blue);
}

void flattenGrading(QVariantMap& m, const QString& p, const GradingParams& s)
{
    PUT(lutPath); PUT(lutOpacity); PUT(lutEnabled);
    PUT(filmProfileId); PUT(filmProfileOpacity);
    PUT(shadowsHue);    PUT(shadowsSaturation);    PUT(shadowsStrength);    PUT(shadowsLuminance);
    PUT(midtonesHue);   PUT(midtonesSaturation);   PUT(midtonesStrength);   PUT(midtonesLuminance);
    PUT(highlightsHue); PUT(highlightsSaturation); PUT(highlightsStrength); PUT(highlightsLuminance);
    PUT(globalHue);     PUT(globalSaturation);     PUT(globalStrength);     PUT(globalLuminance);
    PUT(balance); PUT(blending);
    PUT(lift); PUT(gamma); PUT(gain); PUT(offset);
    PUT(filmicContrast); PUT(highlightRolloff); PUT(shadowLift);
    PUT(fadeBlacks); PUT(colorSeparation);
}

void flattenDetails(QVariantMap& m, const QString& p, const DetailsParams& s)
{
    PUT(sharpeningAmount); PUT(sharpeningRadius); PUT(sharpeningDetail);
    PUT(sharpeningMasking); PUT(luminanceNR); PUT(luminanceDetail);
    PUT(colorNR); PUT(colorDetail);
}

void flattenEffects(QVariantMap& m, const QString& p, const EffectsParams& s)
{
    m[p + ".vignette.amount"]    = toVar(s.vignette.amount);
    m[p + ".vignette.midpoint"]  = toVar(s.vignette.midpoint);
    m[p + ".vignette.feather"]   = toVar(s.vignette.feather);
    m[p + ".vignette.roundness"] = toVar(s.vignette.roundness);
    m[p + ".grain.amount"]       = toVar(s.grain.amount);
    m[p + ".grain.size"]         = toVar(s.grain.size);
    m[p + ".clarity.amount"]     = toVar(s.clarity.amount);
}

void flattenLens(QVariantMap& m, const QString& p, const LensParams& s)
{
    PUT(enabled); PUT(removeChromaticAberration); PUT(distortion);
    PUT(vignetting); PUT(purpleFringe); PUT(greenFringe);
}

void flattenTransform(QVariantMap& m, const QString& p, const TransformParams& s)
{
    PUT(rotationDegrees); PUT(flipHorizontal); PUT(flipVertical);
    PUT(straightenAngle);
    m[p + ".cropRect.x"]      = toVar(s.cropRect.x());
    m[p + ".cropRect.y"]      = toVar(s.cropRect.y());
    m[p + ".cropRect.width"]  = toVar(s.cropRect.width());
    m[p + ".cropRect.height"] = toVar(s.cropRect.height());
}

void flattenStroke(QVariantMap& m, const QString& p, const BrushStroke& s)
{
    PUT(size); PUT(feather); PUT(flow); PUT(density); PUT(erase);
    m[p + ".points.count"] = toVar(static_cast<int>(s.points.size()));
    for (qsizetype i = 0; i < s.points.size(); ++i) {
        const QString ip = idx(p + ".points", static_cast<size_t>(i));
        m[ip + ".x"] = toVar(s.points[i].x());
        m[ip + ".y"] = toVar(s.points[i].y());
    }
}

void flattenMask(QVariantMap& m, const QString& p, const LocalAdjustment& s)
{
    PUT(name); PUT(enabled);
    m[p + ".type"] = toVar(static_cast<int>(s.type));
    m[p + ".startPoint.x"] = toVar(s.startPoint.x());
    m[p + ".startPoint.y"] = toVar(s.startPoint.y());
    m[p + ".endPoint.x"]   = toVar(s.endPoint.x());
    m[p + ".endPoint.y"]   = toVar(s.endPoint.y());
    m[p + ".center.x"]     = toVar(s.center.x());
    m[p + ".center.y"]     = toVar(s.center.y());
    PUT(radius); PUT(feather); PUT(invert); PUT(density); PUT(flow);
    PUT(brushSize); PUT(brushEraseMode);
    m[p + ".brushStrokes.count"] = toVar(static_cast<int>(s.brushStrokes.size()));
    for (qsizetype i = 0; i < s.brushStrokes.size(); ++i)
        flattenStroke(m, idx(p + ".brushStrokes", static_cast<size_t>(i)), s.brushStrokes[i]);
    PUT(exposure); PUT(brightness); PUT(contrast); PUT(saturation);
    PUT(temperature); PUT(tint);
}

void flattenPayload(QVariantMap& m, const QString& p, const LayerAdjustmentData& s)
{
    flattenTone(m, p + ".tone", s.tone);
    flattenColor(m, p + ".color", s.color);
    flattenCurves(m, p + ".curves", s.curves);
    flattenGrading(m, p + ".grading", s.grading);
    flattenEffects(m, p + ".effects", s.effects);
}

void flattenLayer(QVariantMap& m, const QString& p, const AdjustmentLayer& s)
{
    PUT(name); PUT(enabled); PUT(opacity); PUT(maskRef);
    m[p + ".blendMode"] = toVar(static_cast<int>(s.blendMode));
    flattenPayload(m, p + ".adjustmentData", s.adjustmentData);
}

QVariantMap flattenLook(const Look& s)
{
    QVariantMap m;
    const QString p = QStringLiteral("look");
    PUT(name); PUT(schemaVersion);
    flattenHdr(m, p + ".hdr", s.hdr);
    flattenTone(m, p + ".tone", s.tone);
    flattenColor(m, p + ".color", s.color);
    flattenCurves(m, p + ".curves", s.curves);
    flattenGrading(m, p + ".grading", s.grading);
    flattenDetails(m, p + ".details", s.details);
    flattenEffects(m, p + ".effects", s.effects);
    flattenLens(m, p + ".lens", s.lens);
    flattenTransform(m, p + ".transform", s.transform);
    m[p + ".localAdjustments.count"] = toVar(static_cast<int>(s.localAdjustments.size()));
    for (size_t i = 0; i < s.localAdjustments.size(); ++i)
        flattenMask(m, idx(p + ".localAdjustments", i), s.localAdjustments[i]);
    m[p + ".adjustmentLayers.count"] = toVar(static_cast<int>(s.adjustmentLayers.size()));
    for (size_t i = 0; i < s.adjustmentLayers.size(); ++i)
        flattenLayer(m, idx(p + ".adjustmentLayers", i), s.adjustmentLayers[i]);
    return m;
}

#undef PUT

// 17 significant digits round-trips an IEEE-754 double exactly, so comparing
// the rendered text is an exact comparison, not a fuzzy one. QVariant::operator==
// is deliberately avoided here: it compares floating point values fuzzily, and
// a serializer that lost the last few bits of a control point would slip past.
QString describe(const QVariant& v)
{
    if (v.metaType().id() == QMetaType::Double)
        return QString::number(v.toDouble(), 'g', 17);
    if (v.metaType().id() == QMetaType::Bool)
        return v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    return v.toString();
}

// Field-by-field difference between two Looks. Returns one line per mismatch
// so a failure names every damaged field at once rather than the first.
QStringList diffLooks(const Look& actual, const Look& expected)
{
    const QVariantMap a = flattenLook(actual);
    const QVariantMap b = flattenLook(expected);
    QStringList out;
    for (auto it = b.constBegin(); it != b.constEnd(); ++it) {
        if (!a.contains(it.key())) {
            out << QString("%1: missing from actual").arg(it.key());
            continue;
        }
        const QString got = describe(a.value(it.key()));
        const QString want = describe(it.value());
        if (got != want)
            out << QString("%1: got %2, expected %3").arg(it.key(), got, want);
    }
    for (auto it = a.constBegin(); it != a.constEnd(); ++it) {
        if (!b.contains(it.key()))
            out << QString("%1: unexpected in actual").arg(it.key());
    }
    return out;
}

// Every non-finite float anywhere in the model. NaN or infinity reaching the
// pipeline poisons every pixel it touches, and it does so silently.
QStringList nonFiniteFields(const Look& look)
{
    const QVariantMap m = flattenLook(look);
    QStringList out;
    for (auto it = m.constBegin(); it != m.constEnd(); ++it) {
        if (it.value().metaType().id() == QMetaType::Double
            && !std::isfinite(it.value().toDouble())) {
            out << QString("%1 = %2").arg(it.key(), describe(it.value()));
        }
    }
    return out;
}

// ---- small JSON pokers used by the hardening tests --------------------------
QJsonObject poke(QJsonObject root, const QString& section, const QString& key,
                 const QJsonValue& value)
{
    QJsonObject o = root.value(section).toObject();
    o[key] = value;
    root[section] = o;
    return root;
}

QJsonObject pokeElement(QJsonObject root, const QString& arrayKey, int index,
                        const QString& key, const QJsonValue& value)
{
    QJsonArray arr = root.value(arrayKey).toArray();
    QJsonObject o = arr.at(index).toObject();
    o[key] = value;
    arr.replace(index, o);
    root[arrayKey] = arr;
    return root;
}

// ==============================================================================
// "Did the test data actually exercise this key?"
//
// Walks the document written for a default Look next to the one written for the
// fully-populated Look and reports every leaf that came out identical. A leaf
// that matches the default means the populated Look never set it, so the round
// trip is not proving anything about that key. This is what catches a field
// added to Look AND to the serializer but never to this file.
// ==============================================================================
bool pathIsExpectedToMatchDefault(const QString& path)
{
    // Stamped by the writer, identical by construction.
    if (path == "schemaVersion" || path.endsWith(".schemaVersion"))
        return true;
    // A layer payload is bridged through a temporary Look, so the sub-trees a
    // layer does not carry are written out at their defaults every time. This
    // is dead weight in the format rather than a bug, but it is real, and
    // pinning it here means a future change to the bridge shows up.
    static const char* const deadEnds[] = {
        ".adjustmentData.name",
        ".adjustmentData.hdr",
        ".adjustmentData.details",
        ".adjustmentData.lens",
        ".adjustmentData.transform",
        ".adjustmentData.localAdjustments",
        ".adjustmentData.adjustmentLayers",
    };
    for (const char* dead : deadEnds) {
        if (path.contains(QLatin1String(dead)))
            return true;
    }
    return false;
}

void collectLeavesEqualToDefault(const QJsonValue& base, const QJsonValue& full,
                                 const QString& path, QStringList& out)
{
    if (pathIsExpectedToMatchDefault(path))
        return;

    if (base.isObject() && full.isObject()) {
        const QJsonObject b = base.toObject();
        const QJsonObject f = full.toObject();
        for (auto it = b.constBegin(); it != b.constEnd(); ++it) {
            const QString child = path.isEmpty() ? it.key() : path + "." + it.key();
            if (!f.contains(it.key())) {
                out << child + " (written for a default Look but not for a populated one)";
                continue;
            }
            collectLeavesEqualToDefault(it.value(), f.value(it.key()), child, out);
        }
        return;
    }

    if (base.isArray() && full.isArray()) {
        const QJsonArray b = base.toArray();
        const QJsonArray f = full.toArray();
        // Arrays of objects (masks, layers, brush strokes) are walked element
        // by element so their individual fields are covered. Arrays of numbers
        // (curve points, mixer rows, [x, y] pairs) are compared whole: a curve
        // legitimately shares its endpoint x values with the identity curve.
        if (!b.isEmpty() && b.at(0).isObject()) {
            const qsizetype n = std::min(b.size(), f.size());
            for (qsizetype i = 0; i < n; ++i) {
                collectLeavesEqualToDefault(b.at(i), f.at(i),
                                            path + QString("[%1]").arg(i), out);
            }
            return;
        }
        if (b == f)
            out << path + " (same as default)";
        return;
    }

    if (base == full)
        out << path + " (same as default: " + describe(full.toVariant()) + ")";
}

} // namespace

// ==============================================================================
// Round trip
// ==============================================================================
void TstLookSerializer::defaultLookRoundTrips()
{
    const Look original;
    Look loaded;
    QString err;
    QVERIFY2(LookSerializer::fromJson(LookSerializer::toJson(original), loaded, &err),
             qPrintable(err));

    const QStringList d = diffLooks(loaded, original);
    QVERIFY2(d.isEmpty(), qPrintable(d.join("\n")));
    QVERIFY(loaded.isIdentity());
    QVERIFY(err.isEmpty());
}

void TstLookSerializer::fullyPopulatedLookRoundTrips()
{
    const Look original = makeFullyPopulatedLook();
    QVERIFY(!original.isIdentity());

    // Self-check first: the loader clamps every preset it reads, so comparing
    // against the original is only honest if the original is already inside
    // its documented ranges. If this fires, the test data is wrong, not the
    // serializer.
    Look clamped = original;
    clamped.clampRanges();
    const QStringList clampDiff = diffLooks(clamped, original);
    QVERIFY2(clampDiff.isEmpty(),
             qPrintable(QString("test data is out of range:\n%1").arg(clampDiff.join("\n"))));

    Look loaded;
    QString err;
    QVERIFY2(LookSerializer::fromJson(LookSerializer::toJson(original), loaded, &err),
             qPrintable(err));

    const QStringList d = diffLooks(loaded, original);
    QVERIFY2(d.isEmpty(),
             qPrintable(QString("%1 field(s) did not survive the round trip:\n%2")
                            .arg(d.size()).arg(d.join("\n"))));
}

void TstLookSerializer::everySerializedKeyDiffersFromDefault()
{
    // The baseline is a default Look that nonetheless carries one mask, one
    // brush stroke and one layer, so that the fields inside those array
    // elements get compared at all.
    Look baseline;
    LocalAdjustment defaultMask;
    BrushStroke defaultStroke;
    defaultStroke.points.append(QPointF(0.0, 0.0));
    defaultMask.brushStrokes.append(defaultStroke);
    baseline.localAdjustments.push_back(defaultMask);
    const AdjustmentLayer defaultLayer;
    baseline.adjustmentLayers.push_back(defaultLayer);

    QStringList same;
    collectLeavesEqualToDefault(LookSerializer::toJson(baseline),
                                LookSerializer::toJson(makeFullyPopulatedLook()),
                                QString(), same);

    QVERIFY2(same.isEmpty(),
             qPrintable(QString("%1 key(s) are written identically for a default Look and "
                                "for the fully-populated one, so the round-trip test proves "
                                "nothing about them. Set them in makeFullyPopulatedLook():\n%2")
                            .arg(same.size()).arg(same.join("\n"))));
}

void TstLookSerializer::addingAFieldToLookMustReachThisFile()
{
    // Look.h says adding a field is a three-file change: the header, isIdentity
    // in Look.cpp, and the serializer. This file is the fourth, and it is the
    // only one that fails loudly when the fourth is skipped -- a field added to
    // the model but not to LookSerializer round-trips as its default, and every
    // other assertion here still passes.
    //
    // So pin the size of every struct in the model. Adding, removing or
    // retyping a field moves one of these numbers.
    struct Entry { const char* name; size_t actual; size_t expected; };
    const Entry table[] = {
        { "HDRParams",           sizeof(HDRParams),             24 },
        { "ToneParams",          sizeof(ToneParams),            28 },
        { "WhiteBalanceParams",  sizeof(WhiteBalanceParams),     8 },
        { "HSLChannel",          sizeof(HSLChannel),            12 },
        { "HSLParams",           sizeof(HSLParams),             96 },
        { "RGBMixerParams::Row", sizeof(RGBMixerParams::Row),   12 },
        { "RGBMixerParams",      sizeof(RGBMixerParams),        36 },
        { "ColorParams",         sizeof(ColorParams),          148 },
        { "CurvePoints",         sizeof(CurvePoints),           24 },
        { "CurveParams",         sizeof(CurveParams),           96 },
        { "GradingParams",       sizeof(GradingParams),        168 },
        { "DetailsParams",       sizeof(DetailsParams),         32 },
        { "TransformParams",     sizeof(TransformParams),       48 },
        { "VignetteParams",      sizeof(VignetteParams),        16 },
        { "GrainParams",         sizeof(GrainParams),            8 },
        { "ClarityParams",       sizeof(ClarityParams),          4 },
        { "EffectsParams",       sizeof(EffectsParams),         28 },
        { "LensParams",          sizeof(LensParams),            20 },
        { "BrushStroke",         sizeof(BrushStroke),           48 },
        { "LocalAdjustment",     sizeof(LocalAdjustment),      160 },
        { "LayerAdjustmentData", sizeof(LayerAdjustmentData),  472 },
        { "AdjustmentLayer",     sizeof(AdjustmentLayer),      536 },
        { "Look",                sizeof(Look),                 672 },
    };

    QStringList wrong;
    for (const Entry& e : table) {
        if (e.actual != e.expected) {
            wrong << QString("  %1: sizeof is %2, this table says %3")
                         .arg(QLatin1String(e.name))
                         .arg(e.actual).arg(e.expected);
        }
    }
    QVERIFY2(wrong.isEmpty(),
             qPrintable(QString("The Look model changed shape.\n%1\n"
                                "If a field was added or removed: serialize it in "
                                "LookSerializer (both directions), account for it in "
                                "isIdentity/clampRanges, then add it to "
                                "makeFullyPopulatedLook() and flattenLook() here and "
                                "update the number above. If only the compiler or ABI "
                                "changed, just update the number.")
                            .arg(wrong.join("\n"))));
}

void TstLookSerializer::fileRoundTripMatchesInMemoryRoundTrip()
{
    QTemporaryDir dir;
    QVERIFY2(dir.isValid(), qPrintable(dir.errorString()));
    const QString path = dir.filePath("everything.lxp");

    const Look original = makeFullyPopulatedLook();
    const SaveResult saved = LookSerializer::saveToFile(original, path);
    QVERIFY2(saved.ok, qPrintable(saved.errorMessage));
    QVERIFY(saved.errorMessage.isEmpty());
    QVERIFY(QFileInfo::exists(path));

    const LoadResult loaded = LookSerializer::loadFromFile(path);
    QVERIFY2(loaded.ok, qPrintable(loaded.errorMessage));

    const QStringList d = diffLooks(loaded.look, original);
    QVERIFY2(d.isEmpty(),
             qPrintable(QString("%1 field(s) did not survive save/load:\n%2")
                            .arg(d.size()).arg(d.join("\n"))));

    // Going through the file must be indistinguishable from staying in memory.
    Look inMemory;
    QVERIFY(LookSerializer::fromJson(LookSerializer::toJson(original), inMemory));
    const QStringList d2 = diffLooks(loaded.look, inMemory);
    QVERIFY2(d2.isEmpty(), qPrintable(d2.join("\n")));
}

// ==============================================================================
// Curves
//
// Every other parameter is a scalar. A curve is a list, which means it can be
// damaged in ways a scalar cannot: points dropped, duplicated, reordered, or
// silently replaced by the identity ramp. All of those look like "the file
// loaded fine".
// ==============================================================================
void TstLookSerializer::curveControlPointsSurviveRoundTrip()
{
    Look original;
    original.curves.master.points = {{0.0, 0.0}, {0.2, 0.35}, {0.5, 0.5}, {0.75, 0.9}, {1.0, 1.0}};
    original.curves.red.points    = {{0.0, 0.1}, {1.0, 0.9}};
    original.curves.green.points  = {{0.0, 0.0}, {0.4, 0.6}, {1.0, 1.0}};
    original.curves.blue.points   = {{0.0, 0.05}, {0.25, 0.2}, {0.5, 0.55}, {1.0, 0.95}};

    Look loaded;
    QVERIFY(LookSerializer::fromJson(LookSerializer::toJson(original), loaded));

    QCOMPARE(loaded.curves.master.points.size(), size_t(5));
    QCOMPARE(loaded.curves.red.points.size(),    size_t(2));
    QCOMPARE(loaded.curves.green.points.size(),  size_t(3));
    QCOMPARE(loaded.curves.blue.points.size(),   size_t(4));

    // Exact values, in order, per channel.
    const QStringList d = diffLooks(loaded, original);
    QVERIFY2(d.isEmpty(), qPrintable(d.join("\n")));

    // A per-channel curve must not leak into another channel.
    QVERIFY(loaded.curves.red.points != loaded.curves.green.points);
}

void TstLookSerializer::curveWithManyPointsSurvivesRoundTrip()
{
    // 64 points is well past any UI limit, which is the point: a fixed-size
    // buffer or an off-by-one in the writer shows up here and nowhere else.
    Look original;
    original.curves.master.points.clear();
    constexpr int kCount = 64;
    for (int i = 0; i < kCount; ++i) {
        const double x = static_cast<double>(i) / (kCount - 1);
        original.curves.master.points.emplace_back(x, 1.0 - x * x);
    }

    Look loaded;
    QVERIFY(LookSerializer::fromJson(LookSerializer::toJson(original), loaded));
    QCOMPARE(loaded.curves.master.points.size(), size_t(kCount));

    const QStringList d = diffLooks(loaded, original);
    QVERIFY2(d.isEmpty(), qPrintable(d.join("\n")));

    // Ordering is load-bearing for interpolation: x must stay ascending.
    for (size_t i = 1; i < loaded.curves.master.points.size(); ++i) {
        QVERIFY2(loaded.curves.master.points[i].x() > loaded.curves.master.points[i - 1].x(),
                 qPrintable(QString("point %1 is out of order").arg(i)));
    }
}

void TstLookSerializer::emptyOrDegenerateCurveLoadsAsIdentity()
{
    // A curve needs at least two points to be evaluable. The loader refuses to
    // install anything less and falls back to the identity ramp, which is the
    // only safe choice: a one-point or malformed curve would either crash the
    // interpolator or black out the image.
    struct Case { const char* what; QJsonValue master; };
    const Case cases[] = {
        { "empty array",          QJsonArray{} },
        { "single point",         QJsonArray{ QJsonArray{0.5, 0.5} } },
        { "non-numeric point",    QJsonArray{ QJsonArray{0.0, 0.0}, QJsonArray{QString("x"), 1.0} } },
        { "three-element point",  QJsonArray{ QJsonArray{0.0, 0.0, 0.0}, QJsonArray{1.0, 1.0, 1.0} } },
        { "point that is not an array", QJsonArray{ 0.5, 0.5 } },
        { "string instead of a curve",  QJsonValue(QString("nope")) },
    };

    for (const Case& c : cases) {
        QJsonObject curves;
        curves["master"] = c.master;
        QJsonObject root;
        root["curves"] = curves;

        Look loaded;
        QVERIFY2(LookSerializer::fromJson(root, loaded), c.what);
        QVERIFY2(loaded.curves.master.isIdentity(), c.what);
        QCOMPARE(loaded.curves.master.points.size(), size_t(2));
        QVERIFY2(loaded.isIdentity(), c.what);
    }

    // The same is true from the other direction: a Look holding an empty curve
    // is written out as an empty array and comes back as the identity ramp.
    Look original;
    original.curves.master.points.clear();
    Look loaded;
    QVERIFY(LookSerializer::fromJson(LookSerializer::toJson(original), loaded));
    QVERIFY(loaded.curves.master.isIdentity());
}

void TstLookSerializer::curveOrderAndBoundsAreNormalisedOnLoad()
{
    // A hand-edited preset can contain unsorted, out-of-range points.
    // clampRanges() runs on load, so what reaches the engine is sorted, inside
    // [0,1] on both axes, and anchored at x=0 / x=1.
    QJsonObject curves;
    curves["master"] = QJsonArray{ QJsonArray{0.9, 2.0},
                                   QJsonArray{0.1, -1.0},
                                   QJsonArray{0.5, 0.75} };
    QJsonObject root;
    root["curves"] = curves;

    Look loaded;
    QVERIFY(LookSerializer::fromJson(root, loaded));

    Look expected;
    expected.curves.master.points = {{0.0, 0.0}, {0.5, 0.75}, {1.0, 1.0}};
    const QStringList d = diffLooks(loaded, expected);
    QVERIFY2(d.isEmpty(), qPrintable(d.join("\n")));
}

// ==============================================================================
// Hardening
//
// Everything below treats the document as hostile. A .lxp is a plain text file
// that users copy between machines, share on forums and edit by hand, and a
// project file is written by a build that may be older or newer than the one
// reading it. None of that may crash, and none of it may quietly produce a
// Look the engine cannot handle.
// ==============================================================================
void TstLookSerializer::emptyObjectLoadsAsDefaultLook()
{
    Look loaded;
    QString err;
    QVERIFY(LookSerializer::fromJson(QJsonObject{}, loaded, &err));
    QVERIFY(err.isEmpty());
    QVERIFY(loaded.isIdentity());
    QCOMPARE(loaded.schemaVersion, 1);
    QVERIFY(loaded.name.isEmpty());

    const QStringList d = diffLooks(loaded, Look{});
    QVERIFY2(d.isEmpty(), qPrintable(d.join("\n")));

    // Loading must replace the destination wholesale. If it merged instead,
    // applying a preset would leave fragments of the previous edit behind.
    Look reused = makeFullyPopulatedLook();
    QVERIFY(LookSerializer::fromJson(QJsonObject{}, reused));
    const QStringList d2 = diffLooks(reused, Look{});
    QVERIFY2(d2.isEmpty(), qPrintable(d2.join("\n")));
}

void TstLookSerializer::missingFieldsDefaultToIdentity()
{
    // The forward-compatibility contract, stated one section at a time: a file
    // that carries only one section must load that section and leave every
    // other field at its neutral default. This is what lets a preset written
    // by an older build open in a newer one.
    const Look full = makeFullyPopulatedLook();
    const QJsonObject complete = LookSerializer::toJson(full);

    for (const QString& section : complete.keys()) {
        QJsonObject only;
        only[section] = complete.value(section);

        Look expected;
        if      (section == "name")          expected.name = full.name;
        else if (section == "schemaVersion") expected.schemaVersion = full.schemaVersion;
        else if (section == "hdr")           expected.hdr = full.hdr;
        else if (section == "tone")          expected.tone = full.tone;
        else if (section == "color")         expected.color = full.color;
        else if (section == "curves")        expected.curves = full.curves;
        else if (section == "grading")       expected.grading = full.grading;
        else if (section == "details")       expected.details = full.details;
        else if (section == "effects")       expected.effects = full.effects;
        else if (section == "lens")          expected.lens = full.lens;
        else if (section == "transform")     expected.transform = full.transform;
        else if (section == "localAdjustments") expected.localAdjustments = full.localAdjustments;
        else if (section == "adjustmentLayers") expected.adjustmentLayers = full.adjustmentLayers;
        else QFAIL(qPrintable(QString("unknown top-level section \"%1\": a new part of "
                                      "Look reached the file format without reaching "
                                      "this test").arg(section)));

        Look loaded;
        QVERIFY2(LookSerializer::fromJson(only, loaded), qPrintable(section));
        const QStringList d = diffLooks(loaded, expected);
        QVERIFY2(d.isEmpty(),
                 qPrintable(QString("with only \"%1\" present:\n%2")
                                .arg(section, d.join("\n"))));
    }

    // And the same in miniature: one key, everything else neutral.
    QJsonObject tone;
    tone["exposure"] = 1.5;
    QJsonObject root;
    root["tone"] = tone;
    Look expected;
    expected.tone.exposure = 1.5f;
    Look loaded;
    QVERIFY(LookSerializer::fromJson(root, loaded));
    QVERIFY(!loaded.isIdentity());
    const QStringList d = diffLooks(loaded, expected);
    QVERIFY2(d.isEmpty(), qPrintable(d.join("\n")));
}

void TstLookSerializer::olderFileWithoutNewerSectionsStillLoads()
{
    // A realistic V0/V1 file: whole sections that landed later are absent, and
    // so are a handful of individual keys. Everything that is present must
    // survive intact; everything absent must take its documented default.
    const Look full = makeFullyPopulatedLook();
    QJsonObject doc = LookSerializer::toJson(full);
    doc.remove("hdr");
    doc.remove("lens");
    doc.remove("transform");
    doc.remove("localAdjustments");
    doc.remove("adjustmentLayers");

    QJsonObject tone = doc.value("tone").toObject();
    tone.remove("brightness");          // landed after the first release
    doc["tone"] = tone;

    QJsonObject grading = doc.value("grading").toObject();
    grading.remove("lift");
    grading.remove("filmicContrast");
    grading.remove("lutEnabled");       // absent means "on", to match V0
    grading.remove("blending");         // absent means 50, not 0
    doc["grading"] = grading;

    Look expected = full;
    expected.hdr = HDRParams{};
    expected.lens = LensParams{};
    expected.transform = TransformParams{};
    expected.localAdjustments.clear();
    expected.adjustmentLayers.clear();
    expected.tone.brightness = 0.0f;
    expected.grading.lift = 0.0f;
    expected.grading.filmicContrast = 0.0f;
    expected.grading.lutEnabled = true;
    expected.grading.blending = 50.0f;

    Look loaded;
    QVERIFY(LookSerializer::fromJson(doc, loaded));
    const QStringList d = diffLooks(loaded, expected);
    QVERIFY2(d.isEmpty(), qPrintable(d.join("\n")));
}

void TstLookSerializer::unknownFieldsAreIgnored()
{
    // The other half of forward compatibility: a file written by a NEWER build
    // carries keys this one has never heard of. They must be skipped, not
    // treated as corruption.
    const Look full = makeFullyPopulatedLook();
    QJsonObject doc = LookSerializer::toJson(full);
    doc["quantumHarmonizer"] = 42.0;
    doc["futureSection"] = QJsonObject{ {"a", 1.0}, {"b", QJsonArray{1.0, 2.0}} };
    doc = poke(doc, "tone", "midtoneWarp", 3.0);
    doc = poke(doc, "grading", "teal", QJsonArray{1.0, 2.0, 3.0});
    doc = pokeElement(doc, "localAdjustments", 0, "featherCurve", QJsonArray{});
    doc = pokeElement(doc, "adjustmentLayers", 1, "clippingMask", true);

    Look loaded;
    QVERIFY(LookSerializer::fromJson(doc, loaded));
    const QStringList d = diffLooks(loaded, full);
    QVERIFY2(d.isEmpty(), qPrintable(d.join("\n")));
}

void TstLookSerializer::wrongTypedContainersAreIgnored()
{
    // A section that is not an object, or a list that is not a list. The
    // loader must skip it and keep going -- one mangled section may not take
    // the rest of the edit with it.
    const Look full = makeFullyPopulatedLook();
    QJsonObject doc = LookSerializer::toJson(full);
    doc["tone"]             = QJsonValue(QStringLiteral("not an object"));
    doc["details"]          = QJsonArray{1.0, 2.0, 3.0};
    doc["localAdjustments"] = QJsonObject{ {"count", 2.0} };
    doc["adjustmentLayers"] = 7.0;
    doc = poke(doc, "color", "hsl", QJsonValue(QStringLiteral("reddish")));
    doc = poke(doc, "color", "rgbMixer", QJsonArray{});
    doc = poke(doc, "curves", "master", QJsonValue(QStringLiteral("s-curve")));
    doc = poke(doc, "transform", "cropRect", QJsonValue(QStringLiteral("full frame")));

    Look expected = full;
    expected.tone     = ToneParams{};
    expected.details  = DetailsParams{};
    expected.localAdjustments.clear();
    expected.adjustmentLayers.clear();
    expected.color.hsl        = HSLParams{};
    expected.color.rgbMixer   = RGBMixerParams{};
    expected.curves.master    = CurvePoints{};
    expected.transform.cropRect = QRectF(0, 0, 1, 1);

    Look loaded;
    QString err;
    QVERIFY2(LookSerializer::fromJson(doc, loaded, &err), qPrintable(err));
    const QStringList d = diffLooks(loaded, expected);
    QVERIFY2(d.isEmpty(), qPrintable(d.join("\n")));
}

void TstLookSerializer::wrongTypedLeavesAreIgnored()
{
    // A string where a float belongs, a number where a bool belongs, a null.
    // Each poisoned key falls back to its documented default; its neighbours
    // are untouched.
    const Look full = makeFullyPopulatedLook();
    QJsonObject doc = LookSerializer::toJson(full);
    doc = poke(doc, "tone", "exposure", QJsonValue(QStringLiteral("lots")));
    doc = poke(doc, "tone", "contrast", QJsonValue(true));
    doc = poke(doc, "tone", "shadows",  QJsonObject{});
    doc = poke(doc, "tone", "whites",   QJsonArray{1.0});
    doc = poke(doc, "tone", "blacks",   QJsonValue());              // null
    doc = poke(doc, "grading", "lutPath", 12.0);
    doc = poke(doc, "grading", "lutOpacity", QJsonValue(QStringLiteral("half")));
    doc = poke(doc, "grading", "lutEnabled", 1.0);                  // absent bool -> true
    doc = poke(doc, "hdr", "enabled", QJsonValue(QStringLiteral("yes")));
    doc = poke(doc, "transform", "flipHorizontal", 1.0);
    doc = poke(doc, "effects", "grain", QJsonValue(QStringLiteral("chunky")));
    doc = pokeElement(doc, "localAdjustments", 0, "enabled", 3.0);  // -> default true
    doc = pokeElement(doc, "localAdjustments", 0, "type", QJsonValue(QStringLiteral("brush")));
    doc = pokeElement(doc, "localAdjustments", 1, "radius", QJsonValue(QStringLiteral("big")));
    doc = pokeElement(doc, "adjustmentLayers", 0, "opacity", QJsonValue(QStringLiteral("clear")));
    doc = pokeElement(doc, "adjustmentLayers", 0, "blendMode", QJsonValue(QStringLiteral("overlay")));
    doc = pokeElement(doc, "adjustmentLayers", 1, "name", 5.0);

    Look expected = full;
    expected.tone.exposure = 0.0f;
    expected.tone.contrast = 0.0f;
    expected.tone.shadows  = 0.0f;
    expected.tone.whites   = 0.0f;
    expected.tone.blacks   = 0.0f;
    expected.grading.lutPath    = QString();
    expected.grading.lutOpacity = 1.0f;
    expected.grading.lutEnabled = true;
    expected.hdr.enabled = false;
    expected.transform.flipHorizontal = false;
    expected.effects.grain = GrainParams{};
    expected.localAdjustments[0].enabled = true;
    expected.localAdjustments[0].type    = MaskType::LinearGradient;
    expected.localAdjustments[1].radius  = LocalAdjustment{}.radius;
    expected.adjustmentLayers[0].opacity   = 1.0f;
    expected.adjustmentLayers[0].blendMode = BlendMode::Normal;
    expected.adjustmentLayers[1].name      = QString();

    Look loaded;
    QString err;
    QVERIFY2(LookSerializer::fromJson(doc, loaded, &err), qPrintable(err));
    const QStringList d = diffLooks(loaded, expected);
    QVERIFY2(d.isEmpty(), qPrintable(d.join("\n")));
}

void TstLookSerializer::nonFiniteValuesAreRejected()
{
    // NaN and infinity must never reach the pipeline. One NaN pixel spreads:
    // it survives every multiply and lerp downstream, so a single poisoned
    // parameter can black out or speckle an entire export.
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();

    const Look full = makeFullyPopulatedLook();
    QJsonObject doc = LookSerializer::toJson(full);
    doc = poke(doc, "tone", "exposure", nan);
    doc = poke(doc, "tone", "contrast", inf);
    doc = poke(doc, "tone", "shadows", -inf);
    doc = poke(doc, "hdr", "midtonePivot", nan);
    doc = poke(doc, "grading", "lutOpacity", inf);
    doc = poke(doc, "details", "sharpeningRadius", nan);
    doc = poke(doc, "curves", "master", QJsonArray{ QJsonArray{0.0, 0.0},
                                                    QJsonArray{0.5, nan},
                                                    QJsonArray{1.0, 1.0} });
    doc = poke(doc, "transform", "cropRect",
               QJsonObject{ {"x", nan}, {"y", 0.0}, {"width", inf}, {"height", 0.5} });

    QJsonObject color = doc.value("color").toObject();
    QJsonObject mixer = color.value("rgbMixer").toObject();
    mixer["green"] = QJsonArray{0.5, nan, 0.25};
    color["rgbMixer"] = mixer;
    doc["color"] = color;

    Look expected = full;
    expected.tone.exposure = 0.0f;
    expected.tone.contrast = 0.0f;
    expected.tone.shadows  = 0.0f;
    expected.hdr.midtonePivot = HDRParams{}.midtonePivot;
    expected.grading.lutOpacity = 1.0f;
    expected.details.sharpeningRadius = DetailsParams{}.sharpeningRadius;
    expected.curves.master = CurvePoints{};          // one bad point rejects the curve
    expected.transform.cropRect = QRectF(0.0, 0.0, 1.0, 0.5);
    // Only the poisoned coefficient falls back; the row keeps its good values.
    expected.color.rgbMixer.greenOutput = { 0.5f, RGBMixerParams{}.greenOutput.g, 0.25f };

    Look loaded;
    QVERIFY(LookSerializer::fromJson(doc, loaded));
    const QStringList bad = nonFiniteFields(loaded);
    QVERIFY2(bad.isEmpty(), qPrintable(QString("non-finite after load:\n%1").arg(bad.join("\n"))));
    const QStringList d = diffLooks(loaded, expected);
    QVERIFY2(d.isEmpty(), qPrintable(d.join("\n")));
}

void TstLookSerializer::nonFiniteValuesCannotBeWrittenToDisk()
{
    // The other direction: a Look that already holds a NaN (a division by zero
    // in some future engine, a bad slider binding) must not be able to write a
    // file that reintroduces it -- and must not write "nan"/"inf" tokens, which
    // are not JSON and would make the preset unreadable by anything at all.
    Look poisoned = makeFullyPopulatedLook();
    poisoned.tone.exposure       = std::numeric_limits<float>::quiet_NaN();
    poisoned.color.vibrance      = std::numeric_limits<float>::infinity();
    poisoned.effects.grain.amount = -std::numeric_limits<float>::infinity();

    QTemporaryDir dir;
    QVERIFY2(dir.isValid(), qPrintable(dir.errorString()));
    const QString path = dir.filePath("poisoned.lxp");
    const SaveResult saved = LookSerializer::saveToFile(poisoned, path);
    QVERIFY2(saved.ok, qPrintable(saved.errorMessage));

    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QByteArray bytes = f.readAll();
    f.close();
    // Matched on values rather than raw substrings, because "luminance" and
    // "highlightsLuminance" both contain "nan".
    const QRegularExpression nonFiniteToken(
        QStringLiteral(R"(:\s*-?(nan|inf|NaN|Inf|Infinity))"));
    const QRegularExpressionMatch hit = nonFiniteToken.match(QString::fromUtf8(bytes));
    QVERIFY2(!hit.hasMatch(),
             qPrintable(QString("wrote a non-finite token: %1").arg(hit.captured(0))));
    QJsonParseError parseErr;
    const QJsonDocument reparsed = QJsonDocument::fromJson(bytes, &parseErr);
    QCOMPARE(parseErr.error, QJsonParseError::NoError);
    QVERIFY(reparsed.isObject());

    const LoadResult loaded = LookSerializer::loadFromFile(path);
    QVERIFY2(loaded.ok, qPrintable(loaded.errorMessage));
    const QStringList bad = nonFiniteFields(loaded.look);
    QVERIFY2(bad.isEmpty(), qPrintable(bad.join("\n")));

    Look expected = poisoned;
    expected.tone.exposure        = 0.0f;
    expected.color.vibrance       = 0.0f;
    expected.effects.grain.amount = 0.0f;
    const QStringList d = diffLooks(loaded.look, expected);
    QVERIFY2(d.isEmpty(), qPrintable(d.join("\n")));
}

void TstLookSerializer::hugeExponentsInAFileCannotProduceInfinities()
{
    // JSON has no NaN literal, but it has no exponent limit either: 1e999 is
    // legal text that overflows a double. This is the one route by which a
    // hand-edited file can push an infinity into the loader.
    QTemporaryDir dir;
    QVERIFY2(dir.isValid(), qPrintable(dir.errorString()));
    const QString path = dir.filePath("overflow.lxp");

    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("{\n"
            "  \"tone\": { \"exposure\": 1e999, \"contrast\": -1e999, \"shadows\": 1e-999 },\n"
            "  \"effects\": { \"vignette\": { \"amount\": 1e400 } }\n"
            "}\n");
    f.close();

    const LoadResult loaded = LookSerializer::loadFromFile(path);
    if (loaded.ok) {
        const QStringList bad = nonFiniteFields(loaded.look);
        QVERIFY2(bad.isEmpty(),
                 qPrintable(QString("an overflowing literal reached the model:\n%1")
                                .arg(bad.join("\n"))));
        // Whatever survived is inside the documented range.
        QVERIFY(loaded.look.tone.exposure >= -10.0f && loaded.look.tone.exposure <= 10.0f);
        QVERIFY(loaded.look.tone.contrast >= -100.0f && loaded.look.tone.contrast <= 100.0f);
        QVERIFY(loaded.look.effects.vignette.amount >= -100.0f
                && loaded.look.effects.vignette.amount <= 100.0f);
        QCOMPARE(loaded.look.tone.shadows, 0.0f);   // underflow is just zero
    } else {
        // Rejecting the file outright is also acceptable -- as long as it is a
        // clean rejection with something to show the user.
        QVERIFY(!loaded.errorMessage.isEmpty());
        QVERIFY(loaded.look.isIdentity());
    }
}

void TstLookSerializer::maskGeometryRejectsNonFiniteValues()
{
    // Mask geometry takes a different route into the model than every other
    // float: readPoint() and the brush stamps call QJsonValue::toDouble()
    // directly rather than going through readFloat(), so they never see
    // readFloat()'s isfinite() guard. What catches a bad value is
    // clampRanges() afterwards, which maps a non-finite coordinate onto one
    // of the bounds. Which bound is not worth pinning down (it depends on how
    // the compiler lowers the comparison against NaN), but "finite" is: a NaN
    // coordinate would spread through the mask weight and wipe out the
    // adjustment wherever the mask applied.
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();

    const Look full = makeFullyPopulatedLook();
    QJsonObject doc = LookSerializer::toJson(full);
    doc = pokeElement(doc, "localAdjustments", 0, "startPoint", QJsonArray{nan, 0.25});
    doc = pokeElement(doc, "localAdjustments", 0, "endPoint",   QJsonArray{inf, -inf});
    doc = pokeElement(doc, "localAdjustments", 1, "center",     QJsonArray{0.5, nan});

    QJsonArray masks = doc.value("localAdjustments").toArray();
    QJsonObject mask0 = masks.at(0).toObject();
    QJsonArray strokes = mask0.value("brushStrokes").toArray();
    QJsonObject stroke0 = strokes.at(0).toObject();
    stroke0["points"] = QJsonArray{ QJsonArray{nan, nan}, QJsonArray{0.1, 0.2} };
    strokes.replace(0, stroke0);
    mask0["brushStrokes"] = strokes;
    masks.replace(0, mask0);
    doc["localAdjustments"] = masks;

    Look loaded;
    QVERIFY(LookSerializer::fromJson(doc, loaded));
    const QStringList bad = nonFiniteFields(loaded);
    QVERIFY2(bad.isEmpty(),
             qPrintable(QString("non-finite mask geometry reached the model:\n%1")
                            .arg(bad.join("\n"))));
}

void TstLookSerializer::unknownEnumValuesFallBackToDefaults()
{
    // Enums are stored as ints so the format survives reordering. A value from
    // the future, or from a corrupt file, must land on a known member rather
    // than being cast blindly into an out-of-range enum.
    const Look full = makeFullyPopulatedLook();
    QJsonObject doc = LookSerializer::toJson(full);
    doc = pokeElement(doc, "localAdjustments", 0, "type", 77.0);
    doc = pokeElement(doc, "localAdjustments", 1, "type", -1.0);
    doc = pokeElement(doc, "adjustmentLayers", 0, "blendMode", 999.0);
    doc = pokeElement(doc, "adjustmentLayers", 1, "blendMode", -5.0);

    Look loaded;
    QVERIFY(LookSerializer::fromJson(doc, loaded));
    QCOMPARE(loaded.localAdjustments[0].type, MaskType::LinearGradient);
    QCOMPARE(loaded.localAdjustments[1].type, MaskType::LinearGradient);
    QCOMPARE(loaded.adjustmentLayers[0].blendMode, BlendMode::Normal);
    QCOMPARE(loaded.adjustmentLayers[1].blendMode, BlendMode::Normal);

    // ...and every value that IS known survives, including the last one.
    for (int mode = 0; mode <= 10; ++mode) {
        QJsonObject one = LookSerializer::toJson(full);
        one = pokeElement(one, "adjustmentLayers", 0, "blendMode", double(mode));
        Look l;
        QVERIFY(LookSerializer::fromJson(one, l));
        QCOMPARE(static_cast<int>(l.adjustmentLayers[0].blendMode), mode);
    }
    for (int type = 0; type <= 2; ++type) {
        QJsonObject one = LookSerializer::toJson(full);
        one = pokeElement(one, "localAdjustments", 0, "type", double(type));
        Look l;
        QVERIFY(LookSerializer::fromJson(one, l));
        QCOMPARE(static_cast<int>(l.localAdjustments[0].type), type);
    }
}

void TstLookSerializer::schemaVersionIsWrittenAndRead()
{
    // Documented behaviour (LookSerializer.cpp): the writer always stamps the
    // version this build authors; the reader records whatever the file
    // declared and loads it best-effort, relying on missing-field tolerance if
    // the file came from a newer build. There is no version gate, and there
    // must not be one: refusing to open a file because it says "2" would lose
    // an edit over a number.
    QCOMPARE(LookSerializer::toJson(Look{}).value("schemaVersion").toInt(-1), 1);

    const Look full = makeFullyPopulatedLook();
    QJsonObject doc = LookSerializer::toJson(full);
    doc["schemaVersion"] = 99;

    Look loaded;
    QVERIFY(LookSerializer::fromJson(doc, loaded));
    QCOMPARE(loaded.schemaVersion, 99);

    Look expected = full;
    expected.schemaVersion = 99;
    const QStringList d = diffLooks(loaded, expected);
    QVERIFY2(d.isEmpty(),
             qPrintable(QString("a file from a newer build lost fields:\n%1").arg(d.join("\n"))));

    // Re-saving stamps the current version rather than echoing the one read.
    QCOMPARE(LookSerializer::toJson(loaded).value("schemaVersion").toInt(-1), 1);

    // A pre-versioning file keeps the number it declared, so a migration step
    // added later can still tell what it is looking at.
    doc["schemaVersion"] = 0;
    QVERIFY(LookSerializer::fromJson(doc, loaded));
    QCOMPARE(loaded.schemaVersion, 0);

    // Missing or wrong-typed: assume the current version.
    doc.remove("schemaVersion");
    QVERIFY(LookSerializer::fromJson(doc, loaded));
    QCOMPARE(loaded.schemaVersion, 1);
    doc["schemaVersion"] = QStringLiteral("v9");
    QVERIFY(LookSerializer::fromJson(doc, loaded));
    QCOMPARE(loaded.schemaVersion, 1);
}

void TstLookSerializer::outOfRangeValuesAreClampedOnLoad()
{
    // clampRanges() runs at the end of every load, which is what lets the
    // engines skip bounds checks. A hand-edited or corrupt preset therefore
    // cannot push a value past its documented limit.
    QJsonObject doc;
    doc["tone"] = QJsonObject{ {"exposure", 9999.0}, {"contrast", -5000.0},
                               {"brightness", 250.0} };
    doc["hdr"]  = QJsonObject{ {"enabled", true}, {"midtonePivot", 0.0},
                               {"exposureBias", -50.0}, {"highlightCompression", 900.0} };
    doc["details"] = QJsonObject{ {"sharpeningAmount", 1000.0}, {"sharpeningRadius", 0.01} };
    doc["grading"] = QJsonObject{ {"lutOpacity", 7.0}, {"shadowsHue", 450.0},
                                  {"globalHue", -90.0}, {"blending", 900.0},
                                  {"lift", -900.0} };
    doc["color"] = QJsonObject{ {"saturation", 1000000.0},
                                {"rgbMixer", QJsonObject{ {"red", QJsonArray{50.0, -50.0, 0.5}} }} };
    doc["transform"] = QJsonObject{ {"rotationDegrees", 540.0}, {"straightenAngle", 99.0},
                                    {"cropRect", QJsonObject{ {"x", -1.0}, {"y", 0.5},
                                                              {"width", 5.0}, {"height", 5.0} }} };
    doc["localAdjustments"] = QJsonArray{ QJsonObject{ {"radius", 0.0}, {"density", 5.0},
                                                       {"feather", -3.0}, {"exposure", 99.0},
                                                       {"brushSize", 0.0} } };
    doc["adjustmentLayers"] = QJsonArray{ QJsonObject{ {"opacity", 3.0} } };

    Look loaded;
    QVERIFY(LookSerializer::fromJson(doc, loaded));

    QCOMPARE(loaded.tone.exposure,   10.0f);
    QCOMPARE(loaded.tone.contrast,  -100.0f);
    QCOMPARE(loaded.tone.brightness, 100.0f);
    QCOMPARE(loaded.hdr.midtonePivot, 0.05f);
    QCOMPARE(loaded.hdr.exposureBias, -5.0f);
    QCOMPARE(loaded.hdr.highlightCompression, 100.0f);
    QCOMPARE(loaded.details.sharpeningAmount, 150.0f);
    QCOMPARE(loaded.details.sharpeningRadius, 0.5f);
    QCOMPARE(loaded.grading.lutOpacity, 1.0f);
    QCOMPARE(loaded.grading.blending, 100.0f);
    QCOMPARE(loaded.grading.lift, -100.0f);
    // Hues wrap rather than clamp: 450 becomes 90, -90 becomes 270.
    QCOMPARE(loaded.grading.shadowsHue, 90.0f);
    QCOMPARE(loaded.grading.globalHue, 270.0f);
    QCOMPARE(loaded.color.saturation, 100.0f);
    QCOMPARE(loaded.color.rgbMixer.redOutput.r,  2.0f);
    QCOMPARE(loaded.color.rgbMixer.redOutput.g, -2.0f);
    QCOMPARE(loaded.color.rgbMixer.redOutput.b,  0.5f);
    QCOMPARE(loaded.transform.rotationDegrees, 180.0f);
    QCOMPARE(loaded.transform.straightenAngle, 10.0f);
    QCOMPARE(loaded.transform.cropRect, QRectF(0.0, 0.5, 1.0, 0.5));
    QCOMPARE(loaded.localAdjustments.size(), size_t(1));
    QCOMPARE(loaded.localAdjustments[0].radius, 1e-4f);
    QCOMPARE(loaded.localAdjustments[0].density, 1.0f);
    QCOMPARE(loaded.localAdjustments[0].feather, 0.0f);
    QCOMPARE(loaded.localAdjustments[0].exposure, 10.0f);
    QCOMPARE(loaded.localAdjustments[0].brushSize, 0.001f);
    QCOMPARE(loaded.adjustmentLayers.size(), size_t(1));
    QCOMPARE(loaded.adjustmentLayers[0].opacity, 1.0f);
}

void TstLookSerializer::malformedJsonFailsCleanly()
{
    QTemporaryDir dir;
    QVERIFY2(dir.isValid(), qPrintable(dir.errorString()));

    struct Case { const char* name; const char* bytes; };
    const Case cases[] = {
        { "truncated",     "{ \"tone\": { \"exposure\": " },
        { "unterminated",  "{ \"name\": \"unfinished" },
        { "not json",      "this is not a preset, it is a photograph" },
        { "trailing junk", "{ \"name\": \"ok\" } and then some" },
    };

    for (const Case& c : cases) {
        const QString path = dir.filePath(QString("bad-%1.lxp").arg(QLatin1String(c.name)));
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(c.bytes);
        f.close();

        const LoadResult r = LookSerializer::loadFromFile(path);
        QVERIFY2(!r.ok, c.name);
        QVERIFY2(!r.errorMessage.isEmpty(), c.name);
        // The Look handed back on failure must be inert, not half-parsed.
        QVERIFY2(r.look.isIdentity(), c.name);
    }
}

void TstLookSerializer::emptyFileFailsCleanly()
{
    QTemporaryDir dir;
    QVERIFY2(dir.isValid(), qPrintable(dir.errorString()));
    const QString path = dir.filePath("empty.lxp");
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.close();
    QCOMPARE(QFileInfo(path).size(), qint64(0));

    const LoadResult r = LookSerializer::loadFromFile(path);
    QVERIFY(!r.ok);
    QVERIFY(!r.errorMessage.isEmpty());
    QVERIFY(r.look.isIdentity());
}

void TstLookSerializer::arrayRootFailsCleanly()
{
    QTemporaryDir dir;
    QVERIFY2(dir.isValid(), qPrintable(dir.errorString()));
    const QString path = dir.filePath("array.lxp");
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("[ { \"tone\": { \"exposure\": 1.0 } } ]");
    f.close();

    // Valid JSON, wrong shape. A preset *collection* would look like this, and
    // opening one as a single preset has to say so rather than loading nothing
    // and reporting success.
    const LoadResult r = LookSerializer::loadFromFile(path);
    QVERIFY(!r.ok);
    QVERIFY(!r.errorMessage.isEmpty());
    QVERIFY(r.look.isIdentity());
}

void TstLookSerializer::missingFileFailsCleanly()
{
    QTemporaryDir dir;
    QVERIFY2(dir.isValid(), qPrintable(dir.errorString()));
    const LoadResult r = LookSerializer::loadFromFile(dir.filePath("no-such-preset.lxp"));
    QVERIFY(!r.ok);
    QVERIFY(!r.errorMessage.isEmpty());
    QVERIFY(r.look.isIdentity());
}

void TstLookSerializer::unwritablePathFailsCleanly()
{
    QTemporaryDir dir;
    QVERIFY2(dir.isValid(), qPrintable(dir.errorString()));
    // Saving into a directory that does not exist: the folder the user picked
    // lives on a drive that has since been unplugged. Report it, do not crash.
    const SaveResult r = LookSerializer::saveToFile(makeFullyPopulatedLook(),
                                                    dir.filePath("nope/deeper/preset.lxp"));
    QVERIFY(!r.ok);
    QVERIFY(!r.errorMessage.isEmpty());
}

// ==============================================================================
// The Look model itself
//
// isIdentity() is a fast path: the pipeline skips a stage when it reports true.
// A field it forgets to check is therefore an edit the user makes and never
// sees. clampRanges() is the other half of that bargain, since engines are
// documented as free to assume their inputs are in range.
// ==============================================================================
void TstLookSerializer::defaultLookIsIdentity()
{
    const Look look;
    QVERIFY(look.isIdentity());
    QVERIFY(look.hdr.isIdentity());
    QVERIFY(look.tone.isIdentity());
    QVERIFY(look.color.isIdentity());
    QVERIFY(look.curves.isIdentity());
    QVERIFY(look.grading.isIdentity());
    QVERIFY(look.details.isIdentity());
    QVERIFY(look.effects.isIdentity());
    QVERIFY(look.lens.isIdentity());
    QVERIFY(look.transform.isIdentity());
    QVERIFY(look.localAdjustments.empty());
    QVERIFY(look.adjustmentLayers.empty());
    const LayerAdjustmentData payload;
    QVERIFY(payload.isIdentity());
    const AdjustmentLayer layer;
    QVERIFY(layer.isIdentity());
}

// One field at a time, from a default Look. Each edit must both break
// isIdentity() and survive a round trip still broken: a preset whose only
// content is "warm the shadows a little" has to arrive as exactly that.
namespace {

struct Edit { const char* what; std::function<void(Look&)> apply; };

const Edit kBreakingEdits[] = {
    { "tone.exposure",   [](Look& l){ l.tone.exposure   =  0.5f; } },
    { "tone.contrast",   [](Look& l){ l.tone.contrast   = 25.0f; } },
    { "tone.highlights", [](Look& l){ l.tone.highlights = -30.0f; } },
    { "tone.shadows",    [](Look& l){ l.tone.shadows    = 30.0f; } },
    { "tone.whites",     [](Look& l){ l.tone.whites     = 10.0f; } },
    { "tone.blacks",     [](Look& l){ l.tone.blacks     = -10.0f; } },
    { "tone.brightness", [](Look& l){ l.tone.brightness = 15.0f; } },

    { "color.whiteBalance.temperature", [](Look& l){ l.color.whiteBalance.temperature = 20.0f; } },
    { "color.whiteBalance.tint",        [](Look& l){ l.color.whiteBalance.tint = -20.0f; } },
    { "color.vibrance",                 [](Look& l){ l.color.vibrance = 40.0f; } },
    { "color.saturation",               [](Look& l){ l.color.saturation = -40.0f; } },

    { "hsl.red.hue",            [](Look& l){ l.color.hsl.red.hue = 10.0f; } },
    { "hsl.orange.saturation",  [](Look& l){ l.color.hsl.orange.saturation = 10.0f; } },
    { "hsl.yellow.luminance",   [](Look& l){ l.color.hsl.yellow.luminance = 10.0f; } },
    { "hsl.green.hue",          [](Look& l){ l.color.hsl.green.hue = -10.0f; } },
    { "hsl.aqua.saturation",    [](Look& l){ l.color.hsl.aqua.saturation = -10.0f; } },
    { "hsl.blue.luminance",     [](Look& l){ l.color.hsl.blue.luminance = -10.0f; } },
    { "hsl.purple.hue",         [](Look& l){ l.color.hsl.purple.hue = 5.0f; } },
    { "hsl.magenta.saturation", [](Look& l){ l.color.hsl.magenta.saturation = 5.0f; } },

    { "rgbMixer.redOutput.g",   [](Look& l){ l.color.rgbMixer.redOutput.g = 0.5f; } },
    { "rgbMixer.greenOutput.r", [](Look& l){ l.color.rgbMixer.greenOutput.r = -0.5f; } },
    { "rgbMixer.blueOutput.b",  [](Look& l){ l.color.rgbMixer.blueOutput.b = 0.75f; } },

    { "curves.master", [](Look& l){ l.curves.master.points = {{0.0,0.0},{0.5,0.6},{1.0,1.0}}; } },
    { "curves.red",    [](Look& l){ l.curves.red.points    = {{0.0,0.1},{1.0,1.0}}; } },
    { "curves.green",  [](Look& l){ l.curves.green.points  = {{0.0,0.0},{1.0,0.9}}; } },
    { "curves.blue",   [](Look& l){ l.curves.blue.points   = {{0.0,0.0},{0.25,0.3},{1.0,1.0}}; } },

    { "grading.lutPath",       [](Look& l){ l.grading.lutPath = QStringLiteral("/luts/a.cube"); } },
    { "grading.filmProfileId", [](Look& l){ l.grading.filmProfileId = QStringLiteral("portra"); } },
    { "grading shadows wheel",    [](Look& l){ l.grading.shadowsSaturation = 40.0f;
                                               l.grading.shadowsStrength = 40.0f; } },
    { "grading midtones wheel",   [](Look& l){ l.grading.midtonesSaturation = 40.0f;
                                               l.grading.midtonesStrength = 40.0f; } },
    { "grading highlights wheel", [](Look& l){ l.grading.highlightsSaturation = 40.0f;
                                               l.grading.highlightsStrength = 40.0f; } },
    { "grading global wheel",     [](Look& l){ l.grading.globalSaturation = 40.0f;
                                               l.grading.globalStrength = 40.0f; } },
    { "grading.shadowsLuminance",    [](Look& l){ l.grading.shadowsLuminance = 20.0f; } },
    { "grading.midtonesLuminance",   [](Look& l){ l.grading.midtonesLuminance = 20.0f; } },
    { "grading.highlightsLuminance", [](Look& l){ l.grading.highlightsLuminance = 20.0f; } },
    { "grading.globalLuminance",     [](Look& l){ l.grading.globalLuminance = 20.0f; } },
    { "grading.lift",             [](Look& l){ l.grading.lift = 10.0f; } },
    { "grading.gamma",            [](Look& l){ l.grading.gamma = 10.0f; } },
    { "grading.gain",             [](Look& l){ l.grading.gain = 10.0f; } },
    { "grading.offset",           [](Look& l){ l.grading.offset = 10.0f; } },
    { "grading.filmicContrast",   [](Look& l){ l.grading.filmicContrast = 10.0f; } },
    { "grading.highlightRolloff", [](Look& l){ l.grading.highlightRolloff = 10.0f; } },
    { "grading.shadowLift",       [](Look& l){ l.grading.shadowLift = 10.0f; } },
    { "grading.fadeBlacks",       [](Look& l){ l.grading.fadeBlacks = 10.0f; } },
    { "grading.colorSeparation",  [](Look& l){ l.grading.colorSeparation = 10.0f; } },

    { "details.sharpeningAmount", [](Look& l){ l.details.sharpeningAmount = 50.0f; } },
    { "details.luminanceNR",      [](Look& l){ l.details.luminanceNR = 20.0f; } },
    { "details.colorNR",          [](Look& l){ l.details.colorNR = 20.0f; } },

    { "effects.vignette.amount", [](Look& l){ l.effects.vignette.amount = -35.0f; } },
    { "effects.grain.amount",    [](Look& l){ l.effects.grain.amount = 20.0f; } },
    { "effects.clarity.amount",  [](Look& l){ l.effects.clarity.amount = 20.0f; } },

    { "lens.distortion",   [](Look& l){ l.lens.enabled = true; l.lens.distortion = 20.0f; } },
    { "lens.vignetting",   [](Look& l){ l.lens.enabled = true; l.lens.vignetting = 20.0f; } },
    { "lens.purpleFringe", [](Look& l){ l.lens.enabled = true; l.lens.purpleFringe = 20.0f; } },
    { "lens.greenFringe",  [](Look& l){ l.lens.enabled = true; l.lens.greenFringe = 20.0f; } },
    { "lens.removeChromaticAberration",
          [](Look& l){ l.lens.enabled = true; l.lens.removeChromaticAberration = true; } },

    { "transform.rotationDegrees", [](Look& l){ l.transform.rotationDegrees = 90.0f; } },
    { "transform.flipHorizontal",  [](Look& l){ l.transform.flipHorizontal = true; } },
    { "transform.flipVertical",    [](Look& l){ l.transform.flipVertical = true; } },
    { "transform.straightenAngle", [](Look& l){ l.transform.straightenAngle = 2.5f; } },
    { "transform.cropRect",        [](Look& l){ l.transform.cropRect = QRectF(0.25, 0.25, 0.5, 0.5); } },

    { "hdr.enabled", [](Look& l){ l.hdr.enabled = true; } },

    { "a local adjustment", [](Look& l){ LocalAdjustment m; m.name = QStringLiteral("Sky");
                                         m.exposure = 1.0f; l.localAdjustments.push_back(m); } },
    { "a local adjustment (white balance only)",
          [](Look& l){ LocalAdjustment m; m.temperature = 15.0f; l.localAdjustments.push_back(m); } },
    { "an adjustment layer", [](Look& l){ AdjustmentLayer a; a.name = QStringLiteral("Grade");
                                          a.adjustmentData.tone.exposure = 1.0f;
                                          l.adjustmentLayers.push_back(a); } },
    { "an adjustment layer (curve only)",
          [](Look& l){ AdjustmentLayer a;
                       a.adjustmentData.curves.master.points = {{0.0,0.0},{0.5,0.7},{1.0,1.0}};
                       l.adjustmentLayers.push_back(a); } },
};

} // namespace

void TstLookSerializer::anySingleEditBreaksIdentity()
{
    for (const Edit& e : kBreakingEdits) {
        Look edited;
        QVERIFY(edited.isIdentity());
        e.apply(edited);
        QVERIFY2(!edited.isIdentity(),
                 qPrintable(QString("isIdentity() ignores %1, so the pipeline would skip it")
                                .arg(QLatin1String(e.what))));

        // The same edit has to come back off disk still being that edit. The
        // comparison target is the clamped Look because that is what the
        // loader guarantees: clampRanges() runs on every load, and for mask
        // geometry it narrows QPointF coordinates to float, so an untouched
        // default endPoint of 0.4 comes back as float(0.4). Everything else
        // here is already in range, so clamping changes nothing else.
        Look expected = edited;
        expected.clampRanges();
        Look loaded;
        QVERIFY2(LookSerializer::fromJson(LookSerializer::toJson(edited), loaded), e.what);
        QVERIFY2(!loaded.isIdentity(), e.what);
        const QStringList d = diffLooks(loaded, expected);
        QVERIFY2(d.isEmpty(),
                 qPrintable(QString("%1:\n%2").arg(QLatin1String(e.what), d.join("\n"))));
    }
}

void TstLookSerializer::documentedNonEditsKeepIdentity()
{
    // The mirror image, and just as important: these fields are documented as
    // having no effect on their own, and isIdentity() deliberately ignores
    // them. If one of these starts breaking identity, the pipeline stops
    // fast-pathing untouched photos and every thumbnail gets slower for
    // nothing. If one of them ever gains a real effect, this test is the
    // reminder that isIdentity() has to learn about it.
    const Edit nonEdits[] = {
        // Sharpening radius/detail/masking do nothing while amount is 0.
        { "details.sharpeningRadius",  [](Look& l){ l.details.sharpeningRadius = 2.5f; } },
        { "details.sharpeningDetail",  [](Look& l){ l.details.sharpeningDetail = 60.0f; } },
        { "details.sharpeningMasking", [](Look& l){ l.details.sharpeningMasking = 60.0f; } },
        { "details.luminanceDetail",   [](Look& l){ l.details.luminanceDetail = 60.0f; } },
        { "details.colorDetail",       [](Look& l){ l.details.colorDetail = 60.0f; } },
        // HDR and lens both have a master switch; off means off.
        { "hdr params while disabled", [](Look& l){ l.hdr.exposureBias = 3.0f;
                                                    l.hdr.midtonePivot = 0.5f; } },
        { "lens params while disabled", [](Look& l){ l.lens.distortion = 50.0f;
                                                     l.lens.removeChromaticAberration = true; } },
        // A colour wheel tints only when saturation AND strength are non-zero.
        { "grading.shadowsHue alone",        [](Look& l){ l.grading.shadowsHue = 180.0f; } },
        { "grading.shadowsSaturation alone", [](Look& l){ l.grading.shadowsSaturation = 80.0f; } },
        { "grading.midtonesStrength alone",  [](Look& l){ l.grading.midtonesStrength = 80.0f; } },
        // Balance and blending only reshape zones that are not being tinted.
        { "grading.balance",  [](Look& l){ l.grading.balance = 50.0f; } },
        { "grading.blending", [](Look& l){ l.grading.blending = 10.0f; } },
        // Opacity without a LUT, and a LUT that is switched off or invisible.
        { "grading.lutOpacity with no LUT",  [](Look& l){ l.grading.lutOpacity = 0.5f; } },
        { "grading.filmProfileOpacity alone",[](Look& l){ l.grading.filmProfileOpacity = 0.5f; } },
        { "a disabled LUT", [](Look& l){ l.grading.lutPath = QStringLiteral("/luts/a.cube");
                                         l.grading.lutEnabled = false; } },
        { "a transparent LUT", [](Look& l){ l.grading.lutPath = QStringLiteral("/luts/a.cube");
                                            l.grading.lutOpacity = 0.0f; } },
        // Vignette and grain shape parameters with zero amount.
        { "effects.vignette.midpoint",  [](Look& l){ l.effects.vignette.midpoint = 10.0f; } },
        { "effects.vignette.feather",   [](Look& l){ l.effects.vignette.feather = 10.0f; } },
        { "effects.vignette.roundness", [](Look& l){ l.effects.vignette.roundness = 50.0f; } },
        { "effects.grain.size",         [](Look& l){ l.effects.grain.size = 80.0f; } },
        // Masks and layers that cannot contribute.
        { "a disabled mask", [](Look& l){ LocalAdjustment m; m.enabled = false;
                                          m.exposure = 4.0f; l.localAdjustments.push_back(m); } },
        { "an empty mask",   [](Look& l){ LocalAdjustment m; l.localAdjustments.push_back(m); } },
        { "a disabled layer", [](Look& l){ AdjustmentLayer a; a.enabled = false;
                                           a.adjustmentData.tone.exposure = 2.0f;
                                           l.adjustmentLayers.push_back(a); } },
        { "a transparent layer", [](Look& l){ AdjustmentLayer a; a.opacity = 0.0f;
                                              a.adjustmentData.tone.exposure = 2.0f;
                                              l.adjustmentLayers.push_back(a); } },
        // Metadata is not an edit.
        { "name",          [](Look& l){ l.name = QStringLiteral("Golden hour"); } },
        { "schemaVersion", [](Look& l){ l.schemaVersion = 7; } },
    };

    for (const Edit& e : nonEdits) {
        Look look;
        e.apply(look);
        QVERIFY2(look.isIdentity(),
                 qPrintable(QString("%1 now breaks isIdentity(); if that is intended, the "
                                    "engines and this test both need to know")
                                .arg(QLatin1String(e.what))));
    }
}

void TstLookSerializer::resetReturnsToIdentity()
{
    Look look = makeFullyPopulatedLook();
    const QString name = look.name;
    QVERIFY(!look.isIdentity());

    look.reset();

    QVERIFY(look.isIdentity());
    QVERIFY(look.localAdjustments.empty());
    QVERIFY(look.adjustmentLayers.empty());
    // reset() keeps what identifies the preset and drops what it does.
    QCOMPARE(look.name, name);
    QCOMPARE(look.schemaVersion, 1);

    Look expected;
    expected.name = name;
    const QStringList d = diffLooks(look, expected);
    QVERIFY2(d.isEmpty(), qPrintable(d.join("\n")));

    // A reset Look must also be indistinguishable from a fresh one on disk.
    QCOMPARE(LookSerializer::toJson(look), LookSerializer::toJson(expected));

    // The sub-struct resets, which the UI calls from its per-panel buttons.
    Distinct g;
    HDRParams hdr = makeHdr(g);
    hdr.reset();
    QVERIFY(hdr.isIdentity());
    DetailsParams details = makeDetails(g);
    details.reset();
    QVERIFY(details.isIdentity());
    TransformParams transform = makeTransform(g);
    transform.reset();
    QVERIFY(transform.isIdentity());
    LensParams lens = makeLens(g);
    lens.reset();
    QVERIFY(lens.isIdentity());
    LayerAdjustmentData payload;
    payload.tone = makeTone(g);
    payload.reset();
    QVERIFY(payload.isIdentity());
}

void TstLookSerializer::clampRangesEnforcesDocumentedBounds()
{
    // Same bounds as Look.h documents, checked at both ends. This is the last
    // line of defence for the engines, which are allowed to assume it ran.
    Look look;
    look.hdr.exposureBias         = 500.0f;
    look.hdr.highlightCompression = -5.0f;
    look.hdr.midtonePivot         = 5.0f;
    look.hdr.saturationPreserve   = 500.0f;
    look.tone.exposure   = -500.0f;
    look.tone.contrast   = 500.0f;
    look.tone.highlights = -500.0f;
    look.tone.brightness = 500.0f;
    look.color.whiteBalance.temperature = 500.0f;
    look.color.whiteBalance.tint        = -500.0f;
    look.color.vibrance    = 500.0f;
    look.color.hsl.aqua.hue = -500.0f;
    look.color.rgbMixer.blueOutput.r = -50.0f;
    look.grading.lutOpacity         = 5.0f;
    look.grading.filmProfileOpacity = -5.0f;
    look.grading.globalStrength     = 500.0f;
    look.grading.globalLuminance    = -500.0f;
    look.grading.colorSeparation    = 500.0f;
    look.details.sharpeningAmount = 500.0f;
    look.details.sharpeningRadius = 50.0f;
    look.details.colorNR          = -5.0f;
    look.effects.vignette.amount = -500.0f;
    look.effects.grain.size      = 500.0f;
    look.effects.clarity.amount  = 500.0f;
    look.lens.distortion   = -500.0f;
    look.lens.purpleFringe = -5.0f;
    look.transform.straightenAngle = -50.0f;
    look.transform.rotationDegrees = 190.0f;

    look.clampRanges();

    QCOMPARE(look.hdr.exposureBias, 5.0f);
    QCOMPARE(look.hdr.highlightCompression, 0.0f);
    QCOMPARE(look.hdr.midtonePivot, 1.0f);
    QCOMPARE(look.hdr.saturationPreserve, 100.0f);
    QCOMPARE(look.tone.exposure, -10.0f);
    QCOMPARE(look.tone.contrast, 100.0f);
    QCOMPARE(look.tone.highlights, -100.0f);
    QCOMPARE(look.tone.brightness, 100.0f);
    QCOMPARE(look.color.whiteBalance.temperature, 100.0f);
    QCOMPARE(look.color.whiteBalance.tint, -100.0f);
    QCOMPARE(look.color.vibrance, 100.0f);
    QCOMPARE(look.color.hsl.aqua.hue, -100.0f);
    QCOMPARE(look.color.rgbMixer.blueOutput.r, -2.0f);
    QCOMPARE(look.grading.lutOpacity, 1.0f);
    QCOMPARE(look.grading.filmProfileOpacity, 0.0f);
    QCOMPARE(look.grading.globalStrength, 100.0f);
    QCOMPARE(look.grading.globalLuminance, -100.0f);
    QCOMPARE(look.grading.colorSeparation, 100.0f);
    QCOMPARE(look.details.sharpeningAmount, 150.0f);
    QCOMPARE(look.details.sharpeningRadius, 3.0f);
    QCOMPARE(look.details.colorNR, 0.0f);
    QCOMPARE(look.effects.vignette.amount, -100.0f);
    QCOMPARE(look.effects.grain.size, 100.0f);
    QCOMPARE(look.effects.clarity.amount, 100.0f);
    QCOMPARE(look.lens.distortion, -100.0f);
    QCOMPARE(look.lens.purpleFringe, 0.0f);
    QCOMPARE(look.transform.straightenAngle, -10.0f);
    // Rotation wraps into (-180, +180] rather than clamping.
    QCOMPARE(look.transform.rotationDegrees, -170.0f);
    QVERIFY(nonFiniteFields(look).isEmpty());

    // A curve with fewer than two points cannot be evaluated, so clamping
    // restores the identity ramp rather than leaving the engine to guess.
    // Points outside the unit square are pulled in and re-sorted.
    Look degenerate;
    degenerate.curves.master.points = {{0.5, 0.5}};
    degenerate.curves.red.points.clear();
    degenerate.curves.green.points = {{2.0, 2.0}, {-1.0, -1.0}};
    // Crop values are float-exact so the comparison below is exact too.
    degenerate.transform.cropRect = QRectF(0.75, 0.875, 5.0, 5.0);
    degenerate.clampRanges();
    QVERIFY(degenerate.curves.master.isIdentity());
    QVERIFY(degenerate.curves.red.isIdentity());
    QCOMPARE(degenerate.curves.green.points.size(), size_t(2));
    QCOMPARE(degenerate.curves.green.points[0], QPointF(0.0, 0.0));
    QCOMPARE(degenerate.curves.green.points[1], QPointF(1.0, 1.0));
    QCOMPARE(degenerate.transform.cropRect, QRectF(0.75, 0.875, 0.25, 0.125));

    // Masks and layers are clamped too, not just the top-level sliders.
    Look nested;
    LocalAdjustment mask;
    mask.radius = -5.0f;
    mask.density = 5.0f;
    mask.exposure = 500.0f;
    mask.startPoint = QPointF(-50.0, 50.0);
    nested.localAdjustments.push_back(mask);
    AdjustmentLayer layer;
    layer.opacity = 5.0f;
    layer.adjustmentData.tone.contrast = 500.0f;
    nested.adjustmentLayers.push_back(layer);
    nested.clampRanges();
    QCOMPARE(nested.localAdjustments[0].radius, 1e-4f);
    QCOMPARE(nested.localAdjustments[0].density, 1.0f);
    QCOMPARE(nested.localAdjustments[0].exposure, 10.0f);
    QCOMPARE(nested.localAdjustments[0].startPoint, QPointF(-10.0, 10.0));
    QCOMPARE(nested.adjustmentLayers[0].opacity, 1.0f);
    QCOMPARE(nested.adjustmentLayers[0].adjustmentData.tone.contrast, 100.0f);
}

QTEST_MAIN(TstLookSerializer)
#include "tst_lookserializer.moc"
