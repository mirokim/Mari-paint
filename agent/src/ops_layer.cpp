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

// ── 선택 ─────────────────────────────────────────────────────────────────

Result<Json> select(AgentSession& s, const Json& req) {
    const Result<app::Document*> d = s.requireDocument();
    if (!d.ok()) {
        return d.error();
    }
    app::Document* doc = d.value();
    if (req.has("mode") && req["mode"].asString() != "rect") {
        // 🔴 올가미·색상 선택은 선택 마스크 저장소가 있어야 한다. 없는 걸 있는 척하지 않는다.
        return Err("지금은 rect 선택만 된다(요청: \"" + req["mode"].asString() +
                       "\"). 선택 마스크 저장소가 들어오면 그때 연다",
                   ErrorCode::Unsupported);
    }
    if (!req.has("region")) {
        doc->setSelection(Rect{});
        Json out = Json::object();
        out.set("selection", jsonRect(Rect{}));
        return Ok(std::move(out));
    }
    const Size sz = doc->canvasSize();
    const Result<Rect> r = resolveRegion(doc->layers(), s.roles(), req["region"], doc->selection(),
                                         Rect{0, 0, sz.width, sz.height});
    if (!r.ok()) {
        return r.error();
    }
    const Rect clipped = r.value().intersected(Rect{0, 0, sz.width, sz.height});
    doc->setSelection(clipped);
    Json out = Json::object();
    out.set("selection", jsonRect(clipped));
    out.set("requested", jsonRect(r.value()));
    return Ok(std::move(out));
}

Result<Json> selectInvert(AgentSession&, const Json&) {
    // 표에서 supported=false 라 여기까지 오지 않는다. 그래도 정직하게 답한다.
    return Err("선택 반전은 사각형 하나짜리 선택 모델로는 표현할 수 없다",
               ErrorCode::Unsupported);
}

Result<Json> selectExpand(AgentSession& s, const Json& req) {
    const Result<app::Document*> d = s.requireDocument();
    if (!d.ok()) {
        return d.error();
    }
    app::Document* doc = d.value();
    const Rect cur = doc->selection();
    if (cur.isEmpty()) {
        return Err("선택 영역이 비어 있다", ErrorCode::NotFound);
    }
    const i64 by = req["by"].asInt(0);
    if (by < -100000 || by > 100000) {
        return Err("by 가 너무 크다", ErrorCode::InvalidArgument);
    }
    const auto n = static_cast<i32>(by);
    Rect grown{cur.x - n, cur.y - n, cur.width + 2 * n, cur.height + 2 * n};
    if (grown.width <= 0 || grown.height <= 0) {
        grown = Rect{};
    }
    const Size sz = doc->canvasSize();
    grown = grown.intersected(Rect{0, 0, sz.width, sz.height});
    doc->setSelection(grown);
    Json out = Json::object();
    out.set("selection", jsonRect(grown));
    return Ok(std::move(out));
}

Result<Json> selectFeather(AgentSession&, const Json&) {
    return Err("부드러운 선택을 담을 마스크 저장소가 아직 없다", ErrorCode::Unsupported);
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
