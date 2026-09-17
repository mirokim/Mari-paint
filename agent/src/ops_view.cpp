// Mari Paint — 시각 연산 (docs/05 2.1). render · thumbnail · compare.
//
// 🔴 이 세 연산은 **이미지를 만드는 일 자체**를 하지 않는다.
//    시각 피드백은 디스패치가 모든 연산에 공통으로 씌운다(session.cpp).
//    여기서 하는 일은 "어디를 보여 줄지"를 정하는 것뿐이다 —
//    그래서 `render` 와 `stroke` 가 **같은 코드로** 그림을 만든다. 두 벌이 없다.
#include <mari/agent/session.hpp>

#include <mari/app/document.hpp>

namespace mari::agent::ops {

Result<Json> render(AgentSession& s, const Json&) {
    const Result<app::Document*> d = s.requireDocument();
    if (!d.ok()) {
        return d.error();
    }
    const Size sz = d.value()->canvasSize();
    Json out = Json::object();
    out.set("canvas", jsonRect(Rect{0, 0, sz.width, sz.height}));
    // view 를 주지 않으면 이 연산의 기본 모드(full)로 캔버스 전체가 실린다.
    return Ok(std::move(out));
}

Result<Json> thumbnail(AgentSession& s, const Json& req) {
    const Result<app::Document*> d = s.requireDocument();
    if (!d.ok()) {
        return d.error();
    }
    const i64 max = req["max"].asInt(128);
    if (max < 1 || max > kMaxViewSide) {
        return Err("max 는 1..4096 이다", ErrorCode::InvalidArgument);
    }
    app::Document* doc = d.value();
    const Size sz = doc->canvasSize();

    // 썸네일은 크기가 정해진 그림이라 여기서 직접 만든다(디스패치의 기본 max 와 다르다).
    ViewSpec spec;
    spec.mode = ViewMode::Full;
    spec.max = static_cast<i32>(max);
    Result<ViewResult> r = renderView(doc->layers(), spec, Rect{0, 0, sz.width, sz.height});
    if (!r.ok()) {
        return r.error();
    }
    Json out = Json::object();
    out.set("canvas", jsonRect(Rect{0, 0, sz.width, sz.height}));
    out.set("thumbnail", std::move(r).value().image);
    return Ok(std::move(out));
}

Result<Json> compare(AgentSession& s, const Json& req) {
    const Result<app::Document*> d = s.requireDocument();
    if (!d.ok()) {
        return d.error();
    }
    app::Document* doc = d.value();

    const Json& addr = req["snapshot"];
    const DocSnapshot* snap = nullptr;
    if (addr.isNumber()) {
        snap = s.snapshots().find(static_cast<SnapshotId>(addr.asInt()));
    } else if (addr.isString()) {
        snap = s.snapshots().findByLabel(addr.asString());
    } else {
        return Err("snapshot 은 id(정수)나 이름표(문자열)여야 한다", ErrorCode::InvalidArgument);
    }
    if (snap == nullptr) {
        return Err("그런 스냅샷이 없다", ErrorCode::NotFound);
    }

    Result<DocSnapshot> now = takeSnapshot(doc->layers(), std::string{}, doc->selectionMask());
    if (!now.ok()) {
        return now.error();
    }
    const SnapshotDiff diff = diffSnapshots(*snap, now.value());

    Json out = Json::object();
    out.set("snapshot", Json::integer(snap->id));
    out.set("identical", Json::boolean(diff.identical()));
    out.set("changedTiles", Json::integer(static_cast<i64>(diff.changedTiles)));
    out.set("area", jsonRect(diff.area));
    // 🔴 바뀐 영역을 그대로 보여 준다. "무엇이 달라졌나"를 말로 설명하지 않는다.
    //    (dirty 를 이 영역으로 세워 두면 디스패치의 공통 경로가 그림을 붙인다.)
    s.noteDirty(diff.area);
    return Ok(std::move(out));
}

} // namespace mari::agent::ops
