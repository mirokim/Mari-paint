// 선택 마스크가 **에이전트 API 를 통해** 실제로 도는가 (docs/05 4절 선택 행 · 8.3)
//
// docs/05 8.3 은 이렇게 적혀 있었다: "그 'current' 가 담을 수 있는 것은 사각형 하나다.
// 올가미·색상 선택·반전·페더는 마스크 저장소가 없어서 못 한다."
// 이 파일이 그 문장을 무효로 만드는 증거다.
//
// 여기서 증명하는 것:
//   · 선택 방식 6종이 API 로 실제로 돈다(rect·ellipse·lasso·color·content·alpha)
//   · 불린 결합 4종이 API 로 돈다
//   · 🔴 **선택 밖에는 그려지지 않는다** — 획·fill·erase·gradient 넷 다
//   · 전체 선택은 타일 0개(메모리 0)이고, 응답이 그 사실을 말한다
//   · 반전·페더·팽창이 미지원 표시를 떼고 실제로 동작한다
//   · 선택이 스냅샷/되돌리기를 타고 살아 돌아온다
#include <mari/agent/capabilities.hpp>
#include <mari/agent/session.hpp>
#include <mari/app/document.hpp>
#include <mari/ora/image.hpp>
#include <mari/test/harness.hpp>

#include <memory>
#include <string>

using namespace mari;
using mari::agent::AgentSession;
using mari::agent::Json;

namespace {

Json req(const char* op) {
    Json j = Json::object();
    j.set("op", Json::string(op));
    j.set("view", Json::string("none"));
    return j;
}

Json rect(i32 x, i32 y, i32 w, i32 h) {
    Json a = Json::array();
    a.push(Json::integer(x));
    a.push(Json::integer(y));
    a.push(Json::integer(w));
    a.push(Json::integer(h));
    return a;
}

std::unique_ptr<AgentSession> sessionWith(mari::test::Context& mari_ctx, i32 w = 256, i32 h = 256) {
    auto s = AgentSession::open("claude/opus-5");
    CHECK(s.ok());
    if (!s.ok()) {
        return nullptr;
    }
    std::unique_ptr<AgentSession> session = std::move(s).value();
    Json create = req("doc.create");
    create.set("width", Json::integer(w));
    create.set("height", Json::integer(h));
    CHECK(session->execute(create)["ok"].asBool());
    return session;
}

/// 캔버스 한 픽셀을 읽는다. 활성 레이어의 타일에서 직접 읽는다 —
/// PNG 를 거치면 "무엇이 그려졌나"가 아니라 "인코더가 무엇을 했나"를 재게 된다.
Color8 pixelAt(AgentSession& s, i32 x, i32 y) {
    app::Document* doc = s.document();
    if (doc == nullptr) {
        return Color8{0, 0, 0, 0};
    }
    const LayerPtr l = doc->layers().find(doc->layers().activeLayer());
    if (!l || l->tiles() == nullptr) {
        return Color8{0, 0, 0, 0};
    }
    Result<ora::Image8> got = ora::readRegion(*l->tiles(), Rect{x, y, 1, 1});
    if (!got.ok() || got.value().pixels.size() < 4) {
        return Color8{0, 0, 0, 0};
    }
    const std::vector<u8>& p = got.value().pixels;
    return Color8{p[0], p[1], p[2], p[3]};
}

/// 지정 영역을 색으로 채운다(선택을 무시하고 싶을 때는 미리 선택을 해제한다).
Json fillRect(i32 x, i32 y, i32 w, i32 h, const char* color) {
    Json j = req("fill");
    j.set("region", rect(x, y, w, h));
    j.set("color", Json::string(color));
    return j;
}

Json clearSelection() { return req("select"); }

} // namespace

// ── 표에서 미지원 표시가 사라졌다 ────────────────────────────────────────

