// Mari Paint — 시각 피드백 구현. 선언은 include/mari/agent/view.hpp.
//
// 🔴 PNG 인코더는 io/ora 것을 재사용한다. 두 번째 구현을 만들지 않는다(docs/05 2.3 정신).
#include <mari/agent/view.hpp>

#include <mari/core/compositor.hpp>
#include <mari/ora/image.hpp>

#include <algorithm>

namespace mari::agent {
namespace {

constexpr char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/// "layer:12" / "region:[0,0,8,8]" 처럼 접두사 뒤에 인자가 붙은 문자열을 가른다.
bool splitPrefixed(const std::string& s, const char* prefix, std::string& rest) {
    const usize n = std::char_traits<char>::length(prefix);
    if (s.size() <= n || s.compare(0, n, prefix) != 0 || s[n] != ':') {
        return false;
    }
    rest = s.substr(n + 1);
    return true;
}

Result<ViewSpec> fromString(const std::string& s) {
    ViewSpec out;
    if (s == "none") {
        out.mode = ViewMode::None;
        return Ok(out);
    }
    if (s == "dirty") {
        out.mode = ViewMode::Dirty;
        return Ok(out);
    }
    if (s == "full") {
        out.mode = ViewMode::Full;
        return Ok(out);
    }
    std::string rest;
    if (splitPrefixed(s, "layer", rest)) {
        Result<Json> id = Json::parse(rest);
        if (!id.ok() || !id.value().isNumber() || id.value().asInt() <= 0) {
            return Err("view \"layer:<id>\" 의 id 가 양의 정수가 아니다: " + s,
                       ErrorCode::InvalidArgument);
        }
        out.mode = ViewMode::Layer;
        out.layer = static_cast<LayerId>(id.value().asInt());
        return Ok(out);
    }
    if (splitPrefixed(s, "region", rest)) {
        Result<Json> arr = Json::parse(rest);
        if (!arr.ok()) {
            return Err("view \"region:[x,y,w,h]\" 를 읽을 수 없다: " + s,
                       ErrorCode::InvalidArgument);
        }
        Result<Rect> r = rectFromJson(arr.value());
        if (!r.ok()) {
            return r.error();
        }
        out.mode = ViewMode::Region;
        out.region = r.value();
        return Ok(out);
    }
    return Err("모르는 view 모드다: \"" + s +
                   "\" (none | dirty | full | layer:<id> | region:[x,y,w,h])",
               ErrorCode::InvalidArgument);
}

} // namespace

const char* viewModeName(ViewMode m) noexcept {
    switch (m) {
    case ViewMode::None:
        return "none";
    case ViewMode::Dirty:
        return "dirty";
    case ViewMode::Full:
        return "full";
    case ViewMode::Layer:
        return "layer";
    case ViewMode::Region:
        return "region";
    }
    return "dirty";
}

Result<ViewSpec> parseViewSpec(const Json& v) {
    ViewSpec out;
    if (v.isNull()) {
        return Ok(out); // 안 줬으면 기본값 — 기본이 "보여 준다" 여야 한다.
    }
    if (v.isString()) {
        return fromString(v.asString());
    }
    if (!v.isObject()) {
        return Err("view 는 문자열이거나 객체여야 한다", ErrorCode::InvalidArgument);
    }

    // 🔴 모르는 키를 거절한다. 오타가 조용히 무시되면 AI 는 왜 안 보이는지 못 찾는다.
    static const char* kKnown[] = {"mode", "max", "layer", "region", "pngLevel"};
    for (const std::string& k : v.keys()) {
        bool known = false;
        for (const char* kk : kKnown) {
            known = known || k == kk;
        }
        if (!known) {
            return Err("view 에 모르는 키가 있다: \"" + k +
                           "\" (mode | max | layer | region | pngLevel)",
                       ErrorCode::InvalidArgument);
        }
    }

    const Json& mode = v["mode"];
    if (mode.isString()) {
        const std::string& m = mode.asString();
        if (m == "layer" || m == "region") {
            out.mode = (m == "layer") ? ViewMode::Layer : ViewMode::Region;
        } else {
            Result<ViewSpec> base = fromString(m);
            if (!base.ok()) {
                return base;
            }
            out = base.value();
        }
    } else if (!mode.isNull()) {
        return Err("view.mode 는 문자열이어야 한다", ErrorCode::InvalidArgument);
    }

    if (v.has("layer")) {
        const Json& l = v["layer"];
        if (!l.isNumber() || l.asInt() <= 0) {
            return Err("view.layer 는 양의 정수 레이어 id 여야 한다", ErrorCode::InvalidArgument);
        }
        out.layer = static_cast<LayerId>(l.asInt());
        if (out.mode != ViewMode::Layer && !mode.isString()) {
            out.mode = ViewMode::Layer;
        }
    }
    if (v.has("region")) {
        Result<Rect> r = rectFromJson(v["region"]);
        if (!r.ok()) {
            return r.error();
        }
        out.region = r.value();
        if (out.mode != ViewMode::Region && !mode.isString()) {
            out.mode = ViewMode::Region;
        }
    }
    if (out.mode == ViewMode::Layer && out.layer == kInvalidLayerId) {
        return Err("view.mode 가 layer 인데 layer id 가 없다", ErrorCode::InvalidArgument);
    }
    if (out.mode == ViewMode::Region && out.region.isEmpty()) {
        return Err("view.mode 가 region 인데 region 이 비었다", ErrorCode::InvalidArgument);
    }
    if (v.has("max")) {
        const Json& m = v["max"];
        if (!m.isNumber() || m.asInt() < 1 || m.asInt() > kMaxViewSide) {
            return Err("view.max 는 1..4096 이어야 한다", ErrorCode::InvalidArgument);
        }
        out.max = static_cast<i32>(m.asInt());
    }
    if (v.has("pngLevel")) {
        const Json& p = v["pngLevel"];
        if (!p.isNumber() || p.asInt() < 0 || p.asInt() > 9) {
            return Err("view.pngLevel 은 0..9 여야 한다", ErrorCode::InvalidArgument);
        }
        out.pngLevel = static_cast<int>(p.asInt());
    }
    return Ok(out);
}

Rect dirtyAreaOf(const DirtyTiles& tiles) { return dirtyTilesBounds(tiles); }

std::string base64Encode(const u8* data, usize len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    usize i = 0;
    while (i + 3 <= len) {
        const u32 v = (static_cast<u32>(data[i]) << 16) | (static_cast<u32>(data[i + 1]) << 8) |
                      static_cast<u32>(data[i + 2]);
        out.push_back(kB64[(v >> 18) & 0x3Fu]);
        out.push_back(kB64[(v >> 12) & 0x3Fu]);
        out.push_back(kB64[(v >> 6) & 0x3Fu]);
        out.push_back(kB64[v & 0x3Fu]);
        i += 3;
    }
    if (i + 1 == len) {
        const u32 v = static_cast<u32>(data[i]) << 16;
        out.push_back(kB64[(v >> 18) & 0x3Fu]);
        out.push_back(kB64[(v >> 12) & 0x3Fu]);
        out += "==";
    } else if (i + 2 == len) {
        const u32 v = (static_cast<u32>(data[i]) << 16) | (static_cast<u32>(data[i + 1]) << 8);
        out.push_back(kB64[(v >> 18) & 0x3Fu]);
        out.push_back(kB64[(v >> 12) & 0x3Fu]);
        out.push_back(kB64[(v >> 6) & 0x3Fu]);
        out.push_back('=');
    }
    return out;
}

Result<std::vector<u8>> base64Decode(std::string_view text) {
    auto valueOf = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') {
            return c - 'A';
        }
        if (c >= 'a' && c <= 'z') {
            return c - 'a' + 26;
        }
        if (c >= '0' && c <= '9') {
            return c - '0' + 52;
        }
        if (c == '+') {
            return 62;
        }
        if (c == '/') {
            return 63;
        }
        return -1;
    };
    std::vector<u8> out;
    out.reserve(text.size() / 4 * 3);
    u32 acc = 0;
    int bits = 0;
    usize pad = 0;
    for (const char c : text) {
        if (c == '\n' || c == '\r' || c == ' ') {
            continue;
        }
        if (c == '=') {
            ++pad;
            continue;
        }
        const int d = valueOf(c);
        if (d < 0 || pad != 0) {
            return Err("base64 가 아니다", ErrorCode::ParseError);
        }
        acc = (acc << 6) | static_cast<u32>(d);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<u8>((acc >> bits) & 0xFFu));
        }
    }
    if (pad > 2) {
        return Err("base64 패딩이 이상하다", ErrorCode::ParseError);
    }
    return Ok(std::move(out));
}

