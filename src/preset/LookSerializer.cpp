// ==============================================================================
// preset/LookSerializer.cpp
//
// Hardening guarantees (any of these cases returns an error, never crashes):
//   - File doesn't exist / can't be opened
//   - File isn't valid JSON
//   - File's root isn't a JSON object
//   - Any field has the wrong type (e.g. "tone" is a string, "exposure" is an object)
//   - Missing fields default to identity (forward compatibility)
//   - Unknown fields are ignored (forward compatibility)
//
// Schema migration:
//   - schemaVersion is recorded. If a loaded version is NEWER than what we
//     understand, we still try to load; missing-field tolerance handles it.
//   - If we need a one-way migration (e.g. renamed field), add a
//     migrateV1toV2() step in fromJson() before reading fields.
//   - On save we always write the current schemaVersion.
//
// Range clamping: Look::clampRanges() is called on every loaded preset before
// returning, so corrupt-but-parseable files (e.g. exposure = 9999) don't cause
// pipeline damage.
// ==============================================================================
#include "preset/LookSerializer.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QObject>

#include <cmath>

namespace lps {

// Current schema version we author. Bump when making breaking changes.
static constexpr int kCurrentSchemaVersion = 1;

// ============================================================================
// Safe-read helpers
// ============================================================================
namespace {

float readFloat(const QJsonObject& obj, const QString& key, float defaultVal)
{
    const QJsonValue v = obj.value(key);
    if (!v.isDouble()) return defaultVal;
    const double d = v.toDouble(defaultVal);
    // Reject NaN/Inf explicitly — QJsonValue::toDouble wouldn't return them
    // from valid JSON but defensively clamp if they slip in via a text editor.
    if (!std::isfinite(d)) return defaultVal;
    return static_cast<float>(d);
}

QString readString(const QJsonObject& obj, const QString& key)
{
    const QJsonValue v = obj.value(key);
    return v.isString() ? v.toString() : QString();
}

int readInt(const QJsonObject& obj, const QString& key, int defaultVal)
{
    const QJsonValue v = obj.value(key);
    if (!v.isDouble()) return defaultVal;
    return v.toInt(defaultVal);
}

QJsonObject readObject(const QJsonObject& obj, const QString& key)
{
    const QJsonValue v = obj.value(key);
    return v.isObject() ? v.toObject() : QJsonObject();
}

QJsonArray readArray(const QJsonObject& obj, const QString& key)
{
    const QJsonValue v = obj.value(key);
    return v.isArray() ? v.toArray() : QJsonArray();
}

// ---- sub-struct serializers ---------------------------------------------------
QJsonArray pointsToJson(const std::vector<QPointF>& pts)
{
    QJsonArray arr;
    for (const QPointF& p : pts) {
        QJsonArray pair;
        pair.append(p.x());
        pair.append(p.y());
        arr.append(pair);
    }
    return arr;
}

bool pointsFromJson(const QJsonArray& arr, std::vector<QPointF>& out)
{
    out.clear();
    out.reserve(static_cast<size_t>(arr.size()));
    for (const QJsonValue& v : arr) {
        if (!v.isArray()) return false;
        const QJsonArray pair = v.toArray();
        if (pair.size() != 2) return false;
        if (!pair[0].isDouble() || !pair[1].isDouble()) return false;
        const double x = pair[0].toDouble();
        const double y = pair[1].toDouble();
        if (!std::isfinite(x) || !std::isfinite(y)) return false;
        out.emplace_back(x, y);
    }
    return true;
}

QJsonObject hslChannelToJson(const HSLChannel& c)
{
    QJsonObject o;
    o["hue"]        = c.hue;
    o["saturation"] = c.saturation;
    o["luminance"]  = c.luminance;
    return o;
}

HSLChannel hslChannelFromJson(const QJsonObject& o)
{
    HSLChannel c;
    c.hue        = readFloat(o, "hue",        0.0f);
    c.saturation = readFloat(o, "saturation", 0.0f);
    c.luminance  = readFloat(o, "luminance",  0.0f);
    return c;
}

QJsonArray mixerRow(const RGBMixerParams::Row& r)
{
    QJsonArray a; a.append(r.r); a.append(r.g); a.append(r.b); return a;
}

RGBMixerParams::Row mixerRowFromJson(const QJsonArray& a, const RGBMixerParams::Row& fallback)
{
    if (a.size() < 3) return fallback;
    auto readFinite = [](const QJsonValue& v, float def) -> float {
        if (!v.isDouble()) return def;
        const double d = v.toDouble(def);
        return std::isfinite(d) ? static_cast<float>(d) : def;
    };
    RGBMixerParams::Row r;
    r.r = readFinite(a[0], fallback.r);
    r.g = readFinite(a[1], fallback.g);
    r.b = readFinite(a[2], fallback.b);
    return r;
}

} // namespace

// ============================================================================
// toJson
// ============================================================================
QJsonObject LookSerializer::toJson(const Look& look)
{
    QJsonObject root;
    root["name"] = look.name;
    root["schemaVersion"] = kCurrentSchemaVersion;

    // Tone
    {
        QJsonObject t;
        t["exposure"]   = look.tone.exposure;
        t["contrast"]   = look.tone.contrast;
        t["highlights"] = look.tone.highlights;
        t["shadows"]    = look.tone.shadows;
        t["whites"]     = look.tone.whites;
        t["blacks"]     = look.tone.blacks;
        t["brightness"] = look.tone.brightness;
        root["tone"] = t;
    }

    // Color
    {
        QJsonObject c;
        QJsonObject wb;
        wb["temperature"] = look.color.whiteBalance.temperature;
        wb["tint"]        = look.color.whiteBalance.tint;
        c["whiteBalance"] = wb;
        c["vibrance"]   = look.color.vibrance;
        c["saturation"] = look.color.saturation;

        QJsonObject hsl;
        hsl["red"]     = hslChannelToJson(look.color.hsl.red);
        hsl["orange"]  = hslChannelToJson(look.color.hsl.orange);
        hsl["yellow"]  = hslChannelToJson(look.color.hsl.yellow);
        hsl["green"]   = hslChannelToJson(look.color.hsl.green);
        hsl["aqua"]    = hslChannelToJson(look.color.hsl.aqua);
        hsl["blue"]    = hslChannelToJson(look.color.hsl.blue);
        hsl["purple"]  = hslChannelToJson(look.color.hsl.purple);
        hsl["magenta"] = hslChannelToJson(look.color.hsl.magenta);
        c["hsl"] = hsl;

        QJsonObject mix;
        mix["red"]   = mixerRow(look.color.rgbMixer.redOutput);
        mix["green"] = mixerRow(look.color.rgbMixer.greenOutput);
        mix["blue"]  = mixerRow(look.color.rgbMixer.blueOutput);
        c["rgbMixer"] = mix;

        root["color"] = c;
    }

    // Curves
    {
        QJsonObject c;
        c["master"] = pointsToJson(look.curves.master.points);
        c["red"]    = pointsToJson(look.curves.red.points);
        c["green"]  = pointsToJson(look.curves.green.points);
        c["blue"]   = pointsToJson(look.curves.blue.points);
        root["curves"] = c;
    }

    // Grading
    {
        QJsonObject g;
        g["lutPath"]            = look.grading.lutPath;
        g["lutOpacity"]         = look.grading.lutOpacity;
        g["lutEnabled"]         = look.grading.lutEnabled;
        g["filmProfileId"]      = look.grading.filmProfileId;
        g["filmProfileOpacity"] = look.grading.filmProfileOpacity;

        // 3-way color wheels. Stored as a flat list of named floats — a
        // nested per-wheel object would be more structured but this matches
        // the conventions of the rest of this file (flat, terse keys).
        g["shadowsHue"]          = look.grading.shadowsHue;
        g["shadowsSaturation"]   = look.grading.shadowsSaturation;
        g["shadowsStrength"]     = look.grading.shadowsStrength;
        g["shadowsLuminance"]    = look.grading.shadowsLuminance;
        g["midtonesHue"]         = look.grading.midtonesHue;
        g["midtonesSaturation"]  = look.grading.midtonesSaturation;
        g["midtonesStrength"]    = look.grading.midtonesStrength;
        g["midtonesLuminance"]   = look.grading.midtonesLuminance;
        g["highlightsHue"]       = look.grading.highlightsHue;
        g["highlightsSaturation"] = look.grading.highlightsSaturation;
        g["highlightsStrength"]  = look.grading.highlightsStrength;
        g["highlightsLuminance"] = look.grading.highlightsLuminance;
        g["globalHue"]           = look.grading.globalHue;
        g["globalSaturation"]    = look.grading.globalSaturation;
        g["globalStrength"]      = look.grading.globalStrength;
        g["globalLuminance"]     = look.grading.globalLuminance;
        g["balance"]             = look.grading.balance;
        g["blending"]            = look.grading.blending;

        // Advanced grading + filmic look — V1 placeholders. Persist so
        // projects authored now pick up rendering when the engine math
        // lands.
        g["lift"]             = look.grading.lift;
        g["gamma"]            = look.grading.gamma;
        g["gain"]             = look.grading.gain;
        g["offset"]           = look.grading.offset;
        g["filmicContrast"]   = look.grading.filmicContrast;
        g["highlightRolloff"] = look.grading.highlightRolloff;
        g["shadowLift"]       = look.grading.shadowLift;
        g["fadeBlacks"]       = look.grading.fadeBlacks;
        g["colorSeparation"]  = look.grading.colorSeparation;

        root["grading"] = g;
    }

    // Effects
    {
        QJsonObject e;
        QJsonObject v;
        v["amount"]    = look.effects.vignette.amount;
        v["midpoint"]  = look.effects.vignette.midpoint;
        v["feather"]   = look.effects.vignette.feather;
        v["roundness"] = look.effects.vignette.roundness;
        e["vignette"] = v;
        QJsonObject gr;
        gr["amount"] = look.effects.grain.amount;
        gr["size"]   = look.effects.grain.size;
        e["grain"] = gr;
        QJsonObject cl;
        cl["amount"] = look.effects.clarity.amount;
        e["clarity"] = cl;
        root["effects"] = e;
    }

    // Local adjustments — array of mask objects. Each mask serializes as
    // a flat-keys JSON object with all geometry fields present (unused
    // fields per type are still written for forward-compat — cheap, and
    // makes the JSON readable to humans). Type is stored as int so the
    // file format is robust to future enum reordering (we'd map old
    // int values to new on deserialize if that ever happens).
    {
        QJsonArray arr;
        for (const auto& la : look.localAdjustments) {
            QJsonObject m;
            m["name"]    = la.name;
            m["enabled"] = la.enabled;
            m["type"]    = static_cast<int>(la.type);

            // Geometry — fields used per type, but we save all so the
            // file is forward-compatible if a user switches a mask's
            // type later (old field values stick around as defaults).
            QJsonArray sp;     sp << la.startPoint.x() << la.startPoint.y();
            QJsonArray ep;     ep << la.endPoint.x()   << la.endPoint.y();
            QJsonArray ctr;    ctr << la.center.x()    << la.center.y();
            m["startPoint"] = sp;
            m["endPoint"]   = ep;
            m["center"]     = ctr;
            m["radius"]     = la.radius;
            m["feather"]    = la.feather;
            m["invert"]     = la.invert;
            m["density"]    = la.density;
            m["flow"]       = la.flow;

            // Brush stamps — array of [x, y] pairs. V1 doesn't paint, so
            // this is empty by default; serializing it preserves whatever
            // future versions write here.
            QJsonArray stamps;
            for (const QPointF& s : la.brushStamps) {
                QJsonArray pt;
                pt << s.x() << s.y();
                stamps.append(pt);
            }
            m["brushStamps"] = stamps;

            // Adjustment values — flat keys.
            m["exposure"]    = la.exposure;
            m["brightness"]  = la.brightness;
            m["contrast"]    = la.contrast;
            m["saturation"]  = la.saturation;
            m["temperature"] = la.temperature;
            m["tint"]        = la.tint;

            arr.append(m);
        }
        root["localAdjustments"] = arr;
    }

    // Adjustment layers — array of layer objects. Each carries a flat
    // LayerAdjustmentData payload (Tone + Color + Curves + Grading +
    // Effects, NOT a recursive Look) plus compositing controls. We
    // route the payload through a temporary Look so we can reuse the
    // existing per-sub-struct serialization code without duplication.
    // The temporary Look's localAdjustments / adjustmentLayers / name /
    // schemaVersion fields stay default and get ignored on the read side.
    {
        QJsonArray arr;
        for (const auto& al : look.adjustmentLayers) {
            QJsonObject layer;
            layer["name"]       = al.name;
            layer["enabled"]    = al.enabled;
            layer["opacity"]    = al.opacity;
            layer["blendMode"]  = static_cast<int>(al.blendMode);
            layer["maskRef"]    = al.maskRef;

            // Bridge through a temporary Look so the existing toJson()
            // sub-tree code applies. Only the five sub-structs are read
            // back on the deserialize side.
            Look tmp;
            tmp.tone    = al.adjustmentData.tone;
            tmp.color   = al.adjustmentData.color;
            tmp.curves  = al.adjustmentData.curves;
            tmp.grading = al.adjustmentData.grading;
            tmp.effects = al.adjustmentData.effects;
            layer["adjustmentData"] = LookSerializer::toJson(tmp);
            arr.append(layer);
        }
        root["adjustmentLayers"] = arr;
    }

    return root;
}

// ============================================================================
// fromJson — defensive, all-optional, range-clamped on exit
// ============================================================================
bool LookSerializer::fromJson(const QJsonObject& obj, Look& out, QString* errorOut)
{
    out = Look{};

    // ---- Header
    out.name          = readString(obj, "name");
    out.schemaVersion = readInt(obj, "schemaVersion", kCurrentSchemaVersion);

    // Future migrations go here. For now we have only v1.
    // if (out.schemaVersion == 0) { migrateV0toV1(obj); }

    // ---- Tone
    {
        const QJsonObject t = readObject(obj, "tone");
        out.tone.exposure   = readFloat(t, "exposure",   0.0f);
        out.tone.contrast   = readFloat(t, "contrast",   0.0f);
        out.tone.highlights = readFloat(t, "highlights", 0.0f);
        out.tone.shadows    = readFloat(t, "shadows",    0.0f);
        out.tone.whites     = readFloat(t, "whites",     0.0f);
        out.tone.blacks     = readFloat(t, "blacks",     0.0f);
        // Brightness landed after V1; older preset/project files won't
        // have the field — default to 0 so they round-trip as identity
        // for the brightness slider, exactly as if the user hadn't
        // touched it.
        out.tone.brightness = readFloat(t, "brightness", 0.0f);
    }

    // ---- Color
    {
        const QJsonObject c = readObject(obj, "color");

        const QJsonObject wb = readObject(c, "whiteBalance");
        out.color.whiteBalance.temperature = readFloat(wb, "temperature", 0.0f);
        out.color.whiteBalance.tint        = readFloat(wb, "tint",        0.0f);

        out.color.vibrance   = readFloat(c, "vibrance",   0.0f);
        out.color.saturation = readFloat(c, "saturation", 0.0f);

        const QJsonObject h = readObject(c, "hsl");
        auto loadChan = [&](const QString& k, HSLChannel& dst) {
            const QJsonObject o = readObject(h, k);
            if (!o.isEmpty()) dst = hslChannelFromJson(o);
        };
        loadChan("red", out.color.hsl.red);
        loadChan("orange", out.color.hsl.orange);
        loadChan("yellow", out.color.hsl.yellow);
        loadChan("green", out.color.hsl.green);
        loadChan("aqua", out.color.hsl.aqua);
        loadChan("blue", out.color.hsl.blue);
        loadChan("purple", out.color.hsl.purple);
        loadChan("magenta", out.color.hsl.magenta);

        const QJsonObject m = readObject(c, "rgbMixer");
        out.color.rgbMixer.redOutput   = mixerRowFromJson(readArray(m, "red"),   out.color.rgbMixer.redOutput);
        out.color.rgbMixer.greenOutput = mixerRowFromJson(readArray(m, "green"), out.color.rgbMixer.greenOutput);
        out.color.rgbMixer.blueOutput  = mixerRowFromJson(readArray(m, "blue"),  out.color.rgbMixer.blueOutput);
    }

    // ---- Curves
    {
        const QJsonObject c = readObject(obj, "curves");
        auto loadCurve = [&](const QString& k, CurvePoints& dst) {
            const QJsonArray a = readArray(c, k);
            if (a.isEmpty()) return;
            std::vector<QPointF> pts;
            if (pointsFromJson(a, pts) && pts.size() >= 2)
                dst.points = std::move(pts);
        };
        loadCurve("master", out.curves.master);
        loadCurve("red",    out.curves.red);
        loadCurve("green",  out.curves.green);
        loadCurve("blue",   out.curves.blue);
    }

    // ---- Grading
    {
        const QJsonObject g = readObject(obj, "grading");
        out.grading.lutPath            = readString(g, "lutPath");
        out.grading.lutOpacity         = readFloat(g, "lutOpacity", 1.0f);
        // Older files (no lutEnabled key) default to true so a saved LUT
        // path keeps applying — matches V0 behavior.
        out.grading.lutEnabled         = g.value("lutEnabled").toBool(true);
        out.grading.filmProfileId      = readString(g, "filmProfileId");
        out.grading.filmProfileOpacity = readFloat(g, "filmProfileOpacity", 1.0f);

        // 3-way color wheels. Defaults match the struct: 0 hue, 0 sat, 0
        // strength, 0 luminance. Older preset files (no wheels saved)
        // deserialize as identity, which is the right behavior — they
        // round-trip unchanged. Luminance fields landed in V2; older
        // files without them default to 0.
        out.grading.shadowsHue           = readFloat(g, "shadowsHue",           0.0f);
        out.grading.shadowsSaturation    = readFloat(g, "shadowsSaturation",    0.0f);
        out.grading.shadowsStrength      = readFloat(g, "shadowsStrength",      0.0f);
        out.grading.shadowsLuminance     = readFloat(g, "shadowsLuminance",     0.0f);
        out.grading.midtonesHue          = readFloat(g, "midtonesHue",          0.0f);
        out.grading.midtonesSaturation   = readFloat(g, "midtonesSaturation",   0.0f);
        out.grading.midtonesStrength     = readFloat(g, "midtonesStrength",     0.0f);
        out.grading.midtonesLuminance    = readFloat(g, "midtonesLuminance",    0.0f);
        out.grading.highlightsHue        = readFloat(g, "highlightsHue",        0.0f);
        out.grading.highlightsSaturation = readFloat(g, "highlightsSaturation", 0.0f);
        out.grading.highlightsStrength   = readFloat(g, "highlightsStrength",   0.0f);
        out.grading.highlightsLuminance  = readFloat(g, "highlightsLuminance",  0.0f);
        out.grading.globalHue            = readFloat(g, "globalHue",            0.0f);
        out.grading.globalSaturation     = readFloat(g, "globalSaturation",     0.0f);
        out.grading.globalStrength       = readFloat(g, "globalStrength",       0.0f);
        out.grading.globalLuminance      = readFloat(g, "globalLuminance",      0.0f);
        out.grading.balance              = readFloat(g, "balance",              0.0f);
        // blending defaults to 50 (medium softness) when absent — matches
        // the struct default.
        out.grading.blending             = readFloat(g, "blending",             50.0f);

        // Advanced + filmic placeholders. Default to 0 (identity) when
        // missing.
        out.grading.lift             = readFloat(g, "lift",             0.0f);
        out.grading.gamma            = readFloat(g, "gamma",            0.0f);
        out.grading.gain             = readFloat(g, "gain",             0.0f);
        out.grading.offset           = readFloat(g, "offset",           0.0f);
        out.grading.filmicContrast   = readFloat(g, "filmicContrast",   0.0f);
        out.grading.highlightRolloff = readFloat(g, "highlightRolloff", 0.0f);
        out.grading.shadowLift       = readFloat(g, "shadowLift",       0.0f);
        out.grading.fadeBlacks       = readFloat(g, "fadeBlacks",       0.0f);
        out.grading.colorSeparation  = readFloat(g, "colorSeparation",  0.0f);
    }

    // ---- Effects
    {
        const QJsonObject e = readObject(obj, "effects");

        const QJsonObject v = readObject(e, "vignette");
        out.effects.vignette.amount    = readFloat(v, "amount",    0.0f);
        out.effects.vignette.midpoint  = readFloat(v, "midpoint",  50.0f);
        out.effects.vignette.feather   = readFloat(v, "feather",   50.0f);
        out.effects.vignette.roundness = readFloat(v, "roundness", 0.0f);

        const QJsonObject gr = readObject(e, "grain");
        out.effects.grain.amount = readFloat(gr, "amount", 0.0f);
        out.effects.grain.size   = readFloat(gr, "size",   25.0f);

        const QJsonObject cl = readObject(e, "clarity");
        out.effects.clarity.amount = readFloat(cl, "amount", 0.0f);
    }

    // Local adjustments — array of mask objects. Defensive: missing fields
    // default to neutral, unknown enum values clamp to LinearGradient,
    // truncated arrays parse as the points they have. Empty array (or
    // missing key) is the V0 case — older files predate masks and are
    // valid as zero-mask Looks.
    {
        const QJsonArray arr = readArray(obj, "localAdjustments");
        out.localAdjustments.clear();
        out.localAdjustments.reserve(static_cast<size_t>(arr.size()));
        for (const QJsonValue& v : arr) {
            if (!v.isObject()) continue;
            const QJsonObject m = v.toObject();
            LocalAdjustment la;
            la.name    = readString(m, "name");
            la.enabled = m.value("enabled").toBool(true);

            // Type: clamp out-of-range int values to LinearGradient so a
            // hand-edited or future-version file can't crash us with an
            // enum reordering.
            const int typeInt = readInt(m, "type",
                                        static_cast<int>(MaskType::LinearGradient));
            switch (typeInt) {
                case 0: la.type = MaskType::Brush;          break;
                case 2: la.type = MaskType::RadialGradient; break;
                case 1:
                default: la.type = MaskType::LinearGradient; break;
            }

            // Geometry — read [x, y] arrays. Missing/short arrays leave
            // the field at its struct-default value.
            auto readPoint = [&](const QString& key, QPointF& out) {
                const QJsonArray a = readArray(m, key);
                if (a.size() >= 2) out = QPointF(a[0].toDouble(out.x()),
                                                  a[1].toDouble(out.y()));
            };
            readPoint("startPoint", la.startPoint);
            readPoint("endPoint",   la.endPoint);
            readPoint("center",     la.center);
            la.radius  = readFloat(m, "radius",  la.radius);
            la.feather = readFloat(m, "feather", la.feather);
            // V2 fields — older files default to false/1.0 (identity for
            // these modifiers, matching pre-V2 engine behavior).
            la.invert  = m.value("invert").toBool(false);
            la.density = readFloat(m, "density", 1.0f);
            la.flow    = readFloat(m, "flow",    1.0f);

            // Brush stamps.
            la.brushStamps.clear();
            const QJsonArray stamps = readArray(m, "brushStamps");
            la.brushStamps.reserve(stamps.size());
            for (const QJsonValue& sv : stamps) {
                const QJsonArray pa = sv.toArray();
                if (pa.size() >= 2) {
                    la.brushStamps.append(QPointF(pa[0].toDouble(),
                                                   pa[1].toDouble()));
                }
            }

            la.exposure    = readFloat(m, "exposure",    0.0f);
            la.brightness  = readFloat(m, "brightness",  0.0f);
            la.contrast    = readFloat(m, "contrast",    0.0f);
            la.saturation  = readFloat(m, "saturation",  0.0f);
            la.temperature = readFloat(m, "temperature", 0.0f);
            la.tint        = readFloat(m, "tint",        0.0f);

            out.localAdjustments.push_back(std::move(la));
        }
    }

    // Adjustment layers — array of layer objects, each carrying a flat
    // LayerAdjustmentData payload (NOT a recursive Look). Defensive:
    // missing fields default to neutral, unknown blend modes clamp to
    // Normal, missing adjustmentData becomes an identity payload.
    //
    // We bridge through a temporary Look on read so the existing
    // fromJson() sub-tree code applies. Only the five sub-structs are
    // copied out — the temp's localAdjustments / adjustmentLayers /
    // name / schemaVersion are intentionally discarded.
    {
        const QJsonArray arr = readArray(obj, "adjustmentLayers");
        out.adjustmentLayers.clear();
        out.adjustmentLayers.reserve(static_cast<size_t>(arr.size()));
        for (const QJsonValue& v : arr) {
            if (!v.isObject()) continue;
            const QJsonObject lo = v.toObject();
            AdjustmentLayer al;
            al.name    = readString(lo, "name");
            al.enabled = lo.value("enabled").toBool(true);
            al.opacity = readFloat(lo, "opacity", 1.0f);
            const int bmInt = readInt(lo, "blendMode",
                                      static_cast<int>(BlendMode::Normal));
            // Clamp out-of-range blend mode ints to Normal so future
            // values don't crash. The known range is 0..10 in V1.
            if (bmInt < 0 || bmInt > 10) {
                al.blendMode = BlendMode::Normal;
            } else {
                al.blendMode = static_cast<BlendMode>(bmInt);
            }
            al.maskRef = readString(lo, "maskRef");

            // Read into a temporary Look, then copy the five sub-structs
            // into the flat payload. fromJson is defensively parsed —
            // missing keys produce identity values.
            const QJsonObject inner = readObject(lo, "adjustmentData");
            Look tmp;
            LookSerializer::fromJson(inner, tmp, nullptr);
            al.adjustmentData.tone    = tmp.tone;
            al.adjustmentData.color   = tmp.color;
            al.adjustmentData.curves  = tmp.curves;
            al.adjustmentData.grading = tmp.grading;
            al.adjustmentData.effects = tmp.effects;

            out.adjustmentLayers.push_back(std::move(al));
        }
    }

    // Final safety: enforce documented ranges so corrupt files can't damage
    // the pipeline (e.g. exposure = 9999 -> clamped to +10).
    out.clampRanges();

    if (errorOut) errorOut->clear();
    return true;
}

// ============================================================================
// File I/O
// ============================================================================
SaveResult LookSerializer::saveToFile(const Look& look, const QString& filePath)
{
    SaveResult r;
    QFile f(filePath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        r.errorMessage = QObject::tr("Cannot open %1 for writing: %2")
            .arg(filePath, f.errorString());
        return r;
    }
    const QJsonDocument doc(toJson(look));
    const QByteArray bytes = doc.toJson(QJsonDocument::Indented);
    if (f.write(bytes) != bytes.size()) {
        r.errorMessage = QObject::tr("Write truncated for %1").arg(filePath);
        return r;
    }
    f.close();
    r.ok = true;
    return r;
}

LoadResult LookSerializer::loadFromFile(const QString& filePath)
{
    LoadResult r;
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        r.errorMessage = QObject::tr("Cannot open %1: %2")
            .arg(filePath, f.errorString());
        return r;
    }
    const QByteArray bytes = f.readAll();
    f.close();

    QJsonParseError parseErr;
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &parseErr);
    if (parseErr.error != QJsonParseError::NoError) {
        r.errorMessage = QObject::tr("Invalid JSON in %1: %2 (offset %3)")
            .arg(filePath, parseErr.errorString()).arg(parseErr.offset);
        return r;
    }
    if (!doc.isObject()) {
        r.errorMessage = QObject::tr("Preset %1 root is not an object").arg(filePath);
        return r;
    }

    QString err;
    if (!fromJson(doc.object(), r.look, &err)) {
        r.errorMessage = err;
        return r;
    }
    r.ok = true;
    return r;
}

} // namespace lps