MARI_TEST(selection_ops_are_no_longer_marked_unsupported) {
    // docs/05 4절: "미지원 연산은 도구로 내보내지 않는다."
    // 넷 다 supported 가 되었으므로 MCP 도구로도 나간다(tests/mcp 가 그 수를 센다).
    for (const char* name : {"select", "select.invert", "select.expand", "select.feather"}) {
        const agent::OpSpec* op = agent::findOp(name);
        CHECK(op != nullptr);
        if (op != nullptr) {
            CHECK(op->supported);
            CHECK_EQ(std::string(op->unsupportedReason), std::string());
        }
    }
    // 🔴 위조 방지 규약은 그대로다 — 선택 파라미터가 늘어도 origin 은 끼어들 수 없다.
    for (const agent::OpSpec& op : agent::opTable()) {
        for (const agent::ParamSpec& p : op.params) {
            CHECK(std::string(p.name) != "origin");
            CHECK(std::string(p.name) != "agentId");
        }
    }
}

// ── 전체 선택 = 타일 0개 ────────────────────────────────────────────────

MARI_TEST(no_selection_means_no_tiles_and_no_limit) {
    std::unique_ptr<AgentSession> s = sessionWith(mari_ctx);
    if (s == nullptr) {
        return;
    }
    const Json d = s->execute(req("doc.describe"));
    CHECK(d["ok"].asBool());
    // 문서는 "제한 없음"으로 시작한다. 선택 사각형은 비어 있고 마스크는 타일 0개다.
    CHECK_EQ(d["result"]["selectionKind"].asString(), std::string("all"));
    CHECK_EQ(d["result"]["selectionTiles"].asInt(), i64{0});
    CHECK_EQ(d["result"]["selectedPixels"].asInt(), i64{256} * 256);

    Json sel = req("select");
    sel.set("region", rect(10, 10, 20, 20));
    const Json r = s->execute(sel);
    CHECK(r["ok"].asBool());
    CHECK_EQ(r["result"]["kind"].asString(), std::string("mask"));
    CHECK(r["result"]["tiles"].asInt() > 0);
    CHECK_EQ(r["result"]["selectedPixels"].asInt(), i64{400});

    // 해제하면 다시 타일 0개로 돌아간다 — 선택이 메모리를 붙잡아 두지 않는다.
    const Json cleared = s->execute(clearSelection());
    CHECK(cleared["ok"].asBool());
    CHECK_EQ(cleared["result"]["kind"].asString(), std::string("all"));
    CHECK_EQ(cleared["result"]["tiles"].asInt(), i64{0});
}

// ── 선택 방식 6종 ───────────────────────────────────────────────────────

