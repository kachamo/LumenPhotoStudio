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
        g["filmProfileId"]      = look.grading.filmProfileId;
        g["filmProfileOpacity"] = look.grading.filmProfileOpacity;

        // 3-way color wheels. Stored as a flat list of named floats — a
        // nested per-wheel object would be more structured but this matches
        // the conventions of the rest of this file (flat, terse keys).
        g["shadowsHue"]          = look.grading.shadowsHue;
        g["shadowsSaturation"]   = look.grading.shadowsSaturation;
        g["shadowsStrength"]     = look.grading.shadowsStrength;
        g["midtonesHue"]         = look.grading.midtonesHue;
        g["midtonesSaturation"]  = look.grading.midtonesSaturation;
        g["midtonesStrength"]    = look.grading.midtonesStrength;
        g["highlightsHue"]       = look.grading.highlightsHue;
        g["highlightsSaturation"] = look.grading.highlightsSaturation;
        g["highlightsStrength"]  = look.grading.highlightsStrength;
        g["globalHue"]           = look.grading.globalHue;
        g["globalSaturation"]    = look.grading.globalSaturation;
        g["globalStrength"]      = look.grading.globalStrength;
        g["balance"]             = look.grading.balance;
        g["blending"]            = look.grading.blending;

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
        out.grading.filmProfileId      = readString(g, "filmProfileId");
        out.grading.filmProfileOpacity = readFloat(g, "filmProfileOpacity", 1.0f);

        // 3-way color wheels. Defaults match the struct: 0 hue, 0 sat, 0
        // strength. Older preset files (no wheels saved) deserialize as
        // identity, which is the right behavior — they round-trip
        // unchanged.
        out.grading.shadowsHue           = readFloat(g, "shadowsHue",           0.0f);
        out.grading.shadowsSaturation    = readFloat(g, "shadowsSaturation",    0.0f);
        out.grading.shadowsStrength      = readFloat(g, "shadowsStrength",      0.0f);
        out.grading.midtonesHue          = readFloat(g, "midtonesHue",          0.0f);
        out.grading.midtonesSaturation   = readFloat(g, "midtonesSaturation",   0.0f);
        out.grading.midtonesStrength     = readFloat(g, "midtonesStrength",     0.0f);
        out.grading.highlightsHue        = readFloat(g, "highlightsHue",        0.0f);
        out.grading.highlightsSaturation = readFloat(g, "highlightsSaturation", 0.0f);
        out.grading.highlightsStrength   = readFloat(g, "highlightsStrength",   0.0f);
        out.grading.globalHue            = readFloat(g, "globalHue",            0.0f);
        out.grading.globalSaturation     = readFloat(g, "globalSaturation",     0.0f);
        out.grading.globalStrength       = readFloat(g, "globalStrength",       0.0f);
        out.grading.balance              = readFloat(g, "balance",              0.0f);
        // blending defaults to 50 (medium softness) when absent — matches
        // the struct default.
        out.grading.blending             = readFloat(g, "blending",             50.0f);
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