Result<ViewResult> renderView(const LayerTree& tree, const ViewSpec& spec, const Rect& dirtyArea) {
    ViewResult out;
    if (spec.mode == ViewMode::None) {
        return Ok(std::move(out));
    }

    const Size canvas = tree.canvasSize();
    const Rect canvasRect{0, 0, canvas.width, canvas.height};

    const Layer* layer = nullptr;
    Rect area{};
    switch (spec.mode) {
    case ViewMode::Dirty:
        // 안 바뀌었으면 이미지를 만들지 않는다. "변화 없음"도 정보다.
        area = dirtyArea.intersected(canvasRect);
        break;
    case ViewMode::Full:
        area = canvasRect;
        break;
    case ViewMode::Region:
        area = spec.region.intersected(canvasRect);
        if (area.isEmpty()) {
            return Err("region 이 캔버스 밖이다", ErrorCode::InvalidArgument);
        }
        break;
    case ViewMode::Layer: {
        const LayerPtr l = tree.find(spec.layer);
        if (!l) {
            return Err("view 가 가리키는 레이어가 없다: " + std::to_string(spec.layer),
                       ErrorCode::NotFound);
        }
        if (l->tiles() == nullptr) {
            return Err("그룹 레이어에는 픽셀이 없다 — view:layer 로 볼 수 없다",
                       ErrorCode::Unsupported);
        }
        layer = l.get();
        // 레이어는 캔버스 밖으로 나갈 수 있다. 있는 그대로 보여 준다(자르지 않는다).
        area = l->tiles()->bounds();
        break;
    }
    case ViewMode::None:
        break;
    }

    if (area.isEmpty()) {
        out.area = Rect{};
        return Ok(std::move(out)); // 보여 줄 게 없다. 이미지 없이 돌려준다.
    }
    out.area = area;

    ora::Image8 img;
    if (layer != nullptr) {
        Result<ora::Image8> r = ora::readRegion(*layer->tiles(), area);
        if (!r.ok()) {
            return r.error();
        }
        img = std::move(r).value();
    } else {
        img = ora::Image8::make(area.width, area.height);
        const Result<void> c = compositeArea(tree, area, img.pixels.data(), img.stride());
        if (!c.ok()) {
            return c.error();
        }
    }

    const i32 srcW = img.width;
    const i32 srcH = img.height;
    ora::Image8 shown = ora::downscaleToFit(img, spec.max);
    const f64 scale = srcW > 0 ? static_cast<f64>(shown.width) / static_cast<f64>(srcW) : 1.0;

    Result<std::vector<u8>> png = ora::encodePng(shown, spec.pngLevel);
    if (!png.ok()) {
        return png.error();
    }

    Json image = Json::object();
    image.set("format", Json::string("png"));
    image.set("encoding", Json::string("base64"));
    image.set("png", Json::string(base64Encode(png.value().data(), png.value().size())));
    image.set("w", Json::integer(static_cast<i64>(shown.width)));
    image.set("h", Json::integer(static_cast<i64>(shown.height)));
    // 축소했으면 원본 크기와 배율을 같이 준다 — AI 가 좌표를 되짚을 수 있어야 한다.
    image.set("x", Json::integer(static_cast<i64>(area.x)));
    image.set("y", Json::integer(static_cast<i64>(area.y)));
    image.set("srcW", Json::integer(static_cast<i64>(srcW)));
    image.set("srcH", Json::integer(static_cast<i64>(srcH)));
    image.set("scale", Json::number(scale));
    image.set("mode", Json::string(viewModeName(spec.mode)));
    image.set("bytes", Json::integer(static_cast<i64>(png.value().size())));
    out.image = std::move(image);
    return Ok(std::move(out));
}

} // namespace mari::agent