MARI_TEST(all_six_selection_modes_work_through_the_api) {
    std::unique_ptr<AgentSession> s = sessionWith(mari_ctx);
    if (s == nullptr) {
        return;
    }
    // 원본 그림: 빨강 사각형 하나, 파랑 사각형 하나.
    CHECK(s->execute(fillRect(0, 0, 40, 40, "#ff0000"))["ok"].asBool());
    CHECK(s->execute(fillRect(100, 100, 40, 40, "#0000ff"))["ok"].asBool());

    { // rect
        Json j = req("select");
        j.set("mode", Json::string("rect"));
        j.set("region", rect(0, 0, 50, 50));
        const Json r = s->execute(j);
        CHECK(r["ok"].asBool());
        CHECK_EQ(r["result"]["selectedPixels"].asInt(), i64{2500});
    }
    { // ellipse — 내접 원의 면적이 사각형보다 작다
        Json j = req("select");
        j.set("mode", Json::string("ellipse"));
        j.set("region", rect(0, 0, 100, 100));
        const Json r = s->execute(j);
        CHECK(r["ok"].asBool());
        const i64 n = r["result"]["selectedPixels"].asInt();
        CHECK(n > 7600);  // πr² ≈ 7854
        CHECK(n < 8100);
    }
    { // lasso — 삼각형
        Json j = req("select");
        j.set("mode", Json::string("lasso"));
        Json pts = Json::array();
        for (const auto& p : {std::pair<i32, i32>{0, 0}, {100, 0}, {0, 100}}) {
            Json a = Json::array();
            a.push(Json::integer(p.first));
            a.push(Json::integer(p.second));
            pts.push(std::move(a));
        }
        j.set("points", std::move(pts));
        const Json r = s->execute(j);
        CHECK(r["ok"].asBool());
        const i64 n = r["result"]["selectedPixels"].asInt();
        CHECK(n > 4850);
        CHECK(n < 5150);
    }
    { // color — 빨강만
        Json j = req("select");
        j.set("mode", Json::string("color"));
        j.set("color", Json::string("#ff0000"));
        const Json r = s->execute(j);
        CHECK(r["ok"].asBool());
        CHECK_EQ(r["result"]["selectedPixels"].asInt(), i64{1600});
        CHECK_EQ(r["result"]["bounds"].at(2).asInt(), i64{40});
    }
    { // content — 알파가 있는 곳 전부(빨강 + 파랑)
        Json j = req("select");
        j.set("mode", Json::string("content"));
        const Json r = s->execute(j);
        CHECK(r["ok"].asBool());
        CHECK_EQ(r["result"]["selectedPixels"].asInt(), i64{3200});
    }
    { // alpha — 불투명 픽셀이므로 content 와 같은 넓이가 나온다
        Json j = req("select");
        j.set("mode", Json::string("alpha"));
        const Json r = s->execute(j);
        CHECK(r["ok"].asBool());
        CHECK_EQ(r["result"]["selectedPixels"].asInt(), i64{3200});
    }
    { // 모르는 방식은 조용히 rect 로 떨어지지 않는다. 거절한다.
        Json j = req("select");
        j.set("mode", Json::string("magic-wand"));
        const Json r = s->execute(j);
        CHECK(!r["ok"].asBool());
    }
}

// ── 불린 결합 ───────────────────────────────────────────────────────────

MARI_TEST(boolean_combination_through_the_api) {
    std::unique_ptr<AgentSession> s = sessionWith(mari_ctx);
    if (s == nullptr) {
        return;
    }
    const auto sel = [&](i32 x, i32 y, i32 w, i32 h, const char* combine) {
        Json j = req("select");
        j.set("region", rect(x, y, w, h));
        if (combine != nullptr) {
            j.set("combine", Json::string(combine));
        }
        return s->execute(j);
    };

    CHECK(sel(0, 0, 100, 100, nullptr)["ok"].asBool());
    const Json added = sel(50, 50, 100, 100, "add");
    CHECK(added["ok"].asBool());
    CHECK_EQ(added["result"]["selectedPixels"].asInt(), i64{17500});
    CHECK_EQ(added["result"]["combine"].asString(), std::string("add"));

    CHECK(sel(0, 0, 100, 100, nullptr)["ok"].asBool());
    CHECK_EQ(sel(50, 50, 100, 100, "intersect")["result"]["selectedPixels"].asInt(), i64{2500});

    CHECK(sel(0, 0, 100, 100, nullptr)["ok"].asBool());
    CHECK_EQ(sel(50, 50, 100, 100, "subtract")["result"]["selectedPixels"].asInt(), i64{7500});

    CHECK(sel(0, 0, 100, 100, nullptr)["ok"].asBool());
    CHECK_EQ(sel(50, 50, 100, 100, "xor")["result"]["selectedPixels"].asInt(), i64{15000});

    // 모르는 결합 방식은 거절한다.
    CHECK(!sel(0, 0, 10, 10, "그런건없다")["ok"].asBool());
}

// ── 🔴 선택 밖에는 그려지지 않는다 ──────────────────────────────────────

