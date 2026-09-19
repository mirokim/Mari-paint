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
#include <mari/agent/address.hpp>
#include <mari/app/image_ops.hpp>
#include <mari/app/layer_commands.hpp>
#include <mari/app/live_stroke.hpp>
#include <mari/app/stroke_entry.hpp>
#include <mari/core/undo.hpp>
#include <mari/ora/image.hpp>
#include <mari/stroke/native_engine.hpp>
#include <mari/stroke/pipeline.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

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

// strokeMargin() · tilesForRect() 는 app/live_stroke.hpp 로 옮겼다 — 사람 펜 경로와 한 벌이다.
using app::strokeMargin;
using app::tilesForRect;

struct StrokePoint {
    f64 x = 0.0;
    f64 y = 0.0;
    f32 pressure = 1.0f;
    bool hasPressure = false;
    f32 tiltX = 0.0f; ///< -1..1 (사람 펜의 기울기 — 도 단위로 바꿔 파이프라인에 준다)
    f32 tiltY = 0.0f;
    bool hasTilt = false;
    f64 timeMs = -1.0; ///< 획 시작 기준 ms. 음수면 "없음"(균등 간격으로 채운다)
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
            static const char* kKnown[] = {"x", "y", "p", "pressure", "tx", "ty", "t"};
            for (const std::string& k : p.keys()) {
                bool known = false;
                for (const char* kk : kKnown) {
                    known = known || k == kk;
                }
                if (!known) {
                    return Err("points[" + std::to_string(i) + "] 에 모르는 키가 있다: \"" + k +
                                   "\" (x|y|p|tx|ty|t)",
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
            if (p.has("tx") || p.has("ty")) {
                sp.tiltX = static_cast<f32>(p["tx"].asNumber(0.0));
                sp.tiltY = static_cast<f32>(p["ty"].asNumber(0.0));
                if (sp.tiltX < -1.0f || sp.tiltX > 1.0f || sp.tiltY < -1.0f || sp.tiltY > 1.0f) {
                    return Err("기울기(tx, ty)는 -1..1 이다(points[" + std::to_string(i) + "])",
                               ErrorCode::InvalidArgument);
                }
                sp.hasTilt = true;
            }
            if (p.has("t")) {
                sp.timeMs = p["t"].asNumber(-1.0);
                if (!(sp.timeMs >= 0.0)) {
                    return Err("t(ms)는 0 이상이다(points[" + std::to_string(i) + "])", ErrorCode::InvalidArgument);
                }
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

/// 🔴 선택 마스크를 영역 이미지에 먹인다(fill·erase·gradient 가 쓴다).
///
/// 광역 연산도 **선택 밖을 건드리지 않는다.** 마스크가 중간값이면 원래 픽셀과 섞는다.
/// 섞기는 프리멀티플라이 공간에서 한다 — 스트레이트 알파로 채널별 보간하면
/// 반투명 가장자리가 검게 죽는다.
/// 전체 선택이면 아무 일도 하지 않는다(읽기조차 하지 않는다).
Result<void> maskRegionImage(app::Document& doc, const LayerPtr& layer, const Rect& area,
                             ora::Image8& img, bool eraser) {
    const SelectionMask& sel = doc.selectionMask();
    if (sel.isAll()) {
        return Ok();
    }
    Result<ora::Image8> had = ora::readRegion(*layer->tiles(), area);
    if (!had.ok()) {
        return had.error();
    }
    const ora::Image8& old = had.value();
    for (i32 y = 0; y < area.height; ++y) {
        u8* dst = img.pixels.data() + static_cast<usize>(y) * img.stride();
        const u8* src = old.pixels.data() + static_cast<usize>(y) * old.stride();
        for (i32 x = 0; x < area.width; ++x, dst += 4, src += 4) {
            const f32 m = static_cast<f32>(sel.valueAt(area.x + x, area.y + y)) * (1.0f / 255.0f);
            if (m >= 1.0f) {
                continue; // 완전히 선택된 자리 — 새 픽셀 그대로
            }
            if (m <= 0.0f) {
                std::copy(src, src + 4, dst); // 선택 밖 — 원래 픽셀을 되돌린다
                continue;
            }
            if (eraser) {
                // 지우개는 알파만 깎는다. 색을 0 쪽으로 끌면 가장자리가 검어진다.
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[2];
                dst[3] = static_cast<u8>(std::lround(static_cast<f32>(src[3]) * (1.0f - m)));
                continue;
            }
            const f32 oa = static_cast<f32>(src[3]) * (1.0f / 255.0f);
            const f32 na = static_cast<f32>(dst[3]) * (1.0f / 255.0f);
            const f32 outA = oa + (na - oa) * m;
            for (int c = 0; c < 3; ++c) {
                const f32 op = static_cast<f32>(src[c]) * oa;
                const f32 np = static_cast<f32>(dst[c]) * na;
                const f32 mixed = op + (np - op) * m;
                dst[c] = outA > 0.0f
                             ? static_cast<u8>(std::lround(std::clamp(mixed / outA, 0.0f, 255.0f)))
                             : src[c];
            }
            dst[3] = static_cast<u8>(std::lround(std::clamp(outA * 255.0f, 0.0f, 255.0f)));
        }
    }
    return Ok();
}

/// 한 영역을 이미지로 덮어쓰고 **실행취소를 남긴다**(Document 의 경로를 그대로 쓴다).
/// `changedTiles` 에 실제로 바뀐 타일 수가 담긴다(docs/06 결정 ② 축 C).
Result<void> writeRegionWithUndo(app::Document& doc, LayerId id, const Rect& area,
                                 const ora::Image8& img, const char* undoText,
                                 u32& changedTiles) {
    return doc.paintPixels(id, area, img.pixels.data(), img.pixels.size(), undoText,
                           &changedTiles);
}

/// 🔴 기록이 고장 났으면 **그리기 전에** 거절한다(docs/06 결정 ④).
///    기록 없이 바뀐 픽셀이 하나라도 생기면 그 .ora 의 prooflog 는 캔버스를 설명하지
///    못하고, 설명하지 못하는 인증서는 거짓이다(docs/06 6절 H1).
///    자동 복구는 하지 않는다 — 조용히 다시 그려지기 시작하면 그 사이가 구멍이 된다.
Result<void> requireRecording(const app::Document& doc) {
    if (doc.recordingBroken()) {
        return Err("기록이 고장 나 더 그릴 수 없다 — 기록 없이 그리는 모드는 없다"
                   "(docs/06 결정 ④)",
                   ErrorCode::IoError);
    }
    return Ok();
}

/// 영역 직접쓰기 하나를 **규약대로 마무리한다**(docs/06 결정 ① · ② · ④).
///
/// · 합성 프레임 쌍으로 기록하고(붓질 수에 섞이지 않는다)
/// · 저널이 고장 났으면 방금 쓴 픽셀을 undo 로 **롤백**하고 실패시킨다
/// · 파이프(싱크) 실패는 실패가 아니다 — 저널에 스풀되고 성공이다(docs/03 5.1 · 5.2)
Result<void> finishRegionOp(AgentSession& s, app::Document& doc, RegionOpKind kind,
                            const Rect& area, LayerId layerId, bool eraser, u32 changedTiles) {
    const Result<void> rec = app::recordRegionOp(doc, s.strokeSource(), kind, area, layerId,
                                                 eraser, changedTiles);
    if (!rec.ok()) {
        const Result<void> rolled = doc.undo();
        if (!rolled.ok()) {
            // 롤백까지 실패했다면 그 사실을 삼키지 않는다. 더 나쁜 상태를 숨기지 않는다.
            return Err(std::string(rec.message()) + " (게다가 롤백도 실패했다: " +
                           rolled.message() + ")",
                       ErrorCode::IoError);
        }
        return rec;
    }
    s.noteDirty(area);
    // 축 B·C. 붓질 수(축 A)는 **올리지 않는다** — 이건 붓질이 아니다(H3).
    s.countRegionOp();
    s.countChangedTiles(changedTiles);
    return Ok();
}

} // namespace

// ── 🔴 stroke — 사람 펜과 같은 파이프라인 ────────────────────────────────

Result<Json> stroke(AgentSession& s, const Json& req) {
    const Result<app::Document*> d = s.requireDocument();
    if (!d.ok()) {
        return d.error();
    }
    app::Document* doc = d.value();
    const Result<void> ok = requireRecording(*doc);
    if (!ok.ok()) {
        return ok.error();
    }
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
    if (req.has("background")) {
        const Result<Color8> bg = colorFromJson(req["background"], Color8::rgba(255, 255, 255, 255));
        if (!bg.ok()) {
            return bg.error();
        }
        ctx.background = bg.value();
    }
    ctx.eraser = req["eraser"].asBool(false);
    ctx.alphaLocked = layer.value()->alphaLocked();
    ctx.layerId = layer.value()->id();
    ctx.seed = static_cast<u64>(req["seed"].asInt(0));
    // 🔴 선택 밖에는 한 픽셀도 찍히지 않는다. 전체 선택이면 엔진이 이 포인터를 꺼 버리므로
    //    선택을 쓰지 않는 그림에서 늘어나는 비용은 0 이다(brush/engine.hpp 의 규약).
    ctx.selection = &doc->selectionMask();

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
        // 기울기는 사람 펜과 같은 단위(도)로 준다. -1..1 → ±60°.
        e.hasTilt = p.hasTilt;
        e.tiltXDeg = p.tiltX * 60.0f;
        e.tiltYDeg = p.tiltY * 60.0f;
        // 시간을 주면 그대로(속도 동적 반응이 사람처럼 반응한다), 없으면 균등 간격.
        e.timestampNs = p.timeMs >= 0.0 ? static_cast<u64>(p.timeMs * 1e6) : static_cast<u64>(i) * kPointIntervalNs;
        return e;
    };

    // 🔴 기록 입구. 사람 펜(WM_POINTER)이 붙어도 **같은 클래스**를 쓴다 —
    //    두 번째 발행 경로를 만들지 않는다(docs/06 결정 ③).
    //    여기서도 출처를 고르지 않는다: 파이프라인이 들고 있는 것을 그대로 넘긴다.
    app::StrokeEntry entry(*doc, ctx.source, ctx.layerId,
                           chosen.value()->id, ctx.eraser);
    // 프레임에 싣는 점은 **실제로 그린 점**이다. 입력 원본이 아니라 정규화·보정을 지난
    // 파이프라인의 샘플을 쓴다 — 기록과 픽셀이 어긋나면 그 기록은 캔버스를 설명하지 못한다.
    const auto sampleNow = [&]() {
        const pipe_ns::InputSample& in = pipe.lastSample();
        app::PenSample ps;
        ps.pos = in.pos;
        ps.pressure = in.pressure;
        ps.tiltX = in.tiltX;
        ps.tiltY = in.tiltY;
        ps.rotation = in.rotationDeg;
        ps.velocity = in.velocity;
        return ps;
    };

    const std::vector<StrokePoint>& pts = points.value();
    const Result<void> begun = pipe.begin(ctx, makeEvent(pts[0], 0));
    if (!begun.ok()) {
        return begun.error();
    }
    entry.down(sampleNow());
    for (usize i = 1; i + 1 < pts.size(); ++i) {
        pipe.extend(makeEvent(pts[i], i));
        entry.move(sampleNow());
    }
    if (pts.size() > 1) {
        pipe.end(makeEvent(pts.back(), pts.size() - 1));
    } else {
        pipe.end();
    }
    entry.up(sampleNow());

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

    // 획이 끝났다. 저널을 flush 하고 축 A·C 를 올린다(docs/06 결정 ②).
    entry.finish(static_cast<u32>(changed));
    if (entry.broken()) {
        // 🔴 픽셀은 바뀌었는데 정본에 흔적이 없다 = 인증서가 거짓이 된다.
        //    방금 그린 것을 되돌리고 실패시킨다(docs/06 결정 ④ · 6절 H1).
        const Result<void> rolled = doc->undo();
        if (!rolled.ok()) {
            return Err(std::string("저널에 기록하지 못했고 롤백도 실패했다: ") +
                           rolled.message(),
                       ErrorCode::IoError);
        }
        return Err("저널에 기록하지 못해 이 획을 되돌렸다 — 기록 없이 그리지 않는다"
                   "(docs/06 결정 ④)",
                   ErrorCode::IoError);
    }

    doc->markDirty();
    s.noteDirty(dirtyRect);
    // 🔴 이 세션의 붓질을 센다(축 A). 출처 칸은 언제나 Agent 다.
    s.countStroke();
    s.countChangedTiles(changed); // 축 C — 실제로 바뀐 타일 수

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
    // 🔴 기록되었는가를 **숨기지 않는다.** 레코더가 없으면 없다고 적는다 —
    //    "기록된 줄 알았는데 아니었다"가 가장 나쁜 결과다.
    out.set("recorded", Json::boolean(entry.recording()));
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

/// 페인트통 — 클릭한 점과 이어진 같은 색 영역을 전경색으로(GUI 의 G 도구와 같은 코어 경로).
Result<Json> bucket(AgentSession& s, const Json& req) {
    const Result<app::Document*> d = s.requireDocument();
    if (!d.ok()) {
        return d.error();
    }
    app::Document* doc = d.value();
    const Result<void> ok = requireRecording(*doc);
    if (!ok.ok()) {
        return ok.error();
    }
    const Result<LayerPtr> layer = paintTarget(s, req, *doc);
    if (!layer.ok()) {
        return layer.error();
    }
    const Json& at = req["at"];
    if (!at.isArray() || at.size() != 2) {
        return Err("at 은 [x, y] 다", ErrorCode::InvalidArgument);
    }
    const i32 sx = static_cast<i32>(std::floor(at.at(0).asNumber()));
    const i32 sy = static_cast<i32>(std::floor(at.at(1).asNumber()));
    const i64 tol = req["tolerance"].asInt(32);
    const i64 gap = req["gapClose"].asInt(0);
    if (tol < 0 || tol > 255 || gap < 0 || gap > 64) {
        return Err("tolerance 는 0..255, gapClose 는 0..64 다", ErrorCode::InvalidArgument);
    }
    const Result<Color8> color = colorFromJson(req["color"], Color8::rgba(0, 0, 0, 255));
    if (!color.ok()) {
        return color.error();
    }
    const bool eraser = req["eraser"].asBool(false);
    // 기준 레이어: 기본은 칠할 레이어. `sample` 로 다른 레이어(선화)를 볼 수 있다.
    const TileMap* sample = layer.value()->tiles();
    if (req.has("sample")) {
        const Result<LayerId> sid = resolveLayer(doc->layers(), s.roles(), req["sample"]);
        if (!sid.ok()) {
            return sid.error();
        }
        const LayerPtr sl = doc->layers().find(sid.value());
        if (!sl || sl->tiles() == nullptr) {
            return Err("sample 레이어가 래스터가 아니다", ErrorCode::InvalidArgument);
        }
        sample = sl->tiles();
    }
    Result<SelectionMask> region = SelectionMask::fromFlood(doc->canvasSize(), *sample, sx, sy,
                                                            static_cast<i32>(tol), static_cast<i32>(gap));
    if (!region.ok()) {
        return region.error();
    }
    // 🔴 출처는 세션 게이트가 준 것. fillWithMask 가 선택 마스크를 곱하고 저널에 남긴다.
    const Result<u32> r = app::fillWithMask(*doc, s.strokeSource(), layer.value()->id(), region.value(),
                                            color.value(), eraser);
    if (!r.ok()) {
        return r.error();
    }
    const Rect area = region.value().bounds();
    s.noteDirty(area);
    Json out = Json::object();
    out.set("layer", Json::integer(layer.value()->id()));
    out.set("area", jsonRect(area));
    out.set("changedPixels", Json::integer(static_cast<i64>(r.value())));
    return Ok(std::move(out));
}

Result<Json> fill(AgentSession& s, const Json& req) {
    const Result<app::Document*> d = s.requireDocument();
    if (!d.ok()) {
        return d.error();
    }
    app::Document* doc = d.value();
    const Result<void> ok = requireRecording(*doc);
    if (!ok.ok()) {
        return ok.error();
    }
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
    const Result<void> masked =
        maskRegionImage(*doc, layer.value(), area.value(), img, false);
    if (!masked.ok()) {
        return masked.error();
    }
    u32 changed = 0;
    const Result<void> w = writeRegionWithUndo(*doc, layer.value()->id(), area.value(), img,
                                               "에이전트 채우기", changed);
    if (!w.ok()) {
        return w.error();
    }
    // 🔴 붓질이 아니다. 합성 프레임 **쌍**으로 기록하고 축 B 에 센다(docs/06 결정 ①·②).
    //    "AI 획 1개"로 줄여 적으면 캔버스 전체를 칠하고도 0.05% 로 보인다.
    const Result<void> rec = finishRegionOp(s, *doc, RegionOpKind::Fill, area.value(),
                                            layer.value()->id(), false, changed);
    if (!rec.ok()) {
        return rec.error();
    }

    Json out = Json::object();
    out.set("layer", Json::integer(layer.value()->id()));
    out.set("area", jsonRect(area.value()));
    out.set("origin", Json::string(strokeOriginName(s.strokeSource().origin())));
    // 🔴 붓질이 아니라는 사실과 면적을 응답에도 싣는다. 세는 칸이 다르다(H3).
    out.set("regionOp", Json::string(regionOpKindName(RegionOpKind::Fill)));
    out.set("changedTiles", Json::integer(static_cast<i64>(changed)));
    out.set("recorded", Json::boolean(doc->recorder() != nullptr));
    return Ok(std::move(out));
}

Result<Json> erase(AgentSession& s, const Json& req) {
    const Result<app::Document*> d = s.requireDocument();
    if (!d.ok()) {
        return d.error();
    }
    app::Document* doc = d.value();
    const Result<void> ok = requireRecording(*doc);
    if (!ok.ok()) {
        return ok.error();
    }
    const Result<LayerPtr> layer = paintTarget(s, req, *doc);
    if (!layer.ok()) {
        return layer.error();
    }
    const Result<Rect> area = paintRegion(s, req, *doc);
    if (!area.ok()) {
        return area.error();
    }
    ora::Image8 img = ora::Image8::make(area.value().width, area.value().height);
    const Result<void> masked = maskRegionImage(*doc, layer.value(), area.value(), img, true);
    if (!masked.ok()) {
        return masked.error();
    }
    u32 changed = 0;
    const Result<void> w = writeRegionWithUndo(*doc, layer.value()->id(), area.value(), img,
                                               "에이전트 지우기", changed);
    if (!w.ok()) {
        return w.error();
    }
    const Result<void> rec = finishRegionOp(s, *doc, RegionOpKind::Erase, area.value(),
                                            layer.value()->id(), true, changed);
    if (!rec.ok()) {
        return rec.error();
    }

    Json out = Json::object();
    out.set("layer", Json::integer(layer.value()->id()));
    out.set("area", jsonRect(area.value()));
    out.set("regionOp", Json::string(regionOpKindName(RegionOpKind::Erase)));
    out.set("changedTiles", Json::integer(static_cast<i64>(changed)));
    out.set("recorded", Json::boolean(doc->recorder() != nullptr));
    return Ok(std::move(out));
}

Result<Json> gradient(AgentSession& s, const Json& req) {
    const Result<app::Document*> d = s.requireDocument();
    if (!d.ok()) {
        return d.error();
    }
    app::Document* doc = d.value();
    const Result<void> ok = requireRecording(*doc);
    if (!ok.ok()) {
        return ok.error();
    }
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
    const Result<void> masked = maskRegionImage(*doc, layer.value(), r, img, false);
    if (!masked.ok()) {
        return masked.error();
    }
    u32 changed = 0;
    const Result<void> w = writeRegionWithUndo(*doc, layer.value()->id(), r, img,
                                               "에이전트 그라데이션", changed);
    if (!w.ok()) {
        return w.error();
    }
    const Result<void> rec = finishRegionOp(s, *doc, RegionOpKind::Gradient, r,
                                            layer.value()->id(), false, changed);
    if (!rec.ok()) {
        return rec.error();
    }

    Json out = Json::object();
    out.set("layer", Json::integer(layer.value()->id()));
    out.set("area", jsonRect(r));
    out.set("angle", Json::number(angleDeg));
    out.set("regionOp", Json::string(regionOpKindName(RegionOpKind::Gradient)));
    out.set("changedTiles", Json::integer(static_cast<i64>(changed)));
    out.set("recorded", Json::boolean(doc->recorder() != nullptr));
    return Ok(std::move(out));
}

Result<Json> transform(AgentSession& s, const Json& req) {
    const Result<app::Document*> d = s.requireDocument();
    if (!d.ok()) {
        return d.error();
    }
    app::Document* doc = d.value();
    const Result<LayerPtr> layer = paintTarget(s, req, *doc);
    if (!layer.ok()) {
        return layer.error();
    }
    app::TransformParams p;
    p.dx = req["dx"].asNumber(0.0);
    p.dy = req["dy"].asNumber(0.0);
    if (req.has("scale")) {
        p.scaleX = p.scaleY = req["scale"].asNumber(1.0);
    }
    if (req.has("scaleX")) p.scaleX = req["scaleX"].asNumber(1.0);
    if (req.has("scaleY")) p.scaleY = req["scaleY"].asNumber(1.0);
    p.rotateDeg = req["rotate"].asNumber(0.0);
    p.flipH = req["flipH"].asBool(false);
    p.flipV = req["flipV"].asBool(false);
    p.bilinear = req["interpolation"].asString() != "nearest";
    if (req["pivot"].isArray() && req["pivot"].size() == 2) {
        p.usePivot = true;
        p.pivotX = req["pivot"].at(0).asNumber();
        p.pivotY = req["pivot"].at(1).asNumber();
    }
    if (std::abs(p.dx) > 100000 || std::abs(p.dy) > 100000 || std::fabs(p.scaleX) > 64.0 || std::fabs(p.scaleY) > 64.0) {
        return Err("변형량이 너무 크다", ErrorCode::InvalidArgument);
    }
    // 🔴 GUI 의 자유 변형과 같은 함수. 선택이 있으면 선택 안만 옮긴다.
    const Result<Rect> r = app::transformLayer(*doc, s.strokeSource(), layer.value()->id(), p);
    if (!r.ok()) {
        return r.error();
    }
    s.noteDirty(r.value());
    s.countRegionOp();
    Json out = Json::object();
    out.set("layer", Json::integer(layer.value()->id()));
    out.set("area", jsonRect(r.value()));
    out.set("regionOp", Json::string(regionOpKindName(RegionOpKind::Transform)));
    out.set("recorded", Json::boolean(doc->recorder() != nullptr));
    return Ok(std::move(out));
}

/// 색 보정 — GUI 의 이미지 › 보정 과 같은 함수.
Result<Json> adjust(AgentSession& s, const Json& req) {
    const Result<app::Document*> d = s.requireDocument();
    if (!d.ok()) {
        return d.error();
    }
    app::Document* doc = d.value();
    const Result<LayerPtr> layer = paintTarget(s, req, *doc);
    if (!layer.ok()) {
        return layer.error();
    }
    const std::string kind = req["kind"].isString() ? req["kind"].asString() : std::string();
    app::AdjustParams p;
    if (kind == "hsl" || kind == "hueSaturation") p.kind = app::AdjustKind::HueSaturation;
    else if (kind == "brightnessContrast") p.kind = app::AdjustKind::BrightnessContrast;
    else if (kind == "levels") p.kind = app::AdjustKind::Levels;
    else if (kind == "curves") p.kind = app::AdjustKind::Curves;
    else if (kind == "invert") p.kind = app::AdjustKind::Invert;
    else if (kind == "desaturate") p.kind = app::AdjustKind::Desaturate;
    else if (kind == "threshold") p.kind = app::AdjustKind::Threshold;
    else if (kind == "posterize") p.kind = app::AdjustKind::Posterize;
    else return Err("kind 는 hsl|brightnessContrast|levels|curves|invert|desaturate|threshold|posterize 중 하나다", ErrorCode::InvalidArgument);
    p.hue = static_cast<f32>(req["hue"].asNumber(0.0));
    p.saturation = static_cast<f32>(req["saturation"].asNumber(0.0));
    p.lightness = static_cast<f32>(req["lightness"].asNumber(0.0));
    p.brightness = static_cast<f32>(req["brightness"].asNumber(0.0));
    p.contrast = static_cast<f32>(req["contrast"].asNumber(0.0));
    p.inBlack = static_cast<i32>(req["inBlack"].asInt(0));
    p.inWhite = static_cast<i32>(req["inWhite"].asInt(255));
    p.outBlack = static_cast<i32>(req["outBlack"].asInt(0));
    p.outWhite = static_cast<i32>(req["outWhite"].asInt(255));
    p.gamma = static_cast<f32>(req["gamma"].asNumber(1.0));
    p.threshold = static_cast<i32>(req["threshold"].asInt(128));
    p.levels = static_cast<i32>(req["levels"].asInt(4));
    const auto curveOf = [](const Json& arr, std::vector<app::CurvePt>& out) {
        for (usize i = 0; i < arr.size(); ++i) {
            const Json& pt = arr.at(i);
            if (pt.isArray() && pt.size() == 2)
                out.push_back({static_cast<f32>(pt.at(0).asNumber()), static_cast<f32>(pt.at(1).asNumber())});
        }
    };
    curveOf(req["curve"], p.curve);
    curveOf(req["curveR"], p.curveR);
    curveOf(req["curveG"], p.curveG);
    curveOf(req["curveB"], p.curveB);
    const Result<u32> r = app::adjustLayer(*doc, s.strokeSource(), layer.value()->id(), p);
    if (!r.ok()) {
        return r.error();
    }
    const Size cs = doc->canvasSize();
    s.noteDirty(Rect{0, 0, cs.width, cs.height});
    s.countRegionOp();
    s.countChangedTiles(r.value());
    Json out = Json::object();
    out.set("layer", Json::integer(layer.value()->id()));
    out.set("kind", Json::string(kind));
    out.set("changedTiles", Json::integer(static_cast<i64>(r.value())));
    out.set("regionOp", Json::string(regionOpKindName(RegionOpKind::Adjust)));
    return Ok(std::move(out));
}

/// 캔버스 연산: flip | rotate | resize | crop | scale — 모든 레이어, 실행취소 하나.
Result<Json> canvasOp(AgentSession& s, const Json& req) {
    const Result<app::Document*> d = s.requireDocument();
    if (!d.ok()) {
        return d.error();
    }
    app::Document* doc = d.value();
    const std::string action = req["action"].isString() ? req["action"].asString() : std::string();
    Result<void> r = Ok();
    if (action == "flip") {
        r = app::flipCanvas(*doc, s.strokeSource(), req["axis"].asString() != "vertical");
    } else if (action == "rotate") {
        const i64 deg = req["degrees"].asInt(90);
        if (deg % 90 != 0) return Err("degrees 는 90 의 배수다", ErrorCode::InvalidArgument);
        r = app::rotateCanvas(*doc, s.strokeSource(), static_cast<int>(deg / 90));
    } else if (action == "resize" || action == "scale") {
        const i64 w = req["width"].asInt(0), h = req["height"].asInt(0);
        if (w <= 0 || h <= 0 || w > 32768 || h > 32768) return Err("width/height 가 범위 밖이다", ErrorCode::InvalidArgument);
        if (action == "resize")
            r = app::resizeCanvas(*doc, s.strokeSource(), Size{static_cast<i32>(w), static_cast<i32>(h)},
                                  static_cast<int>(req["anchorX"].asInt(1)), static_cast<int>(req["anchorY"].asInt(1)));
        else
            r = app::scaleImage(*doc, s.strokeSource(), Size{static_cast<i32>(w), static_cast<i32>(h)});
    } else if (action == "crop") {
        Rect area;
        if (req.has("region")) {
            const Result<Rect> reg = paintRegion(s, req, *doc);
            if (!reg.ok()) return reg.error();
            area = reg.value();
        } else {
            area = doc->selectionMask().isAll() ? Rect{} : doc->selectionMask().bounds();
            if (area.isEmpty()) return Err("crop 은 region 이나 선택이 필요하다", ErrorCode::InvalidArgument);
        }
        r = app::cropCanvas(*doc, s.strokeSource(), area);
    } else {
        return Err("action 은 flip|rotate|resize|scale|crop 중 하나다", ErrorCode::InvalidArgument);
    }
    if (!r.ok()) {
        return r.error();
    }
    s.roles();
    const Size cs = doc->canvasSize();
    s.noteDirty(Rect{0, 0, cs.width, cs.height});
    s.countRegionOp();
    Json out = Json::object();
    out.set("action", Json::string(action));
    out.set("canvas", jsonRect(Rect{0, 0, cs.width, cs.height}));
    return Ok(std::move(out));
}

} // namespace mari::agent::ops
