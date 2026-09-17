// Mari Paint — 그리기 연산 (docs/05 2.3 — "스트로크로 그린다. 픽셀로 칠하지 않는다")
//
// 🔴 `stroke` 가 주 API 다. `drawLine` 같은 픽셀 연산을 주 API 로 만들지 않는다:
//    1. 결과가 **그린 것처럼** 보인다(브러시 프리셋이 그대로 적용된다)
//    2. 사람과 AI 가 **같은 파이프라인**을 쓴다 → 두 번째 구현이 없다
//    3. Sigan 이 AI 획도 똑같이 기록한다 — 출처를 달고서
//
// 🔴 출처: 이 파일 어디에도 origin 을 고르는 코드가 없다.
//    `session.strokeSource()` 가 게이트에서 나오고, 게이트는 Agent 밖에 못 만든다.
#include <mari/agent/session.hpp>

#include <mari/app/document.hpp>
#include <mari/core/undo.hpp>
#include <mari/ora/image.hpp>
#include <mari/stroke/native_engine.hpp>
#include <mari/stroke/pipeline.hpp>

#include <algorithm>
#include <cmath>

namespace mari::agent::ops {

// 🔴 `ops::stroke` 라는 이름이 있어서 `stroke::` 로는 네임스페이스를 못 가리킨다.
//    별칭을 둔다 — 이름이 겹친다는 이유로 파이프라인을 우회하지는 않는다.
namespace pipe_ns = ::mari::stroke;

namespace {

constexpr u64 kPointIntervalNs = 10'000'000ull; // 10ms — 사람 펜의 샘플 간격 수준

/// 그릴 수 있는 래스터 레이어를 고른다.
Result<LayerPtr> paintTarget(AgentSession& s, const Json& req, app::Document& doc) {
    const Result<LayerId> id = resolveLayer(doc.layers(), s.roles(), req["layer"]);
    if (!id.ok()) {
        return id.error();
    }
    const LayerPtr l = doc.layers().find(id.value());
    if (!l) {
        return Err("레이어가 사라졌다", ErrorCode::NotFound);
    }
    if (l->kind() != LayerKind::Raster || l->tiles() == nullptr) {
        return Err("그룹 레이어에는 그릴 수 없다", ErrorCode::InvalidArgument);
    }
    if (l->locked()) {
        return Err("잠긴 레이어다: \"" + l->name() + "\"", ErrorCode::InvalidArgument);
    }
    return Ok(l);
}

/// 그리기 대상 영역을 푼다(없으면 선택 영역, 그것도 없으면 캔버스 전체).
Result<Rect> paintRegion(AgentSession& s, const Json& req, app::Document& doc) {
    const Size sz = doc.canvasSize();
    const Rect canvas{0, 0, sz.width, sz.height};
    const Rect fallback = doc.selection().isEmpty() ? canvas : doc.selection();
    Result<Rect> r =
        resolveRegion(doc.layers(), s.roles(), req["region"], doc.selection(), fallback);
    if (!r.ok()) {
        return r;
    }
    const Rect clipped = r.value().intersected(canvas);
    if (clipped.isEmpty()) {
        return Err("그릴 영역이 캔버스 밖이거나 비었다", ErrorCode::InvalidArgument);
    }
    return Ok(clipped);
}

pipe_ns::SmoothingMode smoothingFromNumber(f64 v) {
    if (v < 0.05) {
        return pipe_ns::SmoothingMode::Off;
    }
    if (v < 0.35) {
        return pipe_ns::SmoothingMode::Light;
    }
    if (v < 0.7) {
        return pipe_ns::SmoothingMode::Medium;
    }
    return pipe_ns::SmoothingMode::Strong;
}

/// 스탬프가 점에서 벗어날 수 있는 최대 거리(px). 실행취소 범위를 잡는 데 쓴다.
/// **넉넉하게** 잡는다 — 모자라면 실행취소가 거짓말을 한다.
f32 strokeMargin(const brush::MariBrushPreset& p) {
    f32 sizeMul = 1.0f;
    f32 scatter = 0.0f;
    for (const brush::DynamicLink& l : p.dynamics) {
        f32 maxY = 1.0f;
        for (const brush::CurvePoint& c : l.curve.points) {
            maxY = std::max(maxY, std::abs(c.y));
        }
        if (l.output == brush::DynamicOutput::Size) {
            sizeMul = std::max(sizeMul, maxY * std::max(1.0f, std::abs(l.amount)));
        } else if (l.output == brush::DynamicOutput::Scatter) {
            scatter += maxY * std::abs(l.amount);
        }
    }
    const f32 d = p.tip.diameter * sizeMul;
    return d * (0.5f + scatter) + static_cast<f32>(kTileSize);
}

void tilesForRect(const Rect& r, DirtyTiles& out) {
    if (r.isEmpty()) {
        return;
    }
    const i32 tx0 = tileIndexFor(r.x);
    const i32 ty0 = tileIndexFor(r.y);
    const i32 tx1 = tileIndexFor(r.right() - 1);
    const i32 ty1 = tileIndexFor(r.bottom() - 1);
    for (i32 ty = ty0; ty <= ty1; ++ty) {
        for (i32 tx = tx0; tx <= tx1; ++tx) {
            out.push_back(TileCoord{tx, ty});
        }
    }
}

struct StrokePoint {
    f64 x = 0.0;
    f64 y = 0.0;
    f32 pressure = 1.0f;
    bool hasPressure = false;
};

Result<std::vector<StrokePoint>> parsePoints(const Json& v) {
    if (!v.isArray() || v.size() == 0) {
        return Err("points 는 비어 있지 않은 배열이어야 한다", ErrorCode::InvalidArgument);
    }
    const ApiLimits lim;
    if (static_cast<i64>(v.size()) > lim.maxStrokePoints) {
        return Err("points 가 너무 많다(최대 " + std::to_string(lim.maxStrokePoints) + ")",
                   ErrorCode::InvalidArgument);
    }
    std::vector<StrokePoint> out;
    out.reserve(v.size());
    for (usize i = 0; i < v.size(); ++i) {
        const Json& p = v.at(i);
        StrokePoint sp;
        if (p.isArray()) {
            if (p.size() != 2 && p.size() != 3) {
                return Err("points[" + std::to_string(i) + "] 는 [x,y] 또는 [x,y,p] 다",
                           ErrorCode::InvalidArgument);
            }
            sp.x = p.at(0).asNumber();
            sp.y = p.at(1).asNumber();
            if (p.size() == 3) {
                sp.pressure = static_cast<f32>(p.at(2).asNumber(1.0));
                sp.hasPressure = true;
            }
        } else if (p.isObject()) {
            static const char* kKnown[] = {"x", "y", "p", "pressure"};
            for (const std::string& k : p.keys()) {
                bool known = false;
                for (const char* kk : kKnown) {
                    known = known || k == kk;
                }
                if (!known) {
                    return Err("points[" + std::to_string(i) + "] 에 모르는 키가 있다: \"" + k +
                                   "\" (x|y|p)",
                               ErrorCode::InvalidArgument);
                }
            }
            if (!p["x"].isNumber() || !p["y"].isNumber()) {
                return Err("points[" + std::to_string(i) + "] 에 x, y 가 있어야 한다",
                           ErrorCode::InvalidArgument);
            }
            sp.x = p["x"].asNumber();
            sp.y = p["y"].asNumber();
            if (p.has("p") || p.has("pressure")) {
                sp.pressure = static_cast<f32>(p.has("p") ? p["p"].asNumber(1.0)
                                                          : p["pressure"].asNumber(1.0));
                sp.hasPressure = true;
            }
        } else {
            return Err("points[" + std::to_string(i) + "] 는 객체나 배열이어야 한다",
                       ErrorCode::InvalidArgument);
        }
        if (sp.hasPressure && (sp.pressure < 0.0f || sp.pressure > 1.0f)) {
            return Err("필압은 0..1 이다(points[" + std::to_string(i) + "])",
                       ErrorCode::InvalidArgument);
        }
        if (!std::isfinite(sp.x) || !std::isfinite(sp.y)) {
            return Err("좌표가 유한하지 않다(points[" + std::to_string(i) + "])",
                       ErrorCode::InvalidArgument);
        }
        out.push_back(sp);
    }
    return Ok(std::move(out));
}

/// 한 영역을 이미지로 덮어쓰고 **실행취소를 남긴다**(Document 의 경로를 그대로 쓴다).
Result<void> writeRegionWithUndo(app::Document& doc, LayerId id, const Rect& area,
                                 const ora::Image8& img, const char* undoText) {
    return doc.paintPixels(id, area, img.pixels.data(), img.pixels.size(), undoText);
}

} // namespace

// ── 🔴 stroke — 사람 펜과 같은 파이프라인 ────────────────────────────────

Result<Json> stroke(AgentSession& s, const Json& req) {
    const Result<app::Document*> d = s.requireDocument();
    if (!d.ok()) {
        return d.error();
    }
    app::Document* doc = d.value();
    const Result<LayerPtr> layer = paintTarget(s, req, *doc);
    if (!layer.ok()) {
        return layer.error();
    }

    const Result<const BrushEntry*> chosen = s.resolveBrush(req["brush"]);
    if (!chosen.ok()) {
        return chosen.error();
    }
    brush::MariBrushPreset preset = chosen.value()->preset;
    if (req.has("size")) {
        const f64 v = req["size"].asNumber(0.0);
        if (v <= 0.0 || v > 4096.0) {
            return Err("size 는 0 보다 크고 4096 이하여야 한다", ErrorCode::InvalidArgument);
        }
        preset.tip.diameter = static_cast<f32>(v);
    }
    if (req.has("spacing")) {
        const f64 v = req["spacing"].asNumber(0.0);
        if (v <= 0.0 || v > 10.0) {
            return Err("spacing 은 0 보다 크고 10 이하여야 한다", ErrorCode::InvalidArgument);
        }
        preset.spacing = static_cast<f32>(v);
    }
    if (req.has("opacity")) {
        const f64 v = req["opacity"].asNumber(-1.0);
        if (v < 0.0 || v > 1.0) {
            return Err("opacity 는 0..1 이다", ErrorCode::InvalidArgument);
        }
        preset.opacity = static_cast<f32>(v);
    }

    PressureProfile profile = PressureProfile::TaperInOut;
    if (req.has("pressureProfile")) {
        if (!req["pressureProfile"].isString()) {
            return Err("pressureProfile 은 문자열이어야 한다", ErrorCode::InvalidArgument);
        }
        const Result<PressureProfile> p = pressureProfileFromName(req["pressureProfile"].asString());
        if (!p.ok()) {
            return p.error();
        }
        profile = p.value();
    }

    const Result<std::vector<StrokePoint>> points = parsePoints(req["points"]);
    if (!points.ok()) {
        return points.error();
    }
    const Result<Color8> color = colorFromJson(req["color"], Color8::rgba(0, 0, 0, 255));
    if (!color.ok()) {
        return color.error();
    }

    Result<brush::BrushEnginePtr> engine = pipe_ns::makeNativeEngine();
    if (!engine.ok()) {
        return engine.error();
    }
    brush::ImportReport report;
    const Result<void> setp = engine.value()->setPreset(preset, &report);
    if (!setp.ok()) {
        return setp.error();
    }

    // 🔴 출처. **인자가 없다.** 이 세션이 만드는 획은 무조건 Agent 다(docs/05 3.1).
    brush::StrokeContext ctx(s.strokeSource());
    ctx.target = layer.value()->tiles();
    ctx.color = color.value();
    ctx.eraser = req["eraser"].asBool(false);
    ctx.alphaLocked = layer.value()->alphaLocked();
    ctx.layerId = layer.value()->id();
    ctx.seed = static_cast<u64>(req["seed"].asInt(0));

    pipe_ns::StrokePipeline pipe(engine.value().get());
    pipe_ns::StrokeConfig cfg;
    cfg.smoothing = smoothingFromNumber(req["smoothing"].asNumber(0.0));
    pipe.setConfig(cfg);

    // 실행취소는 칠하기 **전에** 담는다(core/undo.hpp 규약).
    Rect hull{};
    for (const StrokePoint& p : points.value()) {
        const auto x = static_cast<i32>(std::floor(p.x));
        const auto y = static_cast<i32>(std::floor(p.y));
        hull = hull.united(Rect{x, y, 1, 1});
    }
    const auto margin = static_cast<i32>(std::ceil(strokeMargin(preset)));
    const Rect undoArea{hull.x - margin, hull.y - margin, hull.width + 2 * margin,
                        hull.height + 2 * margin};
    DirtyTiles undoTiles;
    tilesForRect(undoArea, undoTiles);
    std::unique_ptr<TileSnapshotCommand> cmd =
        TileSnapshotCommand::begin(std::string("에이전트 획"), layer.value()->tiles());
    cmd->captureBefore(undoTiles);

    auto makeEvent = [&](const StrokePoint& p, usize i) {
        pipe_ns::RawInputEvent e{};
        e.x = p.x;
        e.y = p.y;
        const f32 t = points.value().size() <= 1
                          ? 0.5f
                          : static_cast<f32>(i) / static_cast<f32>(points.value().size() - 1);
        e.pressure = p.hasPressure ? p.pressure : pressureProfileAt(profile, t);
        e.pressureMax = 1.0f;
        e.hasPressure = true;
        e.hasTilt = false;
        e.timestampNs = static_cast<u64>(i) * kPointIntervalNs;
        return e;
    };

    const std::vector<StrokePoint>& pts = points.value();
    const Result<void> begun = pipe.begin(ctx, makeEvent(pts[0], 0));
    if (!begun.ok()) {
        return begun.error();
    }
    for (usize i = 1; i + 1 < pts.size(); ++i) {
        pipe.extend(makeEvent(pts[i], i));
    }
    if (pts.size() > 1) {
        pipe.end(makeEvent(pts.back(), pts.size() - 1));
    } else {
        pipe.end();
    }

    const DirtyTiles& dirty = pipe.dirtyTiles();
    const Rect dirtyRect = pipe.dirtyBounds();

    // 실행취소 범위가 실제로 칠해진 범위를 덮고 있는지 확인한다.
    // 못 덮었으면 숨기지 않고 응답에 적는다 — 조용히 거짓 실행취소를 남기지 않는다.
    bool undoComplete = true;
    for (const TileCoord& c : dirty) {
        const Rect tr = c.canvasRect();
        if (tr.intersected(undoArea) != tr) {
            undoComplete = false;
            break;
        }
    }
    cmd->captureAfter(undoTiles);
    const usize changed = cmd->changedTileCount();
    if (!cmd->empty()) {
        doc->undoStack().push(std::move(cmd));
    }

    doc->markDirty();
    s.noteDirty(dirtyRect);
    // 🔴 이 세션의 획을 센다. 출처 칸은 언제나 Agent 다.
    s.countStroke();

    Json out = Json::object();
    out.set("layer", Json::integer(layer.value()->id()));
    out.set("points", Json::integer(static_cast<i64>(pts.size())));
    out.set("stamps", Json::integer(static_cast<i64>(pipe.stampCount())));
    out.set("dirtyTiles", Json::integer(static_cast<i64>(dirty.size())));
    out.set("changedTiles", Json::integer(static_cast<i64>(changed)));
    out.set("brush", Json::string(chosen.value()->preset.name));
    out.set("smoothing", Json::string(pipe_ns::smoothingModeName(cfg.smoothing)));
    out.set("pressureProfile", Json::string(pressureProfileName(profile)));
    // 🔴 출처를 **보고한다.** 고르는 게 아니라 결과로 알려 주는 것이다.
    out.set("origin", Json::string(strokeOriginName(pipe.source().value().origin())));
    out.set("agentId", Json::string(std::string(s.agentId().view())));
    out.set("undoComplete", Json::boolean(undoComplete));
    if (!pipe.lastError().message.empty()) {
        // 핫 패스에서 삼킨 오류는 숨기지 않는다.
        out.set("engineNote", Json::string(pipe.lastError().message));
    }
    if (!report.notes.empty()) {
        Json notes = Json::array();
        for (const auto& n : report.notes) {
            notes.push(Json::string(n.message));
        }
        out.set("brushNotes", std::move(notes));
    }
    return Ok(std::move(out));
}

// ── 픽셀 연산 (보조 수단이다. 주 API 가 아니다) ─────────────────────────

Result<Json> fill(AgentSession& s, const Json& req) {
    const Result<app::Document*> d = s.requireDocument();
    if (!d.ok()) {
        return d.error();
    }
    app::Document* doc = d.value();
    const Result<LayerPtr> layer = paintTarget(s, req, *doc);
    if (!layer.ok()) {
        return layer.error();
    }
    const Result<Rect> area = paintRegion(s, req, *doc);
    if (!area.ok()) {
        return area.error();
    }
    const Result<Color8> color = colorFromJson(req["color"], Color8::rgba(0, 0, 0, 255));
    if (!color.ok()) {
        return color.error();
    }

    ora::Image8 img = ora::Image8::make(area.value().width, area.value().height);
    for (usize i = 0; i + 3 < img.pixels.size(); i += 4) {
        img.pixels[i] = color.value().r;
        img.pixels[i + 1] = color.value().g;
        img.pixels[i + 2] = color.value().b;
        img.pixels[i + 3] = color.value().a;
    }
    const Result<void> w =
        writeRegionWithUndo(*doc, layer.value()->id(), area.value(), img, "에이전트 채우기");
    if (!w.ok()) {
        return w.error();
    }
    s.noteDirty(area.value());
    s.countStroke();

    Json out = Json::object();
    out.set("layer", Json::integer(layer.value()->id()));
    out.set("area", jsonRect(area.value()));
    out.set("origin", Json::string(strokeOriginName(s.strokeSource().origin())));
    return Ok(std::move(out));
}

Result<Json> erase(AgentSession& s, const Json& req) {
    const Result<app::Document*> d = s.requireDocument();
    if (!d.ok()) {
        return d.error();
    }
    app::Document* doc = d.value();
    const Result<LayerPtr> layer = paintTarget(s, req, *doc);
    if (!layer.ok()) {
        return layer.error();
    }
    const Result<Rect> area = paintRegion(s, req, *doc);
    if (!area.ok()) {
        return area.error();
    }
    const ora::Image8 img = ora::Image8::make(area.value().width, area.value().height);
    const Result<void> w =
        writeRegionWithUndo(*doc, layer.value()->id(), area.value(), img, "에이전트 지우기");
    if (!w.ok()) {
        return w.error();
    }
    s.noteDirty(area.value());
    s.countStroke();

    Json out = Json::object();
    out.set("layer", Json::integer(layer.value()->id()));
    out.set("area", jsonRect(area.value()));
    return Ok(std::move(out));
}

Result<Json> gradient(AgentSession& s, const Json& req) {
    const Result<app::Document*> d = s.requireDocument();
    if (!d.ok()) {
        return d.error();
    }
    app::Document* doc = d.value();
    const Result<LayerPtr> layer = paintTarget(s, req, *doc);
    if (!layer.ok()) {
        return layer.error();
    }
    const Result<Rect> area = paintRegion(s, req, *doc);
    if (!area.ok()) {
        return area.error();
    }
    const Result<Color8> from = colorFromJson(req["from"], Color8::rgba(0, 0, 0, 255));
    if (!from.ok()) {
        return from.error();
    }
    const Result<Color8> to = colorFromJson(req["to"], Color8::rgba(255, 255, 255, 255));
    if (!to.ok()) {
        return to.error();
    }
    const f64 angleDeg = req["angle"].asNumber(0.0);
    const f64 rad = angleDeg * 3.14159265358979 / 180.0;
    const f64 ax = std::cos(rad);
    const f64 ay = std::sin(rad);

    const Rect r = area.value();
    // 영역을 지나는 축 방향 투영의 범위를 구해 0..1 로 정규화한다.
    f64 lo = 0.0;
    f64 hi = 0.0;
    bool first = true;
    for (int c = 0; c < 4; ++c) {
        const f64 px = (c & 1) ? static_cast<f64>(r.right() - 1) : static_cast<f64>(r.x);
        const f64 py = (c & 2) ? static_cast<f64>(r.bottom() - 1) : static_cast<f64>(r.y);
        const f64 t = px * ax + py * ay;
        lo = first ? t : std::min(lo, t);
        hi = first ? t : std::max(hi, t);
        first = false;
    }
    const f64 span = (hi - lo) > 1e-9 ? (hi - lo) : 1.0;

    ora::Image8 img = ora::Image8::make(r.width, r.height);
    for (i32 y = 0; y < r.height; ++y) {
        u8* row = img.pixels.data() + static_cast<usize>(y) * img.stride();
        for (i32 x = 0; x < r.width; ++x) {
            const f64 t =
                ((static_cast<f64>(r.x + x) * ax + static_cast<f64>(r.y + y) * ay) - lo) / span;
            const f64 u = std::clamp(t, 0.0, 1.0);
            auto mix = [u](u8 a, u8 b) {
                return static_cast<u8>(std::lround(static_cast<f64>(a) * (1.0 - u) +
                                                   static_cast<f64>(b) * u));
            };
            u8* px = row + static_cast<usize>(x) * 4u;
            px[0] = mix(from.value().r, to.value().r);
            px[1] = mix(from.value().g, to.value().g);
            px[2] = mix(from.value().b, to.value().b);
            px[3] = mix(from.value().a, to.value().a);
        }
    }
    const Result<void> w =
        writeRegionWithUndo(*doc, layer.value()->id(), r, img, "에이전트 그라데이션");
    if (!w.ok()) {
        return w.error();
    }
    s.noteDirty(r);
    s.countStroke();

    Json out = Json::object();
    out.set("layer", Json::integer(layer.value()->id()));
    out.set("area", jsonRect(r));
    out.set("angle", Json::number(angleDeg));
    return Ok(std::move(out));
}

Result<Json> transform(AgentSession& s, const Json& req) {
    if (req.has("scale") || req.has("rotate")) {
        // 🔴 리샘플링 경로가 없다. 있는 척하고 대충 하면 결과가 뭉개진다.
        return Err("지금은 정수 평행이동(dx, dy)만 된다. 확대·회전은 리샘플러가 들어와야 한다",
                   ErrorCode::Unsupported);
    }
    const Result<app::Document*> d = s.requireDocument();
    if (!d.ok()) {
        return d.error();
    }
    app::Document* doc = d.value();
    const Result<LayerPtr> layer = paintTarget(s, req, *doc);
    if (!layer.ok()) {
        return layer.error();
    }
    const i64 dx = req["dx"].asInt(0);
    const i64 dy = req["dy"].asInt(0);
    if (dx == 0 && dy == 0) {
        Json out = Json::object();
        out.set("layer", Json::integer(layer.value()->id()));
        out.set("moved", jsonRect(Rect{}));
        return Ok(std::move(out));
    }
    if (std::abs(dx) > 100000 || std::abs(dy) > 100000) {
        return Err("이동량이 너무 크다", ErrorCode::InvalidArgument);
    }

    const Rect src = layer.value()->tiles()->bounds();
    if (src.isEmpty()) {
        Json out = Json::object();
        out.set("layer", Json::integer(layer.value()->id()));
        out.set("moved", jsonRect(Rect{}));
        return Ok(std::move(out));
    }
    const Rect dst{src.x + static_cast<i32>(dx), src.y + static_cast<i32>(dy), src.width,
                   src.height};
    const Rect uni = src.united(dst);

    Result<ora::Image8> got = ora::readRegion(*layer.value()->tiles(), src);
    if (!got.ok()) {
        return got.error();
    }
    // 합집합 영역을 통째로 새로 쓴다 — 실행취소 한 칸으로 끝난다.
    ora::Image8 out = ora::Image8::make(uni.width, uni.height);
    for (i32 y = 0; y < src.height; ++y) {
        const u8* srcRow = got.value().pixels.data() + static_cast<usize>(y) * got.value().stride();
        const i32 ty = dst.y + y - uni.y;
        u8* dstRow = out.pixels.data() + static_cast<usize>(ty) * out.stride() +
                     static_cast<usize>(dst.x - uni.x) * 4u;
        std::copy(srcRow, srcRow + static_cast<usize>(src.width) * 4u, dstRow);
    }
    const Result<void> w =
        writeRegionWithUndo(*doc, layer.value()->id(), uni, out, "에이전트 이동");
    if (!w.ok()) {
        return w.error();
    }
    s.noteDirty(uni);

    Json res = Json::object();
    res.set("layer", Json::integer(layer.value()->id()));
    res.set("from", jsonRect(src));
    res.set("to", jsonRect(dst));
    res.set("moved", jsonRect(uni));
    return Ok(std::move(res));
}

} // namespace mari::agent::ops
