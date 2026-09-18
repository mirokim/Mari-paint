// Mari Paint — 레이어·선택·브러시 연산. 표는 capabilities.cpp 에 있다.
#include <mari/agent/session.hpp>

#include <mari/app/document.hpp>
#include <mari/core/blend.hpp>
#include <mari/core/layer_ops.hpp>
#include <mari/io/brush/importer.hpp>
#include <mari/ora/image.hpp>

namespace mari::agent::ops {
namespace {

/// 대상 레이어를 푼다(없으면 활성 레이어).
Result<LayerPtr> targetLayer(AgentSession& s, const Json& req, app::Document& doc) {
    const Result<LayerId> id = resolveLayer(doc.layers(), s.roles(), req["layer"]);
    if (!id.ok()) {
        return id.error();
    }
    LayerPtr l = doc.layers().find(id.value());
    if (!l) {
        return Err("레이어가 사라졌다", ErrorCode::NotFound);
    }
    return Ok(std::move(l));
}

/// 같은 부모 안에서 대상 바로 아래에 있는 형제를 찾는다.
LayerPtr siblingBelow(const LayerTree& tree, LayerId id) {
    struct Finder {
        static LayerPtr scan(const std::vector<LayerPtr>& list, LayerId want) {
            for (usize i = 0; i < list.size(); ++i) {
                if (list[i]->id() == want) {
                    return i == 0 ? nullptr : list[i - 1];
                }
                LayerPtr deeper = scan(list[i]->children(), want);
                if (deeper) {
                    return deeper;
                }
            }
            return nullptr;
        }
    };
    return Finder::scan(tree.roots(), id);
}

Json layerNamesOf(const LayerTree& tree, const RoleTags& roles, const Size& canvas) {
    struct W {
        static void walk(const std::vector<LayerPtr>& list, const RoleTags& r, const Size& c,
                         Json& out) {
            for (const LayerPtr& l : list) {
                out.push(layerToJson(*l, r, c));
                walk(l->children(), r, c, out);
            }
        }
    };
    Json arr = Json::array();
    W::walk(tree.roots(), roles, canvas, arr);
    return arr;
}

} // namespace

// ── 레이어 ───────────────────────────────────────────────────────────────

Result<Json> layerList(AgentSession& s, const Json&) {
    const Result<app::Document*> d = s.requireDocument();
    if (!d.ok()) {
        return d.error();
    }
    app::Document* doc = d.value();
    Json out = Json::object();
    out.set("layers", layerNamesOf(doc->layers(), s.roles(), doc->canvasSize()));
    out.set("activeLayer", Json::integer(doc->layers().activeLayer()));
    return Ok(std::move(out));
}

Result<Json> layerAdd(AgentSession& s, const Json& req) {
    const Result<app::Document*> d = s.requireDocument();
    if (!d.ok()) {
        return d.error();
    }
    LayerTree& tree = d.value()->layers();

    const std::string kind = req["kind"].isString() ? req["kind"].asString() : std::string("raster");
    if (kind != "raster" && kind != "group") {
        return Err("layer.kind 는 raster | group 이다", ErrorCode::InvalidArgument);
    }
    LayerId parent = kInvalidLayerId;
    if (req.has("parent")) {
        const Result<LayerId> p = resolveLayer(tree, s.roles(), req["parent"]);
        if (!p.ok()) {
            return p.error();
        }
        parent = p.value();
    }
    const int index = req.has("index") ? static_cast<int>(req["index"].asInt(-1)) : -1;
    std::string name = req["name"].isString() ? req["name"].asString() : std::string{};
    if (name.empty()) {
        name = kind == "group" ? "그룹" : "레이어";
    }

    Result<LayerPtr> added =
        kind == "group" ? tree.addGroup(name, parent, index) : tree.addRaster(name, parent, index);
    if (!added.ok()) {
        return added.error();
    }
    const LayerId id = added.value()->id();
    if (req["role"].isString()) {
        const LayerRole role = layerRoleFromName(req["role"].asString());
        if (role == LayerRole::None) {
            (void)tree.remove(id); // 반쯤 만들어진 상태를 남기지 않는다
            return Err("모르는 역할이다: \"" + req["role"].asString() + "\"",
                       ErrorCode::InvalidArgument);
        }
        s.roles().set(id, role);
    }
    d.value()->markDirty();

    Json out = Json::object();
    out.set("layer", layerToJson(*added.value(), s.roles(), d.value()->canvasSize()));
    return Ok(std::move(out));
}

Result<Json> layerRemove(AgentSession& s, const Json& req) {
    const Result<app::Document*> d = s.requireDocument();
    if (!d.ok()) {
        return d.error();
    }
    const Result<LayerPtr> l = targetLayer(s, req, *d.value());
    if (!l.ok()) {
        return l.error();
    }
    const LayerId id = l.value()->id();
    const Rect bounds = l.value()->bounds();
    const Result<void> r = d.value()->layers().remove(id);
    if (!r.ok()) {
        return r.error();
    }
    s.roles().erase(id);
    d.value()->markDirty();
    s.noteDirty(bounds);
    Json out = Json::object();
    out.set("removed", Json::integer(id));
    return Ok(std::move(out));
}

Result<Json> layerMove(AgentSession& s, const Json& req) {
    const Result<app::Document*> d = s.requireDocument();
    if (!d.ok()) {
        return d.error();
    }
    LayerTree& tree = d.value()->layers();
    const Result<LayerPtr> l = targetLayer(s, req, *d.value());
    if (!l.ok()) {
        return l.error();
    }
    LayerId parent = kInvalidLayerId;
    if (req.has("parent")) {
        const Result<LayerId> p = resolveLayer(tree, s.roles(), req["parent"]);
        if (!p.ok()) {
            return p.error();
        }
        parent = p.value();
    }
    const int index = req.has("index") ? static_cast<int>(req["index"].asInt(-1)) : -1;
    const Rect before = l.value()->bounds();
    const Result<void> r = tree.move(l.value()->id(), parent, index);
    if (!r.ok()) {
        return r.error();
    }
    d.value()->markDirty();
    s.noteDirty(before.united(l.value()->bounds()));
    Json out = Json::object();
    out.set("layer", layerToJson(*l.value(), s.roles(), d.value()->canvasSize()));
    return Ok(std::move(out));
}

Result<Json> layerDuplicate(AgentSession& s, const Json& req) {
    const Result<app::Document*> d = s.requireDocument();
    if (!d.ok()) {
        return d.error();
    }
    const Result<LayerPtr> l = targetLayer(s, req, *d.value());
    if (!l.ok()) {
        return l.error();
    }
    std::string name = req["name"].isString() ? req["name"].asString() : std::string{};
    // 🔴 O(1). 픽셀을 복사하지 않는다(TileMap 의 COW).
    Result<LayerPtr> copy = duplicateLayer(d.value()->layers(), l.value()->id(), std::move(name));
    if (!copy.ok()) {
        return copy.error();
    }
    d.value()->markDirty();
    s.noteDirty(copy.value()->bounds());
    Json out = Json::object();
    out.set("layer", layerToJson(*copy.value(), s.roles(), d.value()->canvasSize()));
    out.set("copiedPixels", Json::boolean(false));
    return Ok(std::move(out));
}

Result<Json> layerMerge(AgentSession& s, const Json& req) {
    const Result<app::Document*> d = s.requireDocument();
    if (!d.ok()) {
        return d.error();
    }
    app::Document* doc = d.value();
    const Result<LayerPtr> upper = targetLayer(s, req, *doc);
    if (!upper.ok()) {
        return upper.error();
    }
    if (upper.value()->kind() != LayerKind::Raster) {
        return Err("그룹 레이어 합치기는 아직 지원하지 않는다", ErrorCode::Unsupported);
    }
    const LayerPtr lower = siblingBelow(doc->layers(), upper.value()->id());
    if (!lower) {
        return Err("아래에 합칠 레이어가 없다", ErrorCode::NotFound);
    }
    if (lower->kind() != LayerKind::Raster) {
        return Err("아래가 그룹이라 합칠 수 없다", ErrorCode::Unsupported);
    }
    if (lower->locked()) {
        return Err("아래 레이어가 잠겨 있다", ErrorCode::InvalidArgument);
    }

    const Rect area = upper.value()->bounds().united(lower->bounds());
    if (area.isEmpty()) {
        const Result<void> rm = doc->layers().remove(upper.value()->id());
        if (!rm.ok()) {
            return rm.error();
        }
        Json out = Json::object();
        out.set("merged", Json::integer(lower->id()));
        out.set("area", jsonRect(area));
        return Ok(std::move(out));
    }

    Result<ora::Image8> top = ora::readRegion(*upper.value()->tiles(), area);
    if (!top.ok()) {
        return top.error();
    }
    Result<ora::Image8> bottom = ora::readRegion(*lower->tiles(), area);
    if (!bottom.ok()) {
        return bottom.error();
    }
    const f32 alpha = upper.value()->visible() ? upper.value()->opacity() : 0.0f;
    for (i32 y = 0; y < area.height; ++y) {
        u8* dst = bottom.value().pixels.data() + static_cast<usize>(y) * bottom.value().stride();
        const u8* src = top.value().pixels.data() + static_cast<usize>(y) * top.value().stride();
        blendRowRgba8(upper.value()->blendMode(), dst, src, area.width, alpha, nullptr);
    }
    const Result<void> w = ora::writeRegion(*lower->tiles(), area, bottom.value());
    if (!w.ok()) {
        return w.error();
    }
    const LayerId gone = upper.value()->id();
    const Result<void> rm = doc->layers().remove(gone);
    if (!rm.ok()) {
        return rm.error();
    }
    s.roles().erase(gone);
    doc->markDirty();
    s.noteDirty(area);

    Json out = Json::object();
    out.set("merged", Json::integer(lower->id()));
    out.set("removed", Json::integer(gone));
    out.set("area", jsonRect(area));
    return Ok(std::move(out));
}

Result<Json> layerSetProps(AgentSession& s, const Json& req) {
    const Result<app::Document*> d = s.requireDocument();
    if (!d.ok()) {
        return d.error();
    }
    app::Document* doc = d.value();
    const Result<LayerPtr> l = targetLayer(s, req, *doc);
    if (!l.ok()) {
        return l.error();
    }
    Layer& layer = *l.value();

    if (req["name"].isString()) {
        layer.setName(req["name"].asString());
    }
    if (req.has("opacity")) {
        const f64 v = req["opacity"].asNumber(-1.0);
        if (v < 0.0 || v > 1.0) {
            return Err("opacity 는 0..1 이다", ErrorCode::InvalidArgument);
        }
        layer.setOpacity(static_cast<f32>(v));
    }
    if (req.has("blendMode")) {
        if (!req["blendMode"].isString()) {
            return Err("blendMode 는 문자열이어야 한다", ErrorCode::InvalidArgument);
        }
        const Result<BlendMode> m = blendModeFromName(req["blendMode"].asString());
        if (!m.ok()) {
            return m.error();
        }
        layer.setBlendMode(m.value());
    }
    if (req.has("visible")) {
        layer.setVisible(req["visible"].asBool(true));
    }
    if (req.has("locked")) {
        layer.setLocked(req["locked"].asBool(false));
    }
    if (req.has("alphaLocked")) {
        layer.setAlphaLocked(req["alphaLocked"].asBool(false));
    }
    if (req.has("clipToBelow")) {
        layer.setClipToBelow(req["clipToBelow"].asBool(false));
    }
    if (req.has("role")) {
        if (!req["role"].isString()) {
            return Err("role 은 문자열이어야 한다", ErrorCode::InvalidArgument);
        }
        const std::string& r = req["role"].asString();
        if (r.empty()) {
            s.roles().erase(layer.id());
        } else {
            const LayerRole role = layerRoleFromName(r);
            if (role == LayerRole::None) {
                return Err("모르는 역할이다: \"" + r + "\"", ErrorCode::InvalidArgument);
            }
            s.roles().set(layer.id(), role);
        }
    }
    if (req["active"].asBool(false)) {
        const Result<void> a = doc->layers().setActiveLayer(layer.id());
        if (!a.ok()) {
            return a.error();
        }
    }
    doc->markDirty();
    s.noteDirty(layer.bounds());

    Json out = Json::object();
    out.set("layer", layerToJson(layer, s.roles(), doc->canvasSize()));
    return Ok(std::move(out));
}

// ── 선택 (docs/05 8.3 이 적어 둔 한계를 여기서 메운다) ───────────────────
//
// 🔴 선택 정본은 **마스크**다(core/selection.hpp). 사각형은 그 경계 상자일 뿐이고,
//    `select` 응답은 언제나 마스크의 실제 상태(kind·tiles·pixels)를 같이 적는다 —
//    사각형만 돌려주면 "사각형이 아닌 선택을 사각형으로 아는" AI 가 생긴다.

namespace {

/// 선택 상태를 응답 본문으로. **경계 상자만 주지 않는다.**
Json selectionToJson(const app::Document& doc) {
    const SelectionMask& m = doc.selectionMask();
    Json out = Json::object();
    // 사각형 칸은 예전 그대로 — 전체 선택(= 제한 없음)은 빈 Rect 다.
    out.set("selection", jsonRect(doc.selection()));
    out.set("bounds", jsonRect(m.bounds()));
    out.set("kind", Json::string(m.isAll() ? "all" : (m.isEmpty() ? "empty" : "mask")));
    // 🔴 메모리를 숨기지 않는다. 전체 선택·빈 선택은 타일 0개여야 한다.
    out.set("tiles", Json::integer(static_cast<i64>(m.tileCount())));
    out.set("selectedPixels", Json::integer(static_cast<i64>(m.selectedPixels())));
    const i64 area = static_cast<i64>(doc.canvasSize().width) * doc.canvasSize().height;
    out.set("coverage", Json::number(area > 0 ? static_cast<f64>(m.selectedPixels()) /
                                                    static_cast<f64>(area)
                                              : 0.0));
    return out;
}

/// 선택을 뽑아 올 원본 레이어의 타일맵.
Result<const TileMap*> sourceTiles(AgentSession& s, const Json& req, app::Document& doc) {
    const Result<LayerId> id = resolveLayer(doc.layers(), s.roles(), req["layer"]);
    if (!id.ok()) {
        return id.error();
    }
    const LayerPtr l = doc.layers().find(id.value());
    if (!l || l->tiles() == nullptr) {
        return Err("그룹 레이어에서는 선택을 뽑을 수 없다", ErrorCode::InvalidArgument);
    }
    return Ok(static_cast<const TileMap*>(l->tiles()));
}

/// `points` 를 올가미 폴리곤으로 읽는다. `[[x,y],...]` 와 `[{x,y},...]` 둘 다 받는다.
Result<std::vector<PointF>> lassoPoints(const Json& v) {
    if (!v.isArray() || v.size() < 3) {
        return Err("올가미는 points 에 점이 3개 이상 있어야 한다", ErrorCode::InvalidArgument);
    }
    std::vector<PointF> out;
    out.reserve(v.size());
    for (usize i = 0; i < v.size(); ++i) {
        const Json& p = v.at(i);
        if (p.isArray() && p.size() >= 2) {
            out.push_back(PointF{static_cast<f32>(p.at(0).asNumber()),
                                 static_cast<f32>(p.at(1).asNumber())});
        } else if (p.isObject() && p["x"].isNumber() && p["y"].isNumber()) {
            out.push_back(
                PointF{static_cast<f32>(p["x"].asNumber()), static_cast<f32>(p["y"].asNumber())});
        } else {
            return Err("points[" + std::to_string(i) + "] 는 [x,y] 또는 {x,y} 다",
                       ErrorCode::InvalidArgument);
        }
    }
    return Ok(std::move(out));
}

/// 요청이 말하는 **새 마스크 한 장**을 만든다. 결합·페더는 부르는 쪽이 한다.
Result<SelectionMask> buildMask(AgentSession& s, const Json& req, app::Document& doc) {
    const Size sz = doc.canvasSize();
    const Rect canvas{0, 0, sz.width, sz.height};
    const std::string mode = req.has("mode") ? req["mode"].asString() : std::string("rect");
    const bool aa = req["antialias"].asBool(true);

    if (mode == "rect" || mode == "ellipse") {
        Result<Rect> r = resolveRegion(doc.layers(), s.roles(), req["region"],
                                       doc.selection(), canvas);
        if (!r.ok()) {
            return r.error();
        }
        return mode == "rect" ? SelectionMask::fromRect(sz, r.value())
                              : SelectionMask::fromEllipse(sz, r.value(), aa);
    }
    if (mode == "lasso") {
        Result<std::vector<PointF>> pts = lassoPoints(req["points"]);
        if (!pts.ok()) {
            return pts.error();
        }
        return SelectionMask::fromPolygon(sz, pts.value(), aa);
    }
    if (mode == "color") {
        const Result<const TileMap*> src = sourceTiles(s, req, doc);
        if (!src.ok()) {
            return src.error();
        }
        const Result<Color8> ref = colorFromJson(req["color"], Color8::rgba(0, 0, 0, 255));
        if (!ref.ok()) {
            return ref.error();
        }
        const i64 tol = req["tolerance"].asInt(0);
        if (tol < 0 || tol > 255) {
            return Err("tolerance 는 0..255 다", ErrorCode::InvalidArgument);
        }
        return SelectionMask::fromColorRange(sz, *src.value(), ref.value(),
                                             static_cast<i32>(tol));
    }
    if (mode == "content") {
        const Result<const TileMap*> src = sourceTiles(s, req, doc);
        if (!src.ok()) {
            return src.error();
        }
        const i64 th = req["threshold"].asInt(1);
        if (th < 0 || th > 255) {
            return Err("threshold 는 0..255 다", ErrorCode::InvalidArgument);
        }
        return SelectionMask::fromContent(sz, *src.value(), static_cast<u8>(th));
    }
    if (mode == "alpha") {
        const Result<const TileMap*> src = sourceTiles(s, req, doc);
        if (!src.ok()) {
            return src.error();
        }
        return SelectionMask::fromLayerAlpha(sz, *src.value());
    }
    return Err("모르는 선택 방식이다: \"" + mode +
                   "\" (rect|ellipse|lasso|color|content|alpha)",
               ErrorCode::InvalidArgument);
}

} // namespace

Result<Json> select(AgentSession& s, const Json& req) {
    const Result<app::Document*> d = s.requireDocument();
    if (!d.ok()) {
        return d.error();
    }
    app::Document* doc = d.value();

    // region 도 mode 도 없으면 선택 해제 = **제한 없음**(전체 선택). 타일 0개로 돌아간다.
    if (!req.has("region") && !req.has("mode") && !req.has("points")) {
        doc->setSelectionMask(SelectionMask::all(doc->canvasSize()));
        return Ok(selectionToJson(*doc));
    }

    Result<SelectionMask> made = buildMask(s, req, *doc);
    if (!made.ok()) {
        return made.error();
    }
    SelectionMask mask = std::move(made).value();

    // 만든 직후에 페더를 먹인다 — 결합하기 전이라야 각 조각의 경계가 부드러워진다.
    if (req.has("feather")) {
        const f64 rad = req["feather"].asNumber(0.0);
        if (rad < 0.0 || rad > 1024.0) {
            return Err("feather 는 0..1024 다", ErrorCode::InvalidArgument);
        }
        const Result<void> f = mask.feather(static_cast<f32>(rad));
        if (!f.ok()) {
            return f.error();
        }
    }

    // 🔴 파라미터 이름이 `op` 가 아니라 `combine` 인 이유: 요청 객체의 `op` 는 **연산 이름**
    //    자리다(capabilities.hpp kOpKey). 겹치면 결합 방식이 연산 이름을 먹는다.
    SelectionOp op = SelectionOp::Replace;
    if (req.has("combine")) {
        const Result<SelectionOp> parsed = selectionOpFromName(req["combine"].asString());
        if (!parsed.ok()) {
            return parsed.error();
        }
        op = parsed.value();
    }
    if (op == SelectionOp::Replace) {
        doc->setSelectionMask(std::move(mask));
    } else {
        SelectionMask cur = doc->selectionMask();
        const Result<void> c = cur.combine(mask, op);
        if (!c.ok()) {
            return c.error();
        }
        doc->setSelectionMask(std::move(cur));
    }
    Json out = selectionToJson(*doc);
    out.set("mode", Json::string(req.has("mode") ? req["mode"].asString() : std::string("rect")));
    out.set("combine", Json::string(selectionOpName(op)));
    return Ok(std::move(out));
}

Result<Json> selectInvert(AgentSession& s, const Json&) {
    const Result<app::Document*> d = s.requireDocument();
    if (!d.ok()) {
        return d.error();
    }
    app::Document* doc = d.value();
    SelectionMask m = doc->selectionMask();
    m.invert(); // O(타일 수). 캔버스를 할당하지 않는다.
    doc->setSelectionMask(std::move(m));
    return Ok(selectionToJson(*doc));
}

Result<Json> selectExpand(AgentSession& s, const Json& req) {
    const Result<app::Document*> d = s.requireDocument();
    if (!d.ok()) {
        return d.error();
    }
    app::Document* doc = d.value();
    const i64 by = req["by"].asInt(0);
    if (by < -16384 || by > 16384) {
        return Err("by 가 너무 크다(±16384)", ErrorCode::InvalidArgument);
    }
    SelectionMask m = doc->selectionMask();
    const Result<void> e = m.expand(static_cast<i32>(by));
    if (!e.ok()) {
        return e.error();
    }
    doc->setSelectionMask(std::move(m));
    Json out = selectionToJson(*doc);
    out.set("by", Json::integer(by));
    return Ok(std::move(out));
}

Result<Json> selectFeather(AgentSession& s, const Json& req) {
    const Result<app::Document*> d = s.requireDocument();
    if (!d.ok()) {
        return d.error();
    }
    app::Document* doc = d.value();
    const f64 rad = req["radius"].asNumber(0.0);
    if (rad < 0.0 || rad > 1024.0) {
        return Err("radius 는 0..1024 다", ErrorCode::InvalidArgument);
    }
    SelectionMask m = doc->selectionMask();
    const Result<void> f = m.feather(static_cast<f32>(rad));
    if (!f.ok()) {
        return f.error();
    }
    doc->setSelectionMask(std::move(m));
    Json out = selectionToJson(*doc);
    out.set("radius", Json::number(rad));
    return Ok(std::move(out));
}

// ── 브러시 ───────────────────────────────────────────────────────────────

namespace {

Json brushToJson(const BrushEntry& b, bool current) {
    Json j = Json::object();
    j.set("id", Json::integer(b.id));
    j.set("name", Json::string(b.preset.name));
    j.set("engine", Json::string(b.preset.engine.empty() ? "native" : b.preset.engine));
    j.set("source", Json::string(b.source));
    j.set("diameter", Json::number(static_cast<f64>(b.preset.tip.diameter)));
    j.set("spacing", Json::number(static_cast<f64>(b.preset.spacing)));
    j.set("hardness", Json::number(static_cast<f64>(b.preset.tip.hardness)));
    j.set("opacity", Json::number(static_cast<f64>(b.preset.opacity)));
    j.set("blendMode", Json::string(blendModeName(b.preset.blendMode)));
    j.set("dynamics", Json::integer(static_cast<i64>(b.preset.dynamics.size())));
    j.set("current", Json::boolean(current));
    return j;
}

} // namespace

Result<Json> brushList(AgentSession& s, const Json&) {
    Json arr = Json::array();
    for (const BrushEntry& b : s.brushes()) {
        arr.push(brushToJson(b, b.id == s.currentBrush()));
    }
    Json out = Json::object();
    out.set("brushes", std::move(arr));
    out.set("currentBrush", Json::integer(s.currentBrush()));
    return Ok(std::move(out));
}

Result<Json> brushImport(AgentSession& s, const Json& req) {
    if (!req["path"].isString()) {
        return Err("path 는 문자열이어야 한다", ErrorCode::InvalidArgument);
    }
    const std::string& path = req["path"].asString();
    const io::brush::BrushFormat fmt = io::brush::detectFormat(path);
    Result<brush::ImportResult> imported = io::brush::importBrushFile(path);
    if (!imported.ok()) {
        return imported.error();
    }
    const char* src = fmt == io::brush::BrushFormat::Abr   ? "abr"
                      : fmt == io::brush::BrushFormat::Sut ? "sut"
                                                           : "unknown";
    // 🔴 번역 못 한 것은 **전부 notes 로 올린다.** 조용히 버리지 않는다(docs/02 5절).
    std::vector<std::string> notes;
    for (const auto& n : imported.value().report.notes) {
        notes.push_back(n.message);
    }
    Json arr = Json::array();
    for (auto& p : imported.value().presets) {
        const BrushId id = s.addBrush(std::move(p), src, notes);
        for (const BrushEntry& b : s.brushes()) {
            if (b.id == id) {
                arr.push(brushToJson(b, false));
            }
        }
    }
    Json noteArr = Json::array();
    for (const std::string& n : notes) {
        noteArr.push(Json::string(n));
    }
    Json out = Json::object();
    out.set("imported", std::move(arr));
    out.set("format", Json::string(src));
    out.set("notes", std::move(noteArr));
    return Ok(std::move(out));
}

Result<Json> brushSet(AgentSession& s, const Json& req) {
    const Result<const BrushEntry*> b = s.resolveBrush(req["brush"]);
    if (!b.ok()) {
        return b.error();
    }
    s.setCurrentBrush(b.value()->id);
    Json out = Json::object();
    out.set("brush", brushToJson(*b.value(), true));
    return Ok(std::move(out));
}

Result<Json> brushDescribe(AgentSession& s, const Json& req) {
    const Result<const BrushEntry*> b = s.resolveBrush(req["brush"]);
    if (!b.ok()) {
        return b.error();
    }
    const BrushEntry& e = *b.value();
    Json out = brushToJson(e, e.id == s.currentBrush());
    Json tip = Json::object();
    tip.set("kind", Json::string(e.preset.tip.kind == brush::TipKind::Bitmap ? "bitmap"
                                                                            : "procedural"));
    tip.set("diameter", Json::number(static_cast<f64>(e.preset.tip.diameter)));
    tip.set("hardness", Json::number(static_cast<f64>(e.preset.tip.hardness)));
    tip.set("angle", Json::number(static_cast<f64>(e.preset.tip.angle)));
    tip.set("aspectRatio", Json::number(static_cast<f64>(e.preset.tip.aspectRatio)));
    if (e.preset.tip.kind == brush::TipKind::Bitmap) {
        tip.set("bitmapW", Json::integer(static_cast<i64>(e.preset.tip.bitmap.width)));
        tip.set("bitmapH", Json::integer(static_cast<i64>(e.preset.tip.bitmap.height)));
    }
    out.set("tip", std::move(tip));

    Json dyn = Json::array();
    for (const brush::DynamicLink& l : e.preset.dynamics) {
        Json j = Json::object();
        j.set("input", Json::string(brush::dynamicInputName(l.input)));
        j.set("output", Json::string(brush::dynamicOutputName(l.output)));
        j.set("amount", Json::number(static_cast<f64>(l.amount)));
        j.set("points", Json::integer(static_cast<i64>(l.curve.points.size())));
        dyn.push(std::move(j));
    }
    out.set("dynamicLinks", std::move(dyn));

    Json notes = Json::array();
    for (const std::string& n : e.notes) {
        notes.push(Json::string(n));
    }
    out.set("importNotes", std::move(notes));
    return Ok(std::move(out));
}

} // namespace mari::agent::ops
