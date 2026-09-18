// Mari Paint — 브러시 라이브러리 구현 (include/mari/agent/brush_library.hpp)
#include <mari/agent/brush_library.hpp>

#include <mari/agent/session.hpp>
#include <mari/agent/view.hpp>
#include <mari/core/fs.hpp>
#include <mari/io/brush/png_gray.hpp>
#include <mari/ora/image.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace mari::agent {

using brush::BrushTexture;
using brush::BrushTip;
using brush::ColorDynamics;
using brush::CurvePoint;
using brush::DualBrush;
using brush::DynamicInput;
using brush::DynamicLink;
using brush::DynamicOutput;
using brush::GrayImage;
using brush::MariBrushPreset;
using brush::ProceduralShape;
using brush::TipKind;

namespace {

constexpr int kFormatVersion = 1;

// ── base64 ───────────────────────────────────────────────────────────────

Result<std::vector<u8>> base64Decode(std::string_view text) {
    static const auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<u8> out;
    out.reserve(text.size() * 3 / 4);
    u32 acc = 0;
    int bits = 0;
    for (const char c : text) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        const int v = value(c);
        if (v < 0) return Err("base64 가 아니다", ErrorCode::ParseError);
        acc = (acc << 6) | static_cast<u32>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<u8>((acc >> bits) & 0xFFu));
        }
    }
    return Ok(std::move(out));
}

// ── GrayImage ↔ JSON(PNG base64) ─────────────────────────────────────────

Json grayToJson(const GrayImage& g) {
    if (g.empty()) return Json::null();
    ora::Image8 img = ora::Image8::make(g.width, g.height);
    for (usize i = 0; i < g.pixels.size(); ++i) {
        img.pixels[i * 4] = img.pixels[i * 4 + 1] = img.pixels[i * 4 + 2] = g.pixels[i];
        img.pixels[i * 4 + 3] = 255;
    }
    const Result<std::vector<u8>> png = ora::encodePng(img, 6);
    if (!png.ok()) return Json::null();
    Json j = Json::object();
    j.set("width", Json::integer(g.width));
    j.set("height", Json::integer(g.height));
    j.set("png", Json::string(base64Encode(png.value().data(), png.value().size())));
    return j;
}

Result<GrayImage> grayFromJson(const Json& j) {
    if (j.isNull()) return Ok(GrayImage{});
    if (!j.isObject() || !j["png"].isString()) return Err("비트맵 항목이 잘못됐다", ErrorCode::ParseError);
    Result<std::vector<u8>> bytes = base64Decode(j["png"].asString());
    if (!bytes.ok()) return bytes.error();
    return io::brush::decodePngGray(bytes.value().data(), bytes.value().size(), false, nullptr);
}

// ── 팁 · 텍스처 · 동적 ────────────────────────────────────────────────────

const char* shapeName(ProceduralShape s) {
    switch (s) {
    case ProceduralShape::Square:  return "square";
    case ProceduralShape::Diamond: return "diamond";
    default:                       return "circle";
    }
}
ProceduralShape shapeFromName(const std::string& s) {
    if (s == "square") return ProceduralShape::Square;
    if (s == "diamond") return ProceduralShape::Diamond;
    return ProceduralShape::Circle;
}

Json tipToJson(const BrushTip& t) {
    Json j = Json::object();
    j.set("kind", Json::string(t.kind == TipKind::Bitmap ? "bitmap" : "procedural"));
    j.set("shape", Json::string(shapeName(t.shape)));
    j.set("diameter", Json::number(t.diameter));
    j.set("angle", Json::number(t.angle));
    j.set("aspectRatio", Json::number(t.aspectRatio));
    j.set("hardness", Json::number(t.hardness));
    if (t.kind == TipKind::Bitmap) j.set("bitmap", grayToJson(t.bitmap));
    return j;
}