MARI_TEST(nothing_is_drawn_outside_the_selection) {
    std::unique_ptr<AgentSession> s = sessionWith(mari_ctx);
    if (s == nullptr) {
        return;
    }
    // 왼쪽 절반만 선택한다.
    Json sel = req("select");
    sel.set("region", rect(0, 0, 128, 256));
    CHECK(s->execute(sel)["ok"].asBool());

    // ① fill — 캔버스 전체를 칠하라고 해도 선택 안쪽만 바뀐다.
    Json f = req("fill");
    f.set("region", Json::string("canvas"));
    f.set("color", Json::string("#ff0000"));
    CHECK(s->execute(f)["ok"].asBool());
    CHECK_EQ(pixelAt(*s, 64, 64).r, u8{255});   // 선택 안
    CHECK_EQ(pixelAt(*s, 200, 64).a, u8{0});    // 🔴 선택 밖 — 손대지 않았다
    CHECK_EQ(pixelAt(*s, 127, 64).r, u8{255});  // 경계 안쪽 마지막 픽셀
    CHECK_EQ(pixelAt(*s, 128, 64).a, u8{0});    // 반열림 — 경계 바로 밖

    // ② gradient — 같은 규약
    Json g = req("gradient");
    g.set("region", Json::string("canvas"));
    g.set("from", Json::string("#00ff00"));
    g.set("to", Json::string("#00ff00"));
    CHECK(s->execute(g)["ok"].asBool());
    CHECK_EQ(pixelAt(*s, 64, 64).g, u8{255});
    CHECK_EQ(pixelAt(*s, 200, 64).a, u8{0});

    // ③ erase — 선택 안쪽만 지워진다
    Json e = req("erase");
    e.set("region", Json::string("canvas"));
    CHECK(s->execute(e)["ok"].asBool());
    CHECK_EQ(pixelAt(*s, 64, 64).a, u8{0});

    // ④ stroke — 선택 경계를 가로지르는 획을 긋는다
    CHECK(s->execute(clearSelection())["ok"].asBool());
    CHECK(s->execute(req("erase"))["ok"].asBool()); // 캔버스를 비우고 시작
    Json sel2 = req("select");
    sel2.set("region", rect(0, 0, 128, 256));
    CHECK(s->execute(sel2)["ok"].asBool());

    Json st = req("stroke");
    Json pts = Json::array();
    for (i32 x = 20; x <= 230; x += 10) {
        Json p = Json::object();
        p.set("x", Json::integer(x));
        p.set("y", Json::integer(128));
        p.set("p", Json::number(1.0));
        pts.push(std::move(p));
    }
    st.set("points", std::move(pts));
    st.set("color", Json::string("#000000"));
    st.set("size", Json::number(12.0));
    const Json sr = s->execute(st);
    CHECK(sr["ok"].asBool());
    CHECK_EQ(pixelAt(*s, 60, 128).a, u8{255}); // 선택 안쪽에는 잉크가 있다
    // 🔴 선택 밖에는 한 픽셀도 없다. 획이 그 위를 지나갔는데도.
    for (i32 x = 140; x <= 230; x += 10) {
        CHECK_EQ(pixelAt(*s, x, 128).a, u8{0});
    }
}

MARI_TEST(non_rectangular_selection_actually_clips_the_stroke) {
    std::unique_ptr<AgentSession> s = sessionWith(mari_ctx);
    if (s == nullptr) {
        return;
    }
    // 원형 선택. 사각형이었다면 모서리에도 잉크가 묻었을 자리를 확인한다.
    Json sel = req("select");
    sel.set("mode", Json::string("ellipse"));
    sel.set("region", rect(28, 28, 200, 200));
    sel.set("antialias", Json::boolean(false));
    CHECK(s->execute(sel)["ok"].asBool());

    Json f = req("fill");
    f.set("region", Json::string("canvas"));
    f.set("color", Json::string("#ff0000"));
    CHECK(s->execute(f)["ok"].asBool());

    CHECK_EQ(pixelAt(*s, 128, 128).a, u8{255}); // 원 한가운데
    CHECK_EQ(pixelAt(*s, 30, 30).a, u8{0});     // 🔴 사각형 모서리 — 원 밖이라 비어 있다
    CHECK_EQ(pixelAt(*s, 226, 226).a, u8{0});
}

// ── 반전 · 페더 · 팽창 ──────────────────────────────────────────────────

