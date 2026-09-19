// Mari Paint — 문서·스냅샷·일괄·이벤트 연산. 표는 capabilities.cpp 에 있다.
#include <mari/agent/session.hpp>

#include <cctype>

#include <mari/app/document.hpp>
#include <mari/core/compositor.hpp>
#include <mari/crypto/sha256.hpp>

#include <cstdio>

namespace mari::agent::ops {
namespace {

/// 퍼센트를 사람이 읽는 말로. describe() 가 쓴다.
std::string fillWord(f64 coverage) {
    if (coverage <= 0.0005) {
        return "비어있음";
    }
    if (coverage >= 0.995) {
        return "채워짐";
    }
    char buf[32];
    const int n = std::snprintf(buf, sizeof(buf), "%.0f%% 채워짐", coverage * 100.0);
    return n > 0 ? std::string(buf, static_cast<usize>(n)) : std::string("일부 채워짐");
}

void describeLayers(const std::vector<LayerPtr>& list, const RoleTags& roles, const Size& canvas,
                    LayerId active, std::string& out, int depth) {
    // docs/05 2.4 의 예시 순서 그대로 **아래(배경)부터** 읽는다:
    //   "배경(채워짐), 러프(12% 채워짐), 선화(비어있음), 채색(비어있음)"
    for (usize i = 0; i < list.size(); ++i) {
        const LayerPtr& l = list[i];
        if (!out.empty()) {
            out += ", ";
        }
        for (int d = 0; d < depth; ++d) {
            out += "└";
        }
        out += l->name();
        if (l->kind() == LayerKind::Group) {
            out += "(그룹 " + std::to_string(l->children().size()) + "장)";
        } else {
            out += "(" + fillWord(layerCoverage(*l, canvas));
            if (!l->visible()) {
                out += ", 숨김";
            }
            if (l->locked()) {
                out += ", 잠김";
            }
            if (l->opacity() < 0.999f) {
                char buf[24];
                const int n = std::snprintf(buf, sizeof(buf), ", 불투명도 %.0f%%",
                                            static_cast<f64>(l->opacity()) * 100.0);
                if (n > 0) {
                    out.append(buf, static_cast<usize>(n));
                }
            }
            if (l->blendMode() != BlendMode::Normal) {
                out += std::string(", ") + blendModeName(l->blendMode());
            }
            if (l->id() == active) {
                out += ", 편집 중";
            }
            out += ")";
        }
        describeLayers(l->children(), roles, canvas, active, out, depth + 1);
    }
}

usize countLayers(const std::vector<LayerPtr>& list) {
    usize n = 0;
    for (const LayerPtr& l : list) {
        n += 1 + countLayers(l->children());
    }
    return n;
}

void layersToJson(const std::vector<LayerPtr>& list, const RoleTags& roles, const Size& canvas,
                  Json& out) {
    for (const LayerPtr& l : list) {
        out.push(layerToJson(*l, roles, canvas));
        layersToJson(l->children(), roles, canvas, out);
    }
}

/// 스냅샷 하나를 JSON 으로.
Json snapshotToJson(const DocSnapshot& s) {
    Json j = Json::object();
    j.set("id", Json::integer(s.id));
    j.set("label", Json::string(s.label));
    if (s.parentSnapshot != kInvalidSnapshotId) {
        j.set("branchedFrom", Json::integer(s.parentSnapshot));
    }
    j.set("layers", Json::integer(static_cast<i64>(s.nodes.size())));
    j.set("tiles", Json::integer(static_cast<i64>(s.totalTiles())));
    // 🔴 픽셀은 공유된다. 여기 적히는 것은 **인덱스 비용**뿐이다.
    j.set("approxOverheadBytes", Json::integer(static_cast<i64>(s.approxOverheadBytes())));
    j.set("sharesPixels", Json::boolean(true));
    return j;
}

Result<const DocSnapshot*> findSnapshot(AgentSession& s, const Json& addr, bool allowLatest) {
    if (addr.isNull()) {
        if (!allowLatest || s.snapshots().size() == 0) {
            return Err("스냅샷을 지목해라(snapshot: id 또는 이름표)", ErrorCode::NotFound);
        }
        return Ok(&s.snapshots().items().back());
    }
    if (addr.isNumber()) {
        const DocSnapshot* f = s.snapshots().find(static_cast<SnapshotId>(addr.asInt()));
        if (f == nullptr) {
            return Err("그런 스냅샷 id 가 없다: " + std::to_string(addr.asInt()),
                       ErrorCode::NotFound);
        }
        return Ok(f);
    }
    if (!addr.isString()) {
        return Err("snapshot 은 id(정수)나 이름표(문자열)여야 한다", ErrorCode::InvalidArgument);
    }
    const DocSnapshot* f = s.snapshots().findByLabel(addr.asString());
    if (f == nullptr) {
        return Err("그런 이름표의 스냅샷이 없다: \"" + addr.asString() + "\"", ErrorCode::NotFound);
    }
    return Ok(f);
}

Json diffToJson(const SnapshotDiff& d) {
    Json j = Json::object();
    j.set("identical", Json::boolean(d.identical()));
    j.set("changedTiles", Json::integer(static_cast<i64>(d.changedTiles)));
    j.set("area", jsonRect(d.area));
    auto ids = [](const std::vector<LayerId>& v) {
        Json a = Json::array();
        for (const LayerId id : v) {
            a.push(Json::integer(id));
        }
        return a;
    };
    j.set("addedLayers", ids(d.addedLayers));
    j.set("removedLayers", ids(d.removedLayers));
    j.set("changedLayers", ids(d.changedLayers));
    j.set("propChangedLayers", ids(d.propChangedLayers));
    return j;
}

} // namespace

// ── 문서 ─────────────────────────────────────────────────────────────────

Result<Json> docCreate(AgentSession& s, const Json& req) {
    const ApiLimits lim;
    const i64 w = req["width"].asInt(0);
    const i64 h = req["height"].asInt(0);
    if (w <= 0 || h <= 0 || w > lim.maxCanvas || h > lim.maxCanvas) {
        return Err("캔버스 크기는 1..16384 여야 한다", ErrorCode::InvalidArgument);
    }
    const Result<app::IDocumentBridge*> d =
        s.application().createDocument(static_cast<i32>(w), static_cast<i32>(h));
    if (!d.ok()) {
        return d.error();
    }
    app::Document* doc = s.document();
    Json out = Json::object();
    out.set("canvas", jsonRect(Rect{0, 0, static_cast<i32>(w), static_cast<i32>(h)}));
    Json layers = Json::array();
    layersToJson(doc->layers().roots(), s.roles(), doc->canvasSize(), layers);
    out.set("layers", std::move(layers));
    out.set("activeLayer", Json::integer(doc->layers().activeLayer()));
    return Ok(std::move(out));
}

Result<Json> docOpen(AgentSession& s, const Json& req) {
    if (!req["path"].isString()) {
        return Err("path 는 문자열이어야 한다", ErrorCode::InvalidArgument);
    }
    const Result<app::IDocumentBridge*> d = s.application().open(req["path"].asString());
    if (!d.ok()) {
        return d.error();
    }
    app::Document* doc = s.document();
    const Size sz = doc->canvasSize();
    Json out = Json::object();
    out.set("path", Json::string(doc->fullPath()));
    out.set("canvas", jsonRect(Rect{0, 0, sz.width, sz.height}));
    Json layers = Json::array();
    layersToJson(doc->layers().roots(), s.roles(), sz, layers);
    out.set("layers", std::move(layers));
    return Ok(std::move(out));
}

Result<Json> docSave(AgentSession& s, const Json& req) {
    const Result<app::Document*> d = s.requireDocument();
    if (!d.ok()) {
        return d.error();
    }
    app::Document* doc = d.value();
    std::string path = req["path"].isString() ? req["path"].asString() : doc->fullPath();
    if (path.empty()) {
        return Err("저장 경로가 없다 — path 를 줘라", ErrorCode::InvalidArgument);
    }
    std::string fmt = req["format"].isString() ? req["format"].asString() : std::string();
    if (fmt.empty()) {
        // 확장자로 고른다: .psd → psd, .png → png, 그 외 ora.
        const auto dot = path.rfind('.');
        std::string ext = dot == std::string::npos ? std::string() : path.substr(dot + 1);
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        fmt = (ext == "psd" || ext == "png") ? ext : std::string("ora");
    }
    const Result<void> r = doc->saveAs(path, fmt);
    if (!r.ok()) {
        return r.error();
    }
    Json out = Json::object();
    out.set("path", Json::string(path));
    out.set("format", Json::string(fmt));
    // 🔴 두 해시는 서로 다른 것이고, 이름으로 구분한다. 섞으면 대조하는 쪽이 반드시 어긋난다.
    //    - canvasHash: 합성 픽셀 해시("mari-canvas-hash/v1" 접두사). 파일과 값이 다르다.
    //    - fileHash  : 저장된 파일 바이트 그대로. `sha256sum <파일>` 과 값이 정확히 같다.
    //    해시를 낼 뿐 봉인하지 않는다 — 체인은 Sigan 의 몫이다(docs/03 2절).
    const Result<std::string> canvas = doc->canvasHash();
    if (canvas.ok()) {
        out.set("canvasHash", Json::string(canvas.value()));
    }
    const Result<std::string> file = crypto::sha256FileHex(path);
    if (file.ok()) {
        out.set("fileHash", Json::string(file.value()));
    }
    out.set("saved", Json::boolean(doc->isSaved()));
    return Ok(std::move(out));
}

Result<Json> docClose(AgentSession& s, const Json& req) {
    const Result<app::Document*> d = s.requireDocument();
    if (!d.ok()) {
        return d.error();
    }
    const bool save = req["save"].asBool(false);
    const Result<void> r = s.application().closeDocument(d.value(), save);
    if (!r.ok()) {
        return r.error();
    }
    // 문서가 사라지면 그 문서의 레이어 id 로 만든 스냅샷·역할 태그는 의미가 없다.
    s.snapshots().clear();
    Json out = Json::object();
    out.set("closed", Json::boolean(true));
    out.set("saved", Json::boolean(save));
    out.set("openDocuments", Json::integer(static_cast<i64>(s.application().documentCount())));
    return Ok(std::move(out));
}

// 🔴 docs/05 2.4 — 캔버스 상태를 **사람이 읽는 한 문장**으로.
//    AI 가 JSON 트리를 파싱하는 것보다 싸고 정확하다.
Result<Json> docDescribe(AgentSession& s, const Json&) {
    const Result<app::Document*> d = s.requireDocument();
    if (!d.ok()) {
        return d.error();
    }
    app::Document* doc = d.value();
    const Size sz = doc->canvasSize();
    const LayerTree& tree = doc->layers();

    std::string layers;
    describeLayers(tree.roots(), s.roles(), sz, tree.activeLayer(), layers, 0);
    const usize n = countLayers(tree.roots());

    std::string text = std::to_string(sz.width) + "x" + std::to_string(sz.height) + ", 레이어 " +
                       std::to_string(n) + "개: " + layers;
    if (!doc->selection().isEmpty()) {
        const Rect r = doc->selection();
        const SelectionMask& mask = doc->selectionMask();
        text += ". 선택 영역 " + std::to_string(r.width) + "x" + std::to_string(r.height) + " @ (" +
                std::to_string(r.x) + "," + std::to_string(r.y) + ")";
        // 🔴 경계 상자만 말하면 올가미·페더 선택이 사각형처럼 들린다. 실제로 선택된
        //    픽셀 수를 같이 적어서 "사각형이 아니다"가 문장에서 드러나게 한다.
        const i64 box = static_cast<i64>(r.width) * r.height;
        const auto sel = static_cast<i64>(mask.selectedPixels());
        if (sel < box) {
            text += ", 그 안에서 실제로 선택된 픽셀 " + std::to_string(sel) + "개";
        }
    }
    if (s.snapshots().size() != 0) {
        text += ". 스냅샷 " + std::to_string(s.snapshots().size()) + "개";
    }
    // 🔴 사실만 적는다. 등급은 없다(docs/03 2절 · docs/05 3.2).
    // 🔴 세 축을 **따로** 말한다(docs/06 결정 ② · 6절 H3). 붓질 수만 말하면
    //    캔버스 전체를 칠한 fill 한 번이 "AI 획 0개"로 들린다 — 참인 숫자로 만든 거짓이다.
    text += ". 이 세션의 AI 획 " + std::to_string(s.originStats().agent()) + "개, 영역 연산 " +
            std::to_string(s.regionOpStats().agent()) + "개, 바뀐 타일 " +
            std::to_string(s.changedTiles()) + "개(에이전트 " +
            std::string(s.agentId().view()) + ")";

    Json out = Json::object();
    out.set("text", Json::string(text));
    out.set("canvas", jsonRect(Rect{0, 0, sz.width, sz.height}));
    out.set("layerCount", Json::integer(static_cast<i64>(n)));
    Json list = Json::array();
    layersToJson(tree.roots(), s.roles(), sz, list);
    out.set("layers", std::move(list));
    out.set("activeLayer", Json::integer(tree.activeLayer()));
    out.set("selection", jsonRect(doc->selection()));
    out.set("selectionKind", Json::string(doc->selectionMask().isAll()
                                              ? "all"
                                              : (doc->selectionMask().isEmpty() ? "empty"
                                                                                : "mask")));
    out.set("selectionTiles", Json::integer(static_cast<i64>(doc->selectionMask().tileCount())));
    out.set("selectedPixels",
            Json::integer(static_cast<i64>(doc->selectionMask().selectedPixels())));
    out.set("path", Json::string(doc->fullPath()));
    out.set("saved", Json::boolean(doc->isSaved()));
    out.set("undoDepth", Json::integer(static_cast<i64>(doc->undoStack().undoCount())));
    out.set("snapshots", Json::integer(static_cast<i64>(s.snapshots().size())));

    // 출처 집계 — 숫자와 비율까지만. 판정은 Sigan 의 몫이다.
    // 🔴 세 축을 따로 낸다. 하나로 합친 "기여도"는 만들지 않는다 —
    //    가중치를 고르는 순간 그게 판정이고, 판정은 Sigan 의 몫이다(docs/03 2절).
    Json origins = Json::object();
    origins.set("agent", Json::integer(s.originStats().agent()));
    origins.set("human", Json::integer(s.originStats().human()));
    origins.set("total", Json::integer(s.originStats().total()));
    origins.set("agentRatio", Json::number(s.originStats().agentRatio()));
    origins.set("agentId", Json::string(std::string(s.agentId().view())));
    // 축 B — 영역 연산(fill·erase·gradient·transform). 붓질과 같은 칸이 아니다.
    Json regionOps = Json::object();
    regionOps.set("agent", Json::integer(s.regionOpStats().agent()));
    regionOps.set("human", Json::integer(s.regionOpStats().human()));
    regionOps.set("total", Json::integer(s.regionOpStats().total()));
    origins.set("regionOps", std::move(regionOps));
    // 축 C — 이 세션이 바꾼 타일 수(픽셀이 아니다).
    origins.set("changedTiles", Json::integer(s.changedTiles()));
    origins.set("recorded", Json::boolean(doc->recorder() != nullptr));
    out.set("origins", std::move(origins));
    return Ok(std::move(out));
}

// 🔴 docs/06 결정 ② — 출처별 집계를 **세 축**으로 내놓는다.
//    MCP·CLI·JSON-RPC 가 전부 이 연산 하나를 통해 같은 숫자를 본다.
//
// 🔴 여기에 등급 문자열이 없다. 세 축을 가중합한 단일 "기여도"도 없다 —
//    가중치를 고르는 순간 그것이 판정이고, 판정은 Sigan 의 몫이다(docs/03 2절).
Result<Json> docOrigins(AgentSession& s, const Json&) {
    Json out = Json::object();
    out.set("agentId", Json::string(std::string(s.agentId().view())));
    out.set("schema", Json::string("mari-origin-tally/1"));

    const auto axis = [](const StrokeOriginStats& st) {
        Json counts = Json::object();
        counts.set("humanPen", Json::integer(st.count(StrokeOrigin::HumanPen)));
        counts.set("humanMouse", Json::integer(st.count(StrokeOrigin::HumanMouse)));
        counts.set("agent", Json::integer(st.count(StrokeOrigin::Agent)));
        counts.set("imported", Json::integer(st.count(StrokeOrigin::Imported)));
        counts.set("filter", Json::integer(st.count(StrokeOrigin::Filter)));
        // 출처가 기록되지 않은 것. **0 이어야 정상이다.** 숨기지 않는다.
        counts.set("unspecified", Json::integer(st.unspecified()));
        Json a = Json::object();
        a.set("byOrigin", std::move(counts));
        a.set("human", Json::integer(st.human()));
        a.set("agent", Json::integer(st.agent()));
        a.set("total", Json::integer(st.total()));
        return a;
    };

    // 축 A — 붓질. "몇 번 그었나".
    Json strokes = axis(s.originStats());
    strokes.set("agentRatio", Json::number(s.originStats().agentRatio()));
    out.set("brushStrokes", std::move(strokes));
    // 축 B — 영역 연산. fill 한 번을 붓질 한 번과 같은 칸에 세지 않는다(H3).
    out.set("regionOps", axis(s.regionOpStats()));
    // 축 C — 변경된 타일 수. "얼마나 칠했나". 픽셀이 아니라 타일이다
    //        (정수라 안티에일리어싱 가장자리나 덧칠로 부풀릴 수 없다).
    Json tiles = Json::object();
    tiles.set("agent", Json::integer(s.changedTiles()));
    tiles.set("tileSize", Json::integer(ApiLimits{}.tileSize));
    out.set("changedTiles", std::move(tiles));

    // 이 세션의 숫자는 **세션이 만든 것**만 센다. 문서에 붙은 기록 구간은 사람 획까지
    // 포함할 수 있으므로, 기록이 붙어 있는지도 같이 알려 준다(docs/06 결정 ③·⑤).
    const app::Document* doc = s.document();
    out.set("recorded", Json::boolean(doc != nullptr && doc->recorder() != nullptr));
    if (doc != nullptr && doc->recorder() != nullptr) {
        const agent::RecordingTally& t = doc->recorder()->tally();
        Json seg = Json::object();
        seg.set("humanBrushStrokes", Json::integer(t.strokes.human()));
        seg.set("agentBrushStrokes", Json::integer(t.strokes.agent()));
        seg.set("humanRegionOps", Json::integer(t.regionOps.human()));
        seg.set("agentRegionOps", Json::integer(t.regionOps.agent()));
        seg.set("humanChangedTiles", Json::integer(t.humanTiles()));
        seg.set("agentChangedTiles", Json::integer(t.agentTiles()));
        seg.set("broken", Json::boolean(doc->recordingBroken()));
        // 🔴 "문서 작업 구간"이다. agent-api 세션이 아니다(docs/06 결정 ⑤).
        out.set("documentSegment", std::move(seg));
    }
    return Ok(std::move(out));
}

Result<Json> capabilities(AgentSession& s, const Json&) { return Ok(buildCapabilities(s)); }

// ── 스냅샷 (docs/05 2.2) ─────────────────────────────────────────────────

Result<Json> snapshotTake(AgentSession& s, const Json& req) {
    const Result<app::Document*> d = s.requireDocument();
    if (!d.ok()) {
        return d.error();
    }
    std::string label = req["label"].isString() ? req["label"].asString() : std::string{};
    Result<DocSnapshot> snap = takeSnapshot(d.value()->layers(), std::move(label),
                                            d.value()->selectionMask());
    if (!snap.ok()) {
        return snap.error();
    }
    const SnapshotId id = s.snapshots().put(std::move(snap).value());
    const DocSnapshot* stored = s.snapshots().find(id);
    Json out = Json::object();
    out.set("snapshot", snapshotToJson(*stored));
    out.set("count", Json::integer(static_cast<i64>(s.snapshots().size())));
    out.set("evicted", Json::integer(s.snapshots().evicted()));
    return Ok(std::move(out));
}

Result<Json> snapshotRestore(AgentSession& s, const Json& req) {
    const Result<app::Document*> d = s.requireDocument();
    if (!d.ok()) {
        return d.error();
    }
    const Result<const DocSnapshot*> snap = findSnapshot(s, req["snapshot"], true);
    if (!snap.ok()) {
        return snap.error();
    }
    app::Document* doc = d.value();

    // 되돌리기 전후를 견줘 **바뀐 영역**을 알아낸다 — 그래야 view:dirty 가 의미를 갖는다.
    Result<DocSnapshot> before = takeSnapshot(doc->layers(), std::string{}, doc->selectionMask());
    if (!before.ok()) {
        return before.error();
    }
    SelectionMask selection;
    const Result<void> r = restoreSnapshot(doc->layers(), *snap.value(), &selection);
    if (!r.ok()) {
        return r.error();
    }
    doc->setSelectionMask(std::move(selection));
    doc->markDirty();

    Result<DocSnapshot> after = takeSnapshot(doc->layers(), std::string{}, doc->selectionMask());
    if (!after.ok()) {
        return after.error();
    }
    const SnapshotDiff diff = diffSnapshots(before.value(), after.value());
    s.noteDirty(diff.area);

    Json out = Json::object();
    out.set("snapshot", snapshotToJson(*snap.value()));
    out.set("diff", diffToJson(diff));
    return Ok(std::move(out));
}

Result<Json> snapshotBranch(AgentSession& s, const Json& req) {
    // 분기 = 그 자리로 되돌린 뒤, 거기서 갈라져 나온 새 스냅샷을 남긴다.
    Json restoreReq = Json::object();
    restoreReq.set("op", Json::string("restore"));
    if (req.has("snapshot")) {
        restoreReq.set("snapshot", req["snapshot"]);
    }
    const Result<Json> restored = snapshotRestore(s, restoreReq);
    if (!restored.ok()) {
        return restored;
    }
    const Result<const DocSnapshot*> from = findSnapshot(s, req["snapshot"], true);
    if (!from.ok()) {
        return from.error();
    }
    const SnapshotId fromId = from.value()->id;

    const Result<app::Document*> d = s.requireDocument();
    if (!d.ok()) {
        return d.error();
    }
    std::string label = req["label"].isString() ? req["label"].asString() : std::string{};
    Result<DocSnapshot> snap =
        takeSnapshot(d.value()->layers(), std::move(label), d.value()->selectionMask());
    if (!snap.ok()) {
        return snap.error();
    }
    snap.value().parentSnapshot = fromId;
    const SnapshotId id = s.snapshots().put(std::move(snap).value());

    Json out = Json::object();
    out.set("snapshot", snapshotToJson(*s.snapshots().find(id)));
    out.set("restored", restored.value());
    return Ok(std::move(out));
}

Result<Json> snapshotDiff(AgentSession& s, const Json& req) {
    const Result<app::Document*> d = s.requireDocument();
    if (!d.ok()) {
        return d.error();
    }
    const Result<const DocSnapshot*> from = findSnapshot(s, req["from"], false);
    if (!from.ok()) {
        return from.error();
    }

    DocSnapshot nowSnap;
    const DocSnapshot* to = nullptr;
    if (req.has("to")) {
        const Result<const DocSnapshot*> t = findSnapshot(s, req["to"], false);
        if (!t.ok()) {
            return t.error();
        }
        to = t.value();
    } else {
        Result<DocSnapshot> cur =
            takeSnapshot(d.value()->layers(), std::string{}, d.value()->selectionMask());
        if (!cur.ok()) {
            return cur.error();
        }
        nowSnap = std::move(cur).value();
        to = &nowSnap;
    }

    const SnapshotDiff diff = diffSnapshots(*from.value(), *to);
    Json out = Json::object();
    out.set("from", Json::integer(from.value()->id));
    out.set("to", to->id == kInvalidSnapshotId ? Json::string("current") : Json::integer(to->id));
    out.set("diff", diffToJson(diff));
    return Ok(std::move(out));
}

// ── 일괄 (docs/05 2.5) ───────────────────────────────────────────────────

Result<Json> batch(AgentSession& s, const Json& req) {
    const Json& list = req["ops"];
    if (!list.isArray()) {
        return Err("batch.ops 는 배열이어야 한다", ErrorCode::InvalidArgument);
    }
    const ApiLimits lim;
    if (static_cast<i64>(list.size()) > lim.maxBatchOps) {
        return Err("batch 가 너무 크다(최대 " + std::to_string(lim.maxBatchOps) + "개)",
                   ErrorCode::InvalidArgument);
    }
    const bool atomic = req["atomic"].asBool(true);
    const bool stopOnError = req["stopOnError"].asBool(true);

    // 🔴 원자성의 알맹이. O(1) 스냅샷이라 **큰 묶음도 안심하고 보낼 수 있다**(docs/05 2.5).
    DocSnapshot rollback;
    bool haveRollback = false;
    app::Document* doc = s.document();
    if (atomic) {
        const Result<app::Document*> d = s.requireDocument();
        if (!d.ok()) {
            return d.error();
        }
        doc = d.value();
        Result<DocSnapshot> snap = takeSnapshot(doc->layers(), std::string{}, doc->selectionMask());
        if (!snap.ok()) {
            return snap.error();
        }
        rollback = std::move(snap).value();
        haveRollback = true;
    }

    const Rect dirtyBefore = s.pendingDirty();
    Json results = Json::array();
    usize done = 0;
    bool failed = false;
    Json failure = Json::null();

    for (usize i = 0; i < list.size(); ++i) {
        const Json& one = list.at(i);
        if (!one.isObject() || !one["op"].isString()) {
            failure = Json::string("ops[" + std::to_string(i) + "] 에 op 가 없다");
            failed = true;
        } else {
            const OpSpec* spec = findOp(one["op"].asString());
            if (spec == nullptr) {
                failure = Json::string("ops[" + std::to_string(i) + "] 모르는 연산: " +
                                       one["op"].asString());
                failed = true;
            } else if (one["op"].asString() == "batch") {
                // 중첩 batch 는 롤백 경계가 겹쳐 헷갈린다. 정직하게 막는다.
                failure = Json::string("batch 안에 batch 를 넣을 수 없다");
                failed = true;
            } else {
                Result<Json> r = s.runOp(*spec, one);
                if (r.ok()) {
                    Json entry = Json::object();
                    entry.set("op", one["op"]);
                    entry.set("ok", Json::boolean(true));
                    if (!r.value().isNull()) {
                        entry.set("result", std::move(r).value());
                    }
                    results.push(std::move(entry));
                    ++done;
                    continue;
                }
                Json entry = Json::object();
                entry.set("op", one["op"]);
                entry.set("ok", Json::boolean(false));
                entry.set("message", Json::string(r.error().message));
                results.push(entry);
                failure = std::move(entry);
                failed = true;
            }
        }

        if (failed) {
            if (atomic) {
                break;
            }
            if (stopOnError) {
                break;
            }
            failed = false; // atomic 이 아니면 계속 간다. 실패는 결과에 남는다.
        }
    }

    Json out = Json::object();
    out.set("atomic", Json::boolean(atomic));
    out.set("requested", Json::integer(static_cast<i64>(list.size())));
    out.set("completed", Json::integer(static_cast<i64>(done)));
    out.set("results", std::move(results));

    if (failed && atomic && haveRollback) {
        SelectionMask selection;
        const Result<void> back = restoreSnapshot(doc->layers(), rollback, &selection);
        if (!back.ok()) {
            // 롤백조차 실패했으면 그걸 숨기면 안 된다. 캔버스 상태를 못 믿게 된다.
            return Err("batch 롤백에 실패했다(캔버스 상태를 신뢰하지 마라): " + back.message(),
                       ErrorCode::Unknown);
        }
        doc->setSelectionMask(std::move(selection));
        // 롤백했으니 이번 batch 가 만든 더티는 없던 일이다. 화면은 되돌아간 영역을 봐야 한다.
        s.noteDirty(dirtyBefore);
        out.set("rolledBack", Json::boolean(true));
        out.set("failure", failure);
        // 🔴 원자적 batch 의 실패는 **연산 전체의 실패**다. ok:true 로 돌려주면
        //    에이전트가 성공한 줄 안다.
        return Err("batch 가 중간에 실패해 전부 롤백했다: " +
                       (failure.isObject() ? failure["message"].asString() : failure.asString()),
                   ErrorCode::InvalidArgument);
    }
    out.set("rolledBack", Json::boolean(false));
    if (!failure.isNull()) {
        out.set("failure", std::move(failure));
    }
    return Ok(std::move(out));
}

// ── 이벤트 (docs/05 2.7) ─────────────────────────────────────────────────

Result<Json> eventsSubscribe(AgentSession& s, const Json& req) {
    std::vector<std::string> kinds;
    const Json& want = req["events"];
    if (want.isArray()) {
        for (usize i = 0; i < want.size(); ++i) {
            if (!want.at(i).isString()) {
                return Err("events 는 문자열 배열이어야 한다", ErrorCode::InvalidArgument);
            }
            kinds.push_back(want.at(i).asString());
        }
    } else if (!want.isNull()) {
        return Err("events 는 배열이어야 한다", ErrorCode::InvalidArgument);
    }
    s.subscribeEvents(std::move(kinds));
    Json out = Json::object();
    out.set("subscribed", Json::boolean(true));
    Json known = Json::array();
    for (const char* k : {"documentOpened", "documentSaved", "canvasSnapshot", "viewChanged",
                          "paste", "undo", "layerChanged", "strokeCompleted"}) {
        known.push(Json::string(k));
    }
    out.set("kinds", std::move(known));
    return Ok(std::move(out));
}

Result<Json> eventsUnsubscribe(AgentSession& s, const Json&) {
    s.unsubscribeEvents();
    Json out = Json::object();
    out.set("subscribed", Json::boolean(false));
    return Ok(std::move(out));
}

Result<Json> eventsPoll(AgentSession& s, const Json& req) {
    const i64 max = req["max"].asInt(64);
    if (max <= 0) {
        return Err("max 는 1 이상이어야 한다", ErrorCode::InvalidArgument);
    }
    std::vector<QueuedEvent> got = s.drainEvents(static_cast<usize>(max));
    Json arr = Json::array();
    for (QueuedEvent& e : got) {
        Json j = Json::object();
        j.set("kind", Json::string(e.kind));
        j.set("data", std::move(e.data));
        arr.push(std::move(j));
    }
    Json out = Json::object();
    out.set("subscribed", Json::boolean(s.subscribed()));
    out.set("events", std::move(arr));
    out.set("remaining", Json::integer(static_cast<i64>(s.queuedEventCount())));
    return Ok(std::move(out));
}

} // namespace mari::agent::ops