Result<BrushTip> tipFromJson(const Json& j) {
    BrushTip t;
    if (!j.isObject()) return Ok(t);
    t.kind = j["kind"].asString() == "bitmap" ? TipKind::Bitmap : TipKind::Procedural;
    t.shape = shapeFromName(j["shape"].asString());
    t.diameter = static_cast<f32>(j["diameter"].asNumber(t.diameter));
    t.angle = static_cast<f32>(j["angle"].asNumber(0.0));
    t.aspectRatio = static_cast<f32>(j["aspectRatio"].asNumber(1.0));
    t.hardness = static_cast<f32>(j["hardness"].asNumber(1.0));
    if (t.kind == TipKind::Bitmap) {
        Result<GrayImage> g = grayFromJson(j["bitmap"]);
        if (!g.ok()) return g.error();
        t.bitmap = std::move(g).value();
        if (t.bitmap.empty()) t.kind = TipKind::Procedural;
    }
    return Ok(std::move(t));
}

DynamicInput inputFromName(const std::string& s) {
    for (int i = 0; i < brush::kDynamicInputCount; ++i) {
        if (s == brush::dynamicInputName(static_cast<DynamicInput>(i))) return static_cast<DynamicInput>(i);
    }
    return DynamicInput::Pressure;
}
DynamicOutput outputFromName(const std::string& s) {
    for (int i = 0; i < brush::kDynamicOutputCount; ++i) {
        if (s == brush::dynamicOutputName(static_cast<DynamicOutput>(i))) return static_cast<DynamicOutput>(i);
    }
    return DynamicOutput::Size;
}

} // namespace

// ── 공개 API ──────────────────────────────────────────────────────────────