MARI_TEST(invert_feather_expand_actually_run) {
    std::unique_ptr<AgentSession> s = sessionWith(mari_ctx);
    if (s == nullptr) {
        return;
    }
    Json sel = req("select");
    sel.set("region", rect(64, 64, 128, 128));
    CHECK(s->execute(sel)["ok"].asBool());

    const Json inv = s->execute(req("select.invert"));
    CHECK(inv["ok"].asBool()); // 예전에는 Unsupported 였다
    CHECK_EQ(inv["result"]["selectedPixels"].asInt(), i64{256} * 256 - 128 * 128);

    CHECK(s->execute(req("select.invert"))["ok"].asBool()); // 다시 뒤집어 제자리로

    Json ex = req("select.expand");
    ex.set("by", Json::integer(10));
    const Json grown = s->execute(ex);
    CHECK(grown["ok"].asBool());
    // 모서리가 둥글어지므로 (128+20)² 보다 작다 — 사각형 팽창이 아니라는 증거다.
    const i64 n = grown["result"]["selectedPixels"].asInt();
    CHECK(n > i64{128} * 128);
    CHECK(n < i64{148} * 148);

    Json sh = req("select.expand");
    sh.set("by", Json::integer(-10));
    CHECK(s->execute(sh)["ok"].asBool());

    Json fe = req("select.feather");
    fe.set("radius", Json::number(6.0));
    const Json fr = s->execute(fe);
    CHECK(fr["ok"].asBool()); // 예전에는 Unsupported 였다
    CHECK_EQ(fr["result"]["kind"].asString(), std::string("mask"));

    // 페더한 선택으로 칠하면 경계가 **반투명**하게 나온다. 이게 페더가 실제로 걸렸다는 뜻이다.
    Json f = req("fill");
    f.set("region", Json::string("canvas"));
    f.set("color", Json::string("#000000"));
    CHECK(s->execute(f)["ok"].asBool());
    const Rect b = Rect{64, 64, 128, 128};
    CHECK_EQ(pixelAt(*s, 128, 128).a, u8{255}); // 한가운데는 꽉 찼다
    const u8 edge = pixelAt(*s, b.x, 128).a;
    CHECK(edge > 0);
    CHECK(edge < 255); // 🔴 경계가 반투명하다

    // 잘못된 값은 거절한다 — 조용히 0으로 만들지 않는다.
    Json bad = req("select.feather");
    bad.set("radius", Json::number(-1.0));
    CHECK(!s->execute(bad)["ok"].asBool());
}

// ── 스냅샷을 타고 살아 돌아온다 ─────────────────────────────────────────

MARI_TEST(selection_survives_snapshot_and_restore) {
    std::unique_ptr<AgentSession> s = sessionWith(mari_ctx);
    if (s == nullptr) {
        return;
    }
    Json sel = req("select");
    sel.set("mode", Json::string("ellipse"));
    sel.set("region", rect(0, 0, 100, 100));
    const Json made = s->execute(sel);
    CHECK(made["ok"].asBool());
    const i64 want = made["result"]["selectedPixels"].asInt();

    const Json snap = s->execute(req("snapshot"));
    CHECK(snap["ok"].asBool());
    const i64 id = snap["result"]["snapshot"]["id"].asInt();

    // 선택을 완전히 다른 것으로 바꾼다.
    CHECK(s->execute(clearSelection())["ok"].asBool());
    const Json mid = s->execute(req("doc.describe"));
    CHECK_EQ(mid["result"]["selectionKind"].asString(), std::string("all"));

    Json restore = req("restore");
    restore.set("snapshot", Json::integer(id));
    CHECK(s->execute(restore)["ok"].asBool());
    // 🔴 사각형이 아닌 선택이 모양 그대로 돌아온다(경계 상자만 돌아오는 게 아니다).
    const Json after = s->execute(req("doc.describe"));
    CHECK_EQ(after["result"]["selectedPixels"].asInt(), want);
    CHECK_EQ(after["result"]["selectionKind"].asString(), std::string("mask"));
}

MARI_TEST_MAIN()