std::string defaultBrushDir() {
    std::filesystem::path dir;
    if (const char* env = std::getenv("MARI_BRUSH_DIR"); env != nullptr && *env != '\0') {
        dir = fsPath(env);
    } else {
#ifdef _WIN32
        const char* base = std::getenv("LOCALAPPDATA");
        dir = fsPath(base != nullptr ? base : ".") / "Mari" / "Mari Paint" / "brushes";
#else
        const char* home = std::getenv("HOME");
        dir = fsPath(home != nullptr ? home : ".") / ".local" / "share" / "mari-paint" / "brushes";
#endif
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return pathToUtf8(dir);
}

Json presetToJson(const MariBrushPreset& p) {
    Json j = Json::object();
    j.set("format", Json::string("mari-brush"));
    j.set("version", Json::integer(kFormatVersion));
    j.set("name", Json::string(p.name));
    j.set("sourceFormat", Json::string(p.sourceFormat));
    j.set("sourceId", Json::string(p.sourceId));
    j.set("engine", Json::string(p.engine));
    j.set("tip", tipToJson(p.tip));
    j.set("spacing", Json::number(p.spacing));
    j.set("opacity", Json::number(p.opacity));
    j.set("flow", Json::number(p.flow));
    j.set("blendMode", Json::string(blendModeName(p.blendMode)));
    Json dyn = Json::array();
    for (const DynamicLink& d : p.dynamics) {
        Json l = Json::object();
        l.set("input", Json::string(brush::dynamicInputName(d.input)));
        l.set("output", Json::string(brush::dynamicOutputName(d.output)));
        l.set("amount", Json::number(d.amount));
        Json pts = Json::array();
        for (const CurvePoint& c : d.curve.points) {
            Json pt = Json::array();
            pt.push(Json::number(c.x));
            pt.push(Json::number(c.y));
            pts.push(std::move(pt));
        }
        l.set("curve", std::move(pts));
        dyn.push(std::move(l));
    }
    j.set("dynamics", std::move(dyn));
    if (p.texture.has_value()) {
        Json t = Json::object();
        t.set("image", grayToJson(p.texture->image));
        t.set("scale", Json::number(p.texture->scale));
        t.set("depth", Json::number(p.texture->depth));
        t.set("blendMode", Json::string(blendModeName(p.texture->blendMode)));
        t.set("anchoredToCanvas", Json::boolean(p.texture->anchoredToCanvas));
        j.set("texture", std::move(t));
    }
    if (p.dual.has_value()) {
        Json d = Json::object();
        d.set("tip", tipToJson(p.dual->tip));
        d.set("spacing", Json::number(p.dual->spacing));
        d.set("scatter", Json::number(p.dual->scatter));
        d.set("count", Json::integer(p.dual->count));
        d.set("blendMode", Json::string(blendModeName(p.dual->blendMode)));
        j.set("dual", std::move(d));
    }
    {
        const ColorDynamics& c = p.colorDynamics;
        Json cd = Json::object();
        cd.set("fgBgJitter", Json::number(c.fgBgJitter));
        cd.set("hueJitter", Json::number(c.hueJitter));
        cd.set("saturationJitter", Json::number(c.saturationJitter));
        cd.set("brightnessJitter", Json::number(c.brightnessJitter));
        cd.set("purity", Json::number(c.purity));
        cd.set("perTip", Json::boolean(c.perTip));
        j.set("colorDynamics", std::move(cd));
    }
    j.set("scatterCount", Json::integer(p.scatterCount));
    j.set("wetEdges", Json::boolean(p.wetEdges));
    j.set("noise", Json::number(p.noise));
    j.set("airbrush", Json::boolean(p.airbrush));
    Json extra = Json::object();
    for (const auto& [k, v] : p.extraParams) extra.set(k, Json::number(v));
    j.set("extraParams", std::move(extra));
    return j;
}

Result<MariBrushPreset> presetFromJson(const Json& j) {
    if (!j.isObject() || j["format"].asString() != "mari-brush")
        return Err("mari-brush 형식이 아니다", ErrorCode::ParseError);
    if (j["version"].asInt(1) > kFormatVersion)
        return Err("더 새 버전의 브러시 파일이다(" + std::to_string(j["version"].asInt()) + ")", ErrorCode::Unsupported);
    MariBrushPreset p;
    p.name = j["name"].asString();
    p.sourceFormat = j["sourceFormat"].isString() ? j["sourceFormat"].asString() : std::string("native");
    p.sourceId = j["sourceId"].asString();
    p.engine = j["engine"].asString();
    Result<BrushTip> tip = tipFromJson(j["tip"]);
    if (!tip.ok()) return tip.error();
    p.tip = std::move(tip).value();
    p.spacing = static_cast<f32>(j["spacing"].asNumber(0.1));
    p.opacity = static_cast<f32>(j["opacity"].asNumber(1.0));
    p.flow = static_cast<f32>(j["flow"].asNumber(1.0));
    if (j["blendMode"].isString()) {
        const Result<BlendMode> m = blendModeFromName(j["blendMode"].asString());
        if (m.ok()) p.blendMode = m.value();
    }
    const Json& dyn = j["dynamics"];
    for (usize i = 0; i < dyn.size(); ++i) {
        const Json& l = dyn.at(i);
        DynamicLink d;
        d.input = inputFromName(l["input"].asString());
        d.output = outputFromName(l["output"].asString());
        d.amount = static_cast<f32>(l["amount"].asNumber(1.0));
        const Json& pts = l["curve"];
        for (usize k = 0; k < pts.size(); ++k) {
            const Json& pt = pts.at(k);
            if (pt.isArray() && pt.size() == 2)
                d.curve.points.push_back(CurvePoint{static_cast<f32>(pt.at(0).asNumber()), static_cast<f32>(pt.at(1).asNumber())});
        }
        p.dynamics.push_back(std::move(d));
    }
    if (j["texture"].isObject()) {
        const Json& t = j["texture"];
        BrushTexture tex;
        Result<GrayImage> img = grayFromJson(t["image"]);
        if (!img.ok()) return img.error();
        tex.image = std::move(img).value();
        tex.scale = static_cast<f32>(t["scale"].asNumber(1.0));
        tex.depth = static_cast<f32>(t["depth"].asNumber(1.0));
        if (t["blendMode"].isString()) {
            const Result<BlendMode> m = blendModeFromName(t["blendMode"].asString());
            if (m.ok()) tex.blendMode = m.value();
        }
        tex.anchoredToCanvas = t["anchoredToCanvas"].asBool(true);
        if (!tex.image.empty()) p.texture = std::move(tex);
    }
    if (j["dual"].isObject()) {
        const Json& d = j["dual"];
        DualBrush dual;
        Result<BrushTip> dt = tipFromJson(d["tip"]);
        if (!dt.ok()) return dt.error();
        dual.tip = std::move(dt).value();
        dual.spacing = static_cast<f32>(d["spacing"].asNumber(0.25));
        dual.scatter = static_cast<f32>(d["scatter"].asNumber(0.0));
        dual.count = static_cast<i32>(d["count"].asInt(1));
        if (d["blendMode"].isString()) {
            const Result<BlendMode> m = blendModeFromName(d["blendMode"].asString());
            if (m.ok()) dual.blendMode = m.value();
        }
        p.dual = std::move(dual);
    }
    if (j["colorDynamics"].isObject()) {
        const Json& c = j["colorDynamics"];
        p.colorDynamics.fgBgJitter = static_cast<f32>(c["fgBgJitter"].asNumber(0.0));
        p.colorDynamics.hueJitter = static_cast<f32>(c["hueJitter"].asNumber(0.0));
        p.colorDynamics.saturationJitter = static_cast<f32>(c["saturationJitter"].asNumber(0.0));
        p.colorDynamics.brightnessJitter = static_cast<f32>(c["brightnessJitter"].asNumber(0.0));
        p.colorDynamics.purity = static_cast<f32>(c["purity"].asNumber(0.0));
        p.colorDynamics.perTip = c["perTip"].asBool(true);
    }
    p.scatterCount = static_cast<i32>(j["scatterCount"].asInt(1));
    p.wetEdges = j["wetEdges"].asBool(false);
    p.noise = static_cast<f32>(j["noise"].asNumber(0.0));
    p.airbrush = j["airbrush"].asBool(false);
    const Json& extra = j["extraParams"];
    for (const std::string& k : extra.keys()) p.extraParams.emplace_back(k, static_cast<f32>(extra[k].asNumber()));
    return Ok(std::move(p));
}

Result<void> savePresetFile(const std::string& path, const MariBrushPreset& p) {
    const std::string text = presetToJson(p).dump(2);
    std::FILE* f = fopenUtf8(path, "wb");
    if (f == nullptr) return Err("브러시 파일을 열 수 없다: " + path, ErrorCode::IoError);
    const usize wrote = std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
    if (wrote != text.size()) return Err("브러시 파일을 다 쓰지 못했다: " + path, ErrorCode::IoError);
    return Ok();
}

Result<MariBrushPreset> loadPresetFile(const std::string& path) {
    std::FILE* f = fopenUtf8(path, "rb");
    if (f == nullptr) return Err("브러시 파일을 열 수 없다: " + path, ErrorCode::IoError);
    std::string text;
    char buf[8192];
    for (;;) {
        const usize n = std::fread(buf, 1, sizeof buf, f);
        if (n == 0) break;
        text.append(buf, n);
    }
    std::fclose(f);
    Result<Json> j = Json::parse(text);
    if (!j.ok()) return j.error();
    return presetFromJson(j.value());
}

std::vector<MariBrushPreset> loadPresetDir(const std::string& dir, std::vector<std::string>* skipped) {
    std::vector<MariBrushPreset> out;
    std::error_code ec;
    std::vector<std::filesystem::path> files;
    for (const auto& e : std::filesystem::directory_iterator(fsPath(dir), ec)) {
        if (e.is_regular_file(ec) && e.path().extension() == ".mbp") files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    for (const auto& path : files) {
        Result<MariBrushPreset> p = loadPresetFile(pathToUtf8(path));
        if (p.ok()) {
            out.push_back(std::move(p).value());
        } else if (skipped != nullptr) {
            skipped->push_back(pathToUtf8(path) + ": " + p.message());
        }
    }
    return out;
}

std::string presetFilePath(const std::string& dir, const std::string& name) {
    std::string slug;
    for (const char c : name) {
        const bool bad = c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
                         c == '>' || c == '|' || static_cast<unsigned char>(c) < 32;
        slug.push_back(bad ? '_' : c);
    }
    if (slug.empty()) slug = "brush";
    const std::filesystem::path base = fsPath(dir);
    std::filesystem::path p = base / fsPath(slug + ".mbp");
    std::error_code ec;
    for (int n = 2; std::filesystem::exists(p, ec) && n < 1000; ++n) {
        p = base / fsPath(slug + "-" + std::to_string(n) + ".mbp");
    }
    return pathToUtf8(p);
}

Result<void> removePresetFile(const std::string& dir, const std::string& name) {
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(fsPath(dir), ec)) {
        if (!e.is_regular_file(ec) || e.path().extension() != ".mbp") continue;
        Result<MariBrushPreset> p = loadPresetFile(pathToUtf8(e.path()));
        if (p.ok() && p.value().name == name) {
            std::filesystem::remove(e.path(), ec);
            if (ec) return Err("브러시 파일을 지우지 못했다: " + pathToUtf8(e.path()), ErrorCode::IoError);
        }
    }
    return Ok();
}

} // namespace mari::agent
