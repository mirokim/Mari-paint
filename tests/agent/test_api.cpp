// Mari Paint — 에이전트 API 표면 (docs/05 4·6절)
//
// 여기서 지키는 것:
//   · `agent_origin_forced`   — 이 API 로 들어온 획의 origin 은 **무조건** Agent 다
//   · `no_origin_override`    — 연산 표 어디에도 origin 파라미터가 없다
//   · `batch_atomic`          — 중간 실패 시 캔버스가 원상복구된다
//   · `capabilities` 정확성   — 표 == 디스패치 == MCP 도구 목록의 원본
//   · `describe` 정확성       — docs/05 2.4 의 그 문장 모양이 실제로 나온다
#include <mari/agent/capabilities.hpp>
#include <mari/agent/brush_library.hpp>
#include <mari/agent/session.hpp>
#include <mari/crypto/canvas_hash.hpp>
#include <mari/crypto/sha256.hpp>
#include <mari/test/harness.hpp>

#include <filesystem>
#include <string>
#include <vector>

using namespace mari;
using mari::agent::AgentSession;
using mari::agent::Json;
using mari::agent::presetToJson;
using mari::agent::presetFromJson;

namespace {

Json req(const char* op) {
    Json j = Json::object();
    j.set("op", Json::string(op));
    j.set("view", Json::string("none"));
    return j;
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

Json strokeAt(i32 x, i32 y, const char* color = "#000000") {
    Json j = req("stroke");
    Json pts = Json::array();
    for (int i = 0; i < 4; ++i) {
        Json p = Json::object();
        p.set("x", Json::integer(x + i * 8));
        p.set("y", Json::integer(y));
        pts.push(std::move(p));
    }
    j.set("points", std::move(pts));
    j.set("color", Json::string(color));
    return j;
}

bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

} // namespace

// ── 🔴 출처 ──────────────────────────────────────────────────────────────

MARI_TEST(agent_origin_forced) {
    std::unique_ptr<AgentSession> s = sessionWith(mari_ctx);
    CHECK(s != nullptr);
    if (s == nullptr) {
        return;
    }
    // 이 세션이 내놓는 출처는 인자 없이 게이트에서 나온다. 언제나 Agent 다.
    CHECK(s->strokeSource().origin() == StrokeOrigin::Agent);
    CHECK(s->strokeSource().isAgent());
    CHECK(!isHumanOrigin(s->strokeSource().origin()));

    const Json r = s->execute(strokeAt(20, 20));
    CHECK(r["ok"].asBool());
    // 응답은 출처를 **보고한다**(고르는 게 아니라 알려 주는 것이다).
    CHECK_EQ(r["result"]["origin"].asString(), std::string("agent"));
    CHECK_EQ(r["result"]["agentId"].asString(), std::string("claude/opus-5"));

    // 집계에도 AI 획으로만 쌓인다. 사람 획이 될 길이 없다.
    CHECK_EQ(s->originStats().agent(), static_cast<u64>(1));
    CHECK_EQ(s->originStats().human(), static_cast<u64>(0));
    CHECK_EQ(s->originStats().unspecified(), static_cast<u64>(0));

    // 픽셀 연산도 마찬가지다 — 붓을 안 거쳤다고 출처가 사라지지 않는다.
    Json fill = req("fill");
    fill.set("region", Json::string("canvas"));
    const Json f = s->execute(fill);
    CHECK(f["ok"].asBool());
    CHECK_EQ(f["result"]["origin"].asString(), std::string("agent"));
    // 🔴 그러나 **붓질로 세지는 않는다**(docs/06 결정 ② · 6절 H3).
    //    캔버스 전체를 칠한 fill 을 붓질 한 번과 같은 칸에 세면 그 순간 단위가 거짓이 된다.
    //    출처는 그대로 Agent 고, 칸만 다르다.
    CHECK_EQ(s->originStats().agent(), static_cast<u64>(1));   // 축 A — 붓질은 그대로 1
    CHECK_EQ(s->regionOpStats().agent(), static_cast<u64>(1)); // 축 B — 영역 연산 1
    CHECK_EQ(s->regionOpStats().human(), static_cast<u64>(0));
    CHECK(s->changedTiles() > 0);                              // 축 C — 실제로 칠해졌다
}

MARI_TEST(anonymous_agent_gets_no_session) {
    // 🔴 익명 AI 획은 만들지 않는다. 세션 자체가 안 열린다.
    CHECK(!AgentSession::open("").ok());
    CHECK(!AgentSession::open("나쁜 아이디").ok());
    CHECK(!AgentSession::open(std::string(40, 'x')).ok());
    CHECK(AgentSession::open("mari.agent_1:v2+x/y-z").ok());
}

MARI_TEST(no_origin_override) {
    // 🔴 스키마 어디에도 origin 을 고르는 파라미터가 없다(docs/05 6절 표).
    //    표가 곧 스키마라서, 표에 없으면 **존재할 수 없다**(모르는 키는 거절된다).
    usize params = 0;
    for (const agent::OpSpec& op : agent::opTable()) {
        for (const agent::ParamSpec& p : op.params) {
            ++params;
            const std::string n = p.name;
            CHECK(n != "origin");
            CHECK(n != "agentId");
            CHECK(n != "asHuman");
            CHECK(n != "strokeOrigin");
            CHECK(n != "source");
        }
    }
    CHECK(params > 30); // 표가 실제로 돌았는지(빈 통과 방지)

    std::unique_ptr<AgentSession> s = sessionWith(mari_ctx);
    CHECK(s != nullptr);
    if (s == nullptr) {
        return;
    }
    // 실제로 넣어 보면 거절당한다.
    Json forged = strokeAt(10, 10);
    forged.set("origin", Json::string("humanPen"));
    const Json r = s->execute(forged);
    CHECK(!r["ok"].asBool());
    CHECK_EQ(r["error"]["code"].asString(), std::string("InvalidArgument"));
    CHECK(contains(r["error"]["message"].asString(), "origin"));

    Json forged2 = strokeAt(10, 10);
    forged2.set("agentId", Json::string("사람인척"));
    CHECK(!s->execute(forged2)["ok"].asBool());

    // capabilities 는 무엇으로 고정돼 있는지 **밝히기만** 한다.
    const Json caps = s->execute(req("capabilities"));
    CHECK(caps["ok"].asBool());
    CHECK_EQ(caps["result"]["origin"]["forced"].asString(), std::string("agent"));
    CHECK(!caps["result"]["origin"]["settable"].asBool());
}

// ── capabilities (docs/05 2.6) ───────────────────────────────────────────

MARI_TEST(capabilities_matches_dispatch) {
    std::unique_ptr<AgentSession> s = sessionWith(mari_ctx);
    CHECK(s != nullptr);
    if (s == nullptr) {
        return;
    }
    const Json caps = s->execute(req("capabilities"));
    CHECK(caps["ok"].asBool());
    const Json& r = caps["result"];
    CHECK_EQ(r["schema"].asString(), std::string(agent::kApiSchema));

    // 표 == 응답. MCP 도구 목록은 이 배열에서 그대로 생성된다.
    CHECK_EQ(r["ops"].size(), agent::opTable().size());
    std::vector<std::string> seen;
    for (usize i = 0; i < r["ops"].size(); ++i) {
        const Json& op = r["ops"].at(i);
        const std::string name = op["name"].asString();
        // 이름이 겹치면 MCP 도구가 덮어씌워진다.
        for (const std::string& other : seen) {
            CHECK_NE(other, name);
        }
        seen.push_back(name);

        // 응답에 있는 연산은 전부 실제로 디스패치된다.
        const agent::OpSpec* spec = agent::findOp(name);
        CHECK(spec != nullptr);
        if (spec != nullptr) {
            CHECK(spec->fn != nullptr);
            CHECK_EQ(op["supported"].asBool(), spec->supported);
            CHECK_EQ(op["params"].size(), spec->params.size() + 1u); // +1 = 공통 view
            if (!op["supported"].asBool()) {
                // 못 하는 것은 **이유를 적는다.** 되는 척하지 않는다.
                CHECK(!op["unsupportedReason"].asString().empty());
            }
        }
        CHECK(!op["summary"].asString().empty());
        CHECK(!op["group"].asString().empty());
        // 🔴 모든 연산이 시각 피드백을 받는다(docs/05 2.1).
        bool hasView = false;
        for (usize k = 0; k < op["params"].size(); ++k) {
            hasView = hasView || op["params"].at(k)["name"].asString() == "view";
        }
        CHECK(hasView);
    }

    // docs/05 4절 표의 아홉 영역이 전부 있다.
    for (const char* group : {"doc", "snapshot", "layer", "draw", "select", "brush", "visual",
                              "batch", "event"}) {
        bool found = false;
        for (const agent::OpSpec& op : agent::opTable()) {
            found = found || std::string(op.group) == group;
        }
        CHECK(found);
    }
    // docs/05 4절 표의 연산 이름이 실제로 있다(이름을 바꿨으면 여기서 걸린다).
    for (const char* name : {"doc.create", "doc.open", "doc.save", "doc.close", "doc.describe",
                             "capabilities", "snapshot", "restore", "branch", "diff",
                             "layer.list", "layer.add", "layer.remove", "layer.move",
                             "layer.duplicate", "layer.merge", "layer.setProps", "layer.flatten", "layer.mask",
                             "layer.group", "layer.ungroup", "stroke", "fill", "bucket",
                             "erase", "gradient", "transform", "adjust", "canvas", "select", "select.invert",
                             "select.expand", "select.feather", "brush.list", "brush.import",
                             "brush.set", "brush.describe", "brush.save", "brush.remove", "brush.export", "render", "thumbnail", "compare",
                             "batch", "events.subscribe", "events.unsubscribe"}) {
        CHECK(agent::findOp(name) != nullptr);
    }

    // 런타임 발견: 브러시 목록과 한계가 실려 있다.
    CHECK(r["brushes"].size() >= 4);
    CHECK(r["limits"]["tileSize"].asInt() == kTileSize);
    CHECK(r["limits"]["maxBatchOps"].asInt() > 0);
    CHECK(r["blendModes"].size() >= 19);
    CHECK(r["pressureProfiles"].size() == 5);
    CHECK(r["headless"].asBool());
}

MARI_TEST(unknown_op_and_unknown_param_are_refused) {
    std::unique_ptr<AgentSession> s = sessionWith(mari_ctx);
    CHECK(s != nullptr);
    if (s == nullptr) {
        return;
    }
    const Json bad = s->execute(req("drawLine")); // docs/05 2.3 이 거절한 그 API
    CHECK(!bad["ok"].asBool());
    CHECK_EQ(bad["error"]["code"].asString(), std::string("NotFound"));

    Json typo = req("layer.add");
    typo.set("nmae", Json::string("오타"));
    const Json r = s->execute(typo);
    CHECK(!r["ok"].asBool());
    CHECK(contains(r["error"]["message"].asString(), "nmae"));

    Json missing = req("doc.create");
    missing.set("width", Json::integer(10));
    const Json m = s->execute(missing);
    CHECK(!m["ok"].asBool());
    CHECK(contains(m["error"]["message"].asString(), "height"));

    // 지원하지 않는 연산은 **이유와 함께** 거절한다.
    //
    // 🔴 이 검사는 표를 훑는다. 예전에는 `select.invert` 를 이름으로 박아 뒀지만,
    //    선택 마스크가 들어오면서 그 연산이 **실제로 지원된다.** 이름을 그대로 두면
    //    "미지원을 거절한다"가 아니라 "select.invert 는 미지원이다"를 검사하게 된다 —
    //    사실이 아닌 것을 테스트가 붙잡고 있는 꼴이다.
    //    지금 이 빌드에는 미지원 연산이 **하나도 없다**(그래서 이 루프는 0바퀴 돈다).
    //    그 사실 자체는 아래 `unsupportedOps` 수로 드러내고 숨기지 않는다.
    usize unsupportedOps = 0;
    for (const agent::OpSpec& op : agent::opTable()) {
        if (op.supported) {
            continue;
        }
        ++unsupportedOps;
        const Json unsup = s->execute(req(op.name));
        CHECK(!unsup["ok"].asBool());
        CHECK_EQ(unsup["error"]["code"].asString(), std::string("Unsupported"));
        // 이유가 비어 있으면 "안 된다"만 말하고 왜인지는 안 말하는 것이다.
        CHECK(std::string(op.unsupportedReason).size() > 0);
    }
    std::printf("  [표] 미지원 연산 %zu개 / 전체 %zu개\n", unsupportedOps,
                agent::opTable().size());

    // 문자열 입구도 같은 규약을 지킨다. 깨진 JSON 도 **응답**이지 예외가 아니다.
    const std::string text = s->executeText("{ not json");
    const Result<Json> parsed = Json::parse(text);
    CHECK(parsed.ok());
    if (parsed.ok()) {
        CHECK(!parsed.value()["ok"].asBool());
        CHECK_EQ(parsed.value()["error"]["code"].asString(), std::string("ParseError"));
    }
}

// ── 🔴 batch_atomic (docs/05 2.5) ────────────────────────────────────────

MARI_TEST(batch_atomic) {
    std::unique_ptr<AgentSession> s = sessionWith(mari_ctx);
    CHECK(s != nullptr);
    if (s == nullptr) {
        return;
    }
    auto hash = [&]() {
        const Result<std::string> h = s->document()->canvasHash();
        return h.ok() ? h.value() : std::string("<실패>");
    };
    CHECK(s->execute(strokeAt(30, 30))["ok"].asBool());
    const std::string before = hash();
    const usize layersBefore = s->execute(req("layer.list"))["result"]["layers"].size();

    // 성공 묶음: 전부 적용된다.
    Json good = req("batch");
    Json ops = Json::array();
    ops.push(strokeAt(40, 60, "#FF0000"));
    ops.push(strokeAt(40, 80, "#00FF00"));
    good.set("ops", std::move(ops));
    const Json goodR = s->execute(good);
    CHECK(goodR["ok"].asBool());
    CHECK_EQ(goodR["result"]["completed"].asInt(), 2);
    CHECK(!goodR["result"]["rolledBack"].asBool());
    const std::string afterGood = hash();
    CHECK_NE(afterGood, before);

    // 🔴 실패 묶음: 앞의 것들이 이미 그려졌어도 **전부 되돌린다.**
    Json bad = req("batch");
    Json badOps = Json::array();
    badOps.push(strokeAt(100, 100, "#0000FF"));
    Json addLayer = req("layer.add");
    addLayer.set("name", Json::string("망가질 레이어"));
    badOps.push(std::move(addLayer));
    badOps.push(strokeAt(120, 120, "#0000FF"));
    Json boom = req("stroke");
    boom.set("layer", Json::string("없는 레이어")); // 여기서 터진다
    Json pts = Json::array();
    Json p = Json::object();
    p.set("x", Json::integer(5));
    p.set("y", Json::integer(5));
    pts.push(std::move(p));
    boom.set("points", std::move(pts));
    badOps.push(std::move(boom));
    badOps.push(strokeAt(140, 140, "#0000FF"));
    bad.set("ops", std::move(badOps));

    const Json badR = s->execute(bad);
    CHECK(!badR["ok"].asBool()); // 원자적 묶음의 실패는 연산 전체의 실패다
    CHECK(contains(badR["error"]["message"].asString(), "롤백"));

    // 캔버스가 묶음 **직전** 상태 그대로다 — 반쯤 망가지지 않았다.
    CHECK_EQ(hash(), afterGood);
    // 만들어졌던 레이어도 사라졌다.
    const Json list = s->execute(req("layer.list"));
    CHECK_EQ(list["result"]["layers"].size(), layersBefore);
    for (usize i = 0; i < list["result"]["layers"].size(); ++i) {
        CHECK_NE(list["result"]["layers"].at(i)["name"].asString(),
                 std::string("망가질 레이어"));
    }

    // atomic:false 면 되돌리지 않는다(그걸 고른 사람 책임이다. 결과에 적어 준다).
    Json loose = req("batch");
    loose.set("atomic", Json::boolean(false));
    Json looseOps = Json::array();
    looseOps.push(strokeAt(160, 160, "#FFFF00"));
    Json boom2 = req("stroke");
    boom2.set("layer", Json::string("없는 레이어"));
    Json pts2 = Json::array();
    Json p2 = Json::object();
    p2.set("x", Json::integer(5));
    p2.set("y", Json::integer(5));
    pts2.push(std::move(p2));
    boom2.set("points", std::move(pts2));
    looseOps.push(std::move(boom2));
    loose.set("ops", std::move(looseOps));
    const Json looseR = s->execute(loose);
    CHECK(looseR["ok"].asBool());
    CHECK(!looseR["result"]["rolledBack"].asBool());
    CHECK_EQ(looseR["result"]["completed"].asInt(), 1);
    CHECK(looseR["result"].has("failure"));
    CHECK_NE(hash(), afterGood); // 첫 획은 남아 있다

    // 중첩 batch 는 롤백 경계가 겹쳐서 막는다.
    Json nested = req("batch");
    Json inner = Json::array();
    Json innerBatch = req("batch");
    innerBatch.set("ops", Json::array());
    inner.push(std::move(innerBatch));
    nested.set("ops", std::move(inner));
    CHECK(!s->execute(nested)["ok"].asBool());
}

MARI_TEST(batch_renders_once) {
    // docs/05 2.5 — "결과는 한 번만 렌더". 왕복도 한 번, 그림도 한 장이다.
    std::unique_ptr<AgentSession> s = sessionWith(mari_ctx);
    CHECK(s != nullptr);
    if (s == nullptr) {
        return;
    }
    Json b = Json::object();
    b.set("op", Json::string("batch"));
    Json ops = Json::array();
    ops.push(strokeAt(20, 20, "#FF0000"));
    ops.push(strokeAt(20, 200, "#FF0000"));
    b.set("ops", std::move(ops));
    b.set("view", Json::string("dirty"));
    const Json r = s->execute(b);
    CHECK(r["ok"].asBool());
    CHECK(r.has("image"));
    // 더티 영역은 두 획을 **합친** 하나다.
    CHECK(r["dirtyRect"].at(3).asInt() > 150);
    CHECK_EQ(r["result"]["results"].size(), static_cast<usize>(2));
}

// ── describe (docs/05 2.4) ───────────────────────────────────────────────

MARI_TEST(describe_reads_like_a_sentence) {
    std::unique_ptr<AgentSession> s = sessionWith(mari_ctx, 1024, 1024);
    CHECK(s != nullptr);
    if (s == nullptr) {
        return;
    }
    // docs/05 2.4 의 예시와 같은 모양을 만든다:
    //   "1024x1024, 레이어 4개: 배경(채워짐), 러프(12% 채워짐), 선화(비어있음), 채색(비어있음)"
    Json rename = req("layer.setProps");
    rename.set("name", Json::string("배경"));
    CHECK(s->execute(rename)["ok"].asBool());

    Json fill = req("fill");
    fill.set("layer", Json::string("배경"));
    fill.set("region", Json::string("canvas"));
    fill.set("color", Json::string("#FFFFFF"));
    CHECK(s->execute(fill)["ok"].asBool());

    for (const char* name : {"러프", "선화", "채색"}) {
        Json add = req("layer.add");
        add.set("name", Json::string(name));
        CHECK(s->execute(add)["ok"].asBool());
    }
    // 러프에 캔버스의 약 12% 를 칠한다(1024*1024 의 12% ≈ 354² 짜리 덩어리).
    Json rough = req("fill");
    rough.set("layer", Json::string("러프"));
    Json region = Json::array();
    region.push(Json::integer(0));
    region.push(Json::integer(0));
    region.push(Json::integer(1024));
    region.push(Json::integer(123));
    rough.set("region", std::move(region));
    rough.set("color", Json::string("#888888"));
    CHECK(s->execute(rough)["ok"].asBool());

    const Json d = s->execute(req("doc.describe"));
    CHECK(d["ok"].asBool());
    const std::string text = d["result"]["text"].asString();
    std::printf("      [describe] %s\n", text.c_str());

    CHECK(contains(text, "1024x1024"));
    CHECK(contains(text, "레이어 4개"));
    CHECK(contains(text, "배경(채워짐"));
    CHECK(contains(text, "러프(12% 채워짐"));
    CHECK(contains(text, "선화(비어있음"));
    CHECK(contains(text, "채색(비어있음"));
    // 🔴 사실만. 등급 문자열은 없다(docs/03 2절 경계선).
    CHECK(!contains(text, "human-only"));
    CHECK(!contains(text, "ai-assisted"));
    CHECK(!contains(text, "ai-generated"));
    CHECK(contains(text, "AI 획"));

    // JSON 쪽도 같은 사실을 말한다.
    CHECK_EQ(d["result"]["layerCount"].asInt(), 4);
    CHECK_EQ(d["result"]["layers"].size(), static_cast<usize>(4));
    // 이 문서는 fill 두 번으로 만들어졌다 — **붓질은 0이다.**
    // 그래서 붓질 비율은 0 이고, 영역 연산 칸에 2 가 있다. 둘 다 참이고 둘 다 필요하다.
    CHECK_EQ(d["result"]["origins"]["total"].asInt(), 0);
    CHECK_EQ(d["result"]["origins"]["regionOps"]["agent"].asInt(), 2);
    CHECK_EQ(d["result"]["origins"]["regionOps"]["human"].asInt(), 0);
    CHECK(d["result"]["origins"]["changedTiles"].asInt() > 0);
    CHECK_EQ(d["result"]["origins"]["human"].asInt(), 0);
    const Json& bg = d["result"]["layers"].at(0);
    CHECK_EQ(bg["name"].asString(), std::string("배경"));
    CHECK(bg["coverage"].asNumber() > 0.99);
    // 역할은 이름에서 유추했다고 **출처를 밝힌다.**
    CHECK_EQ(bg["role"].asString(), std::string("background"));
    CHECK_EQ(bg["roleSource"].asString(), std::string("name"));
}

// ── 시맨틱 주소 (docs/05 2.4) ────────────────────────────────────────────

MARI_TEST(semantic_addressing) {
    std::unique_ptr<AgentSession> s = sessionWith(mari_ctx, 256, 256);
    CHECK(s != nullptr);
    if (s == nullptr) {
        return;
    }
    Json add = req("layer.add");
    add.set("name", Json::string("잉크 레이어"));
    add.set("role", Json::string("lineart"));
    const Json added = s->execute(add);
    CHECK(added["ok"].asBool());
    const i64 id = added["result"]["layer"]["id"].asInt();
    CHECK_EQ(added["result"]["layer"]["role"].asString(), std::string("lineart"));
    CHECK_EQ(added["result"]["layer"]["roleSource"].asString(), std::string("tag"));

    // 이름으로
    Json byName = strokeAt(10, 10);
    byName.set("layer", Json::string("잉크 레이어"));
    const Json r1 = s->execute(byName);
    CHECK(r1["ok"].asBool());
    CHECK_EQ(r1["result"]["layer"].asInt(), id);

    // 역할로
    Json byRole = strokeAt(20, 20);
    Json role = Json::object();
    role.set("role", Json::string("lineart"));
    byRole.set("layer", std::move(role));
    const Json r2 = s->execute(byRole);
    CHECK(r2["ok"].asBool());
    CHECK_EQ(r2["result"]["layer"].asInt(), id);

    // 내용이 있는 영역으로
    Json region = Json::object();
    region.set("content", Json::string("nonEmpty"));
    region.set("layer", Json::string("잉크 레이어"));
    Json sel = req("select");
    sel.set("region", std::move(region));
    const Json r3 = s->execute(sel);
    CHECK(r3["ok"].asBool());
    CHECK(r3["result"]["selection"].at(2).asInt() > 0);

    // 🔴 애매하면 **거절한다.** 조용히 하나 고르면 AI 가 엉뚱한 데 그리고도 모른다.
    Json dup = req("layer.add");
    dup.set("name", Json::string("잉크 레이어"));
    CHECK(s->execute(dup)["ok"].asBool());
    const Json r4 = s->execute(byName);
    CHECK(!r4["ok"].asBool());
    CHECK(contains(r4["error"]["message"].asString(), "애매"));
}

// ── 브러시 · 선택 · 이벤트 ───────────────────────────────────────────────

MARI_TEST(brushes_are_discovered_at_runtime) {
    std::unique_ptr<AgentSession> s = sessionWith(mari_ctx);
    CHECK(s != nullptr);
    if (s == nullptr) {
        return;
    }
    const Json list = s->execute(req("brush.list"));
    CHECK(list["ok"].asBool());
    CHECK(list["result"]["brushes"].size() >= 4);
    const std::string first = list["result"]["brushes"].at(0)["name"].asString();

    Json set = req("brush.set");
    set.set("brush", Json::string("에어브러시"));
    const Json setR = s->execute(set);
    CHECK(setR["ok"].asBool());
    CHECK(setR["result"]["brush"]["current"].asBool());

    const Json desc = s->execute(req("brush.describe"));
    CHECK(desc["ok"].asBool());
    CHECK_EQ(desc["result"]["name"].asString(), std::string("에어브러시"));
    CHECK(desc["result"]["tip"]["diameter"].asNumber() > 0.0);
    CHECK(desc["result"].has("dynamicLinks"));

    Json nope = req("brush.set");
    nope.set("brush", Json::string("없는 브러시"));
    CHECK(!s->execute(nope)["ok"].asBool());

    // 없는 파일 임포트는 정직하게 실패한다.
    Json imp = req("brush.import");
    imp.set("path", Json::string("/definitely/not/here.abr"));
    CHECK(!s->execute(imp)["ok"].asBool());
    CHECK_EQ(first, list["result"]["brushes"].at(0)["name"].asString());
}

MARI_TEST(pressure_profiles_change_the_stroke) {
    // docs/05 2.3 — "필압 커브를 AI 가 직접 못 정하면 프리셋도 준다".
    std::unique_ptr<AgentSession> s = sessionWith(mari_ctx, 256, 256);
    CHECK(s != nullptr);
    if (s == nullptr) {
        return;
    }
    auto run = [&](const char* profile) {
        Json j = req("stroke");
        Json pts = Json::array();
        for (int i = 0; i < 12; ++i) {
            Json p = Json::object();
            p.set("x", Json::integer(20 + i * 15));
            p.set("y", Json::integer(120));
            pts.push(std::move(p));
        }
        j.set("points", std::move(pts));
        j.set("size", Json::number(20.0));
        j.set("brush", Json::string("연필"));
        j.set("pressureProfile", Json::string(profile));
        const Json r = s->execute(j);
        CHECK(r["ok"].asBool());
        CHECK_EQ(r["result"]["pressureProfile"].asString(), std::string(profile));
        const Result<std::string> h = s->document()->canvasHash();
        Json undo = Json::object(); // 다음 시도를 위해 지운다
        (void)undo;
        CHECK(s->document()->undo().ok());
        return h.ok() ? h.value() : std::string{};
    };
    const std::string flat = run("flat");
    const std::string taper = run("taper-in-out");
    const std::string pulse = run("pulse");
    // 필압 프리셋이 실제로 픽셀을 바꾼다(이름만 받고 무시하지 않는다).
    CHECK_NE(flat, taper);
    CHECK_NE(taper, pulse);

    Json bad = req("stroke");
    Json pts = Json::array();
    Json p = Json::object();
    p.set("x", Json::integer(5));
    p.set("y", Json::integer(5));
    pts.push(std::move(p));
    bad.set("points", std::move(pts));
    bad.set("pressureProfile", Json::string("타페린"));
    CHECK(!s->execute(bad)["ok"].asBool());
}

MARI_TEST(events_flow_through_the_same_bus) {
    // docs/05 2.7 — Sigan 에 쓰는 것과 **같은 이벤트 버스**를 쓴다. 두 번 만들지 않는다.
    std::unique_ptr<AgentSession> s = sessionWith(mari_ctx);
    CHECK(s != nullptr);
    if (s == nullptr) {
        return;
    }
    // 구독 전에는 아무것도 모으지 않는다(조용한 세션이 기본이다).
    CHECK(s->execute(strokeAt(10, 10))["ok"].asBool());
    CHECK_EQ(s->queuedEventCount(), static_cast<usize>(0));

    CHECK(s->execute(req("events.subscribe"))["ok"].asBool());
    Json fill = req("fill");
    fill.set("region", Json::string("canvas"));
    CHECK(s->execute(fill)["ok"].asBool());

    const Json polled = s->execute(req("events.poll"));
    CHECK(polled["ok"].asBool());
    CHECK(polled["result"]["events"].size() > 0);
    bool sawLayerChanged = false;
    for (usize i = 0; i < polled["result"]["events"].size(); ++i) {
        sawLayerChanged =
            sawLayerChanged || polled["result"]["events"].at(i)["kind"].asString() == "layerChanged";
    }
    CHECK(sawLayerChanged);

    CHECK(s->execute(req("events.unsubscribe"))["ok"].asBool());
    CHECK(s->execute(fill)["ok"].asBool());
    CHECK_EQ(s->queuedEventCount(), static_cast<usize>(0));
}

MARI_TEST(layer_ops_do_what_they_say) {
    std::unique_ptr<AgentSession> s = sessionWith(mari_ctx, 128, 128);
    CHECK(s != nullptr);
    if (s == nullptr) {
        return;
    }
    Json add = req("layer.add");
    add.set("name", Json::string("위"));
    CHECK(s->execute(add)["ok"].asBool());

    Json fillLower = req("fill");
    fillLower.set("layer", Json::string("레이어 1"));
    fillLower.set("region", Json::string("canvas"));
    fillLower.set("color", Json::string("#FF0000"));
    CHECK(s->execute(fillLower)["ok"].asBool());

    Json fillUpper = req("fill");
    fillUpper.set("layer", Json::string("위"));
    Json region = Json::array();
    region.push(Json::integer(0));
    region.push(Json::integer(0));
    region.push(Json::integer(64));
    region.push(Json::integer(64));
    fillUpper.set("region", std::move(region));
    fillUpper.set("color", Json::string("#0000FF"));
    CHECK(s->execute(fillUpper)["ok"].asBool());

    // 복제는 픽셀을 복사하지 않는다.
    Json dup = req("layer.duplicate");
    dup.set("layer", Json::string("위"));
    const Json dupR = s->execute(dup);
    CHECK(dupR["ok"].asBool());
    CHECK(!dupR["result"]["copiedPixels"].asBool());
    CHECK(s->execute([] {
               Json j = req("layer.remove");
               j.set("layer", Json::string("위 복사"));
               return j;
           }())["ok"]
              .asBool());

    // 합치기: 위 레이어가 아래로 구워지고 사라진다.
    Json merge = req("layer.merge");
    merge.set("layer", Json::string("위"));
    const Json mergeR = s->execute(merge);
    CHECK(mergeR["ok"].asBool());
    const Json after = s->execute(req("layer.list"));
    CHECK_EQ(after["result"]["layers"].size(), static_cast<usize>(1));

    // 합쳐진 결과: 왼쪽 위는 파랑, 나머지는 빨강.
    std::vector<u8> px;
    CHECK(s->document()->exportComposite(px).ok());
    auto at = [&](i32 x, i32 y) {
        const usize i = (static_cast<usize>(y) * 128u + static_cast<usize>(x)) * 4u;
        return Color8::rgba(px[i], px[i + 1], px[i + 2], px[i + 3]);
    };
    CHECK(at(10, 10) == Color8::rgba(0, 0, 255, 255));
    CHECK(at(100, 100) == Color8::rgba(255, 0, 0, 255));

    // 잠긴 레이어에는 그리지 않는다.
    Json lock = req("layer.setProps");
    lock.set("locked", Json::boolean(true));
    CHECK(s->execute(lock)["ok"].asBool());
    const Json blocked = s->execute(strokeAt(10, 10));
    CHECK(!blocked["ok"].asBool());
    CHECK(contains(blocked["error"]["message"].asString(), "잠긴"));
}

MARI_TEST(transform_is_honest_about_what_it_cannot_do) {
    std::unique_ptr<AgentSession> s = sessionWith(mari_ctx, 128, 128);
    CHECK(s != nullptr);
    if (s == nullptr) {
        return;
    }
    Json fill = req("fill");
    Json region = Json::array();
    region.push(Json::integer(0));
    region.push(Json::integer(0));
    region.push(Json::integer(32));
    region.push(Json::integer(32));
    fill.set("region", std::move(region));
    fill.set("color", Json::string("#FFFFFF"));
    CHECK(s->execute(fill)["ok"].asBool());

    Json move = req("transform");
    move.set("dx", Json::integer(64));
    move.set("dy", Json::integer(64));
    const Json moved = s->execute(move);
    CHECK(moved["ok"].asBool());

    std::vector<u8> px;
    CHECK(s->document()->exportComposite(px).ok());
    auto alphaAt = [&](i32 x, i32 y) {
        return px[(static_cast<usize>(y) * 128u + static_cast<usize>(x)) * 4u + 3u];
    };
    CHECK_EQ(static_cast<int>(alphaAt(10, 10)), 0);   // 원래 자리는 비었다
    CHECK_EQ(static_cast<int>(alphaAt(80, 80)), 255); // 옮겨간 자리에 있다

    // 확대·회전은 이제 리샘플러(app::transformLayer)가 있다. 2배: 64..96 → 48..112 (중심 80).
    Json scale = req("transform");
    scale.set("scale", Json::number(2.0));
    const Json scaled = s->execute(scale);
    CHECK(scaled["ok"].asBool());
    CHECK(s->document()->exportComposite(px).ok());
    CHECK_EQ(static_cast<int>(alphaAt(50, 50)), 255);
    CHECK_EQ(static_cast<int>(alphaAt(40, 40)), 0);
    // 색 보정과 캔버스 연산도 같은 계층에서.
    Json inv = req("adjust");
    inv.set("kind", Json::string("invert"));
    CHECK(s->execute(inv)["ok"].asBool());
    CHECK(s->document()->exportComposite(px).ok());
    CHECK_EQ(static_cast<int>(px[(80u * 128u + 80u) * 4u]), 0); // 흰색 → 검정
    Json flip = req("canvas");
    flip.set("action", Json::string("rotate"));
    flip.set("degrees", Json::integer(90));
    const Json rot = s->execute(flip);
    CHECK(rot["ok"].asBool());
    CHECK(s->document()->canvasSize() == (Size{128, 128}));
    Json bad = req("canvas");
    bad.set("action", Json::string("melt"));
    CHECK(!s->execute(bad)["ok"].asBool());
}

MARI_TEST(json_round_trips) {
    // 능력 계층의 입출력 표현. 여기가 새면 전부 샌다.
    const char* text =
        R"({"op":"stroke","points":[{"x":1,"y":2.5,"p":0.25}],"s":"따옴표\"와 \\ 와 \n","t":true,
            "n":null,"neg":-17,"exp":1e3,"deep":[[[1]]],"u":"🎨"})";
    const Result<Json> parsed = Json::parse(text);
    CHECK(parsed.ok());
    if (!parsed.ok()) {
        return;
    }
    const Json& j = parsed.value();
    CHECK_EQ(j["op"].asString(), std::string("stroke"));
    CHECK(j["points"].at(0)["x"].isIntegral());
    CHECK(!j["points"].at(0)["y"].isIntegral());
    CHECK_EQ(j["neg"].asInt(), -17);
    CHECK_EQ(j["exp"].asNumber(), 1000.0);
    CHECK(j["t"].asBool());
    CHECK(j["n"].isNull());
    CHECK_EQ(j["u"].asString(), std::string("\xF0\x9F\x8E\xA8")); // 🎨 서로게이트 쌍

    const Result<Json> again = Json::parse(j.dump());
    CHECK(again.ok());
    if (again.ok()) {
        CHECK(again.value() == j); // 왕복해도 같다
    }
    // 키 순서가 유지된다(응답 모양이 매번 같아야 한다).
    CHECK_EQ(j.keys().at(0), std::string("op"));
    CHECK_EQ(j.keys().at(1), std::string("points"));

    CHECK(!Json::parse("{\"a\":1} 쓰레기").ok());
    CHECK(!Json::parse("[1,]").ok());
    CHECK(!Json::parse("").ok());
}

MARI_TEST(save_and_open_round_trip) {
    // docs/05 2.8 — 헤드리스가 기본값이다. 파일 왕복도 같은 코드 경로로 돈다.
    std::unique_ptr<AgentSession> s = sessionWith(mari_ctx, 96, 96);
    CHECK(s != nullptr);
    if (s == nullptr) {
        return;
    }
    Json fill = req("fill");
    fill.set("region", Json::string("canvas"));
    fill.set("color", Json::string("#123456"));
    CHECK(s->execute(fill)["ok"].asBool());
    CHECK(s->execute(strokeAt(20, 20, "#FFFFFF"))["ok"].asBool());
    const Result<std::string> before = s->document()->canvasHash();
    CHECK(before.ok());

    const std::string path =
        (std::filesystem::temp_directory_path() / "mari_agent_roundtrip.ora").string();
    Json save = req("doc.save");
    save.set("path", Json::string(path));
    const Json saved = s->execute(save);
    CHECK(saved["ok"].asBool());
    CHECK_EQ(saved["result"]["canvasHash"].asString(), before.valueOr(std::string{}));
    // 🔴 두 해시를 이름으로 구분한다. fileHash 는 **파일 바이트 그대로**여서
    //    `sha256sum <파일>` 과 값이 같아야 하고, 합성 픽셀 해시와는 절대 같지 않다.
    const Result<std::string> fileHash = crypto::sha256FileHex(path);
    CHECK(fileHash.ok());
    CHECK_EQ(saved["result"]["fileHash"].asString(), fileHash.valueOr(std::string{}));
    CHECK(saved["result"]["fileHash"].asString().size() == 64);
    CHECK(saved["result"]["fileHash"].asString() != saved["result"]["canvasHash"].asString());

    CHECK(s->execute(req("doc.close"))["ok"].asBool());
    CHECK(s->document() == nullptr);
    // 문서가 없으면 그리기는 **정직하게** 실패한다.
    const Json orphan = s->execute(strokeAt(10, 10));
    CHECK(!orphan["ok"].asBool());
    CHECK_EQ(orphan["error"]["code"].asString(), std::string("NotFound"));

    Json open = req("doc.open");
    open.set("path", Json::string(path));
    const Json opened = s->execute(open);
    CHECK(opened["ok"].asBool());
    const Result<std::string> after = s->document()->canvasHash();
    CHECK(after.ok());
    if (before.ok() && after.ok()) {
        CHECK_EQ(after.value(), before.value()); // 픽셀이 그대로 돌아왔다
    }
    // .png 는 열지 않는다(추측해서 열지 않는다).
    Json wrong = req("doc.open");
    wrong.set("path", Json::string("/tmp/없는것.png"));
    CHECK(!s->execute(wrong)["ok"].asBool());
    std::filesystem::remove(path);
}

MARI_TEST(stroke_undo_is_exact) {
    // 🔴 실행취소 범위는 **칠하기 전에** 잡는다. 범위가 모자라면 실행취소가 거짓말을 한다.
    //    붓 크기·흩뿌림을 넉넉히 감안했는지를 해시로 못박는다.
    std::unique_ptr<AgentSession> s = sessionWith(mari_ctx, 256, 256);
    CHECK(s != nullptr);
    if (s == nullptr) {
        return;
    }
    auto hash = [&]() {
        const Result<std::string> h = s->document()->canvasHash();
        return h.ok() ? h.value() : std::string("<실패>");
    };
    const std::string base = hash();

    Json j = req("stroke");
    Json pts = Json::array();
    for (int i = 0; i < 10; ++i) {
        Json p = Json::object();
        p.set("x", Json::integer(30 + i * 20));
        p.set("y", Json::integer(40 + i * 15));
        pts.push(std::move(p));
    }
    j.set("points", std::move(pts));
    j.set("brush", Json::string("에어브러시")); // 지름이 큰 붓 — 범위를 넓게 먹는다
    j.set("size", Json::number(64.0));
    j.set("color", Json::string("#FF00FF"));
    const Json drew = s->execute(j);
    CHECK(drew["ok"].asBool());
    CHECK(drew["result"]["undoComplete"].asBool());
    CHECK(drew["result"]["stamps"].asInt() > 0);
    CHECK(drew["result"]["changedTiles"].asInt() > 0);
    const std::string painted = hash();
    CHECK_NE(painted, base);

    CHECK(s->document()->undo().ok());
    CHECK_EQ(hash(), base); // 한 픽셀도 남지 않았다
    CHECK(s->document()->redo().ok());
    CHECK_EQ(hash(), painted);
}

// ── 사람이 쓰는 도구를 헤드리스에서도 똑같이 ──────────────────────────────

MARI_TEST(agent_can_use_bucket_wand_mask_group_flatten) {
    std::unique_ptr<AgentSession> s = sessionWith(mari_ctx, 128, 128);
    CHECK(s != nullptr);
    if (s == nullptr) {
        return;
    }
    const auto alphaAt = [&](i32 x, i32 y) -> int {
        std::vector<u8> px(static_cast<usize>(128) * 128u * 4u);
        (void)s->document()->layers().flatten(Rect{0, 0, 128, 128}, px.data(), 128u * 4u);
        return px[(static_cast<usize>(y) * 128u + static_cast<usize>(x)) * 4u + 3u];
    };

    // 사각 선화(선 두께 8)를 fill 로 그린다: 바깥 테두리.
    for (const char* region : {"[20,20,88,8]", "[20,92,88,8]", "[20,20,8,80]", "[100,20,8,80]"}) {
        Json f = req("fill");
        f.set("region", Json::parse(region).value());
        f.set("color", Json::string("#000000"));
        CHECK(s->execute(f)["ok"].asBool());
    }
    // 페인트통: 안쪽을 클릭하면 안쪽만 채워진다.
    Json b = req("bucket");
    b.set("at", Json::parse("[60,60]").value());
    b.set("color", Json::string("#FF0000"));
    const Json bucketed = s->execute(b);
    CHECK(bucketed["ok"].asBool());
    CHECK(bucketed["result"]["changedPixels"].asInt() > 0);
    CHECK_EQ(alphaAt(60, 60), 255);
    CHECK_EQ(alphaAt(5, 5), 0); // 바깥은 그대로 투명

    // 마술봉: 바깥(투명)을 잡으면 안쪽은 선택에 안 들어간다.
    Json w = req("select");
    w.set("mode", Json::string("wand"));
    w.set("at", Json::parse("[5,5]").value());
    const Json wand = s->execute(w);
    CHECK(wand["ok"].asBool());
    CHECK_EQ(s->document()->selectionMask().valueAt(5, 5), 255);
    CHECK_EQ(s->document()->selectionMask().valueAt(60, 60), 0);

    // 마스크: 선택(바깥)만 보이게 → 안쪽 빨강이 가려진다. 지우면 돌아온다.
    Json m = req("layer.mask");
    m.set("action", Json::string("fromSelection"));
    CHECK(s->execute(m)["ok"].asBool());
    CHECK_EQ(alphaAt(60, 60), 0);
    m.set("action", Json::string("remove"));
    CHECK(s->execute(m)["ok"].asBool());
    CHECK_EQ(alphaAt(60, 60), 255);

    // 그룹으로 묶고 풀기.
    const Json grouped = s->execute(req("layer.group"));
    CHECK(grouped["ok"].asBool());
    CHECK_EQ(s->document()->layers().roots().size(), usize{1});
    CHECK(s->document()->layers().roots()[0]->kind() == LayerKind::Group);
    Json ug = req("layer.ungroup");
    ug.set("layer", Json::integer(grouped["result"]["group"]["id"].asInt()));
    CHECK(s->execute(ug)["ok"].asBool());
    CHECK(s->document()->layers().roots()[0]->kind() == LayerKind::Raster);

    // 레이어 하나 더 만들고 평탄화 → 루트 1개, 픽셀은 그대로.
    CHECK(s->execute(req("layer.add"))["ok"].asBool());
    CHECK_EQ(s->document()->layers().roots().size(), usize{2});
    CHECK(s->execute(req("layer.flatten"))["ok"].asBool());
    CHECK_EQ(s->document()->layers().roots().size(), usize{1});
    CHECK_EQ(alphaAt(60, 60), 255);
}

MARI_TEST(agent_stroke_accepts_tilt_and_time) {
    std::unique_ptr<AgentSession> s = sessionWith(mari_ctx, 128, 128);
    CHECK(s != nullptr);
    if (s == nullptr) {
        return;
    }
    Json j = req("stroke");
    Json pts = Json::array();
    for (int i = 0; i < 5; ++i) {
        Json p = Json::object();
        p.set("x", Json::integer(20 + i * 20));
        p.set("y", Json::integer(64));
        p.set("p", Json::number(0.5 + 0.1 * i));
        p.set("tx", Json::number(0.3));
        p.set("ty", Json::number(-0.2));
        p.set("t", Json::number(i * 16.0));
        pts.push(std::move(p));
    }
    j.set("points", std::move(pts));
    j.set("background", Json::string("#00FF00"));
    const Json drew = s->execute(j);
    CHECK(drew["ok"].asBool());
    CHECK(drew["result"]["stamps"].asInt() > 0);
    // 범위 밖 기울기는 거절한다.
    Json bad = req("stroke");
    Json bp = Json::array();
    Json p = Json::object();
    p.set("x", Json::integer(10)); p.set("y", Json::integer(10)); p.set("tx", Json::number(2.0));
    bp.push(std::move(p));
    bad.set("points", std::move(bp));
    CHECK(!s->execute(bad)["ok"].asBool());
}

MARI_TEST(airbrush_holds_at_the_last_point_too) {
    // 두 점짜리 "같은 자리에 3초" 획 — 펜을 뗀 자리에서도 시간만큼 쌓여야 한다.
    std::unique_ptr<AgentSession> s = sessionWith(mari_ctx, 128, 128);
    CHECK(s != nullptr);
    if (s == nullptr) {
        return;
    }
    const auto holdStroke = [&](double holdMs) {
        Json j = req("stroke");
        j.set("brush", Json::string("에어브러시"));
        j.set("size", Json::number(40));
        j.set("opacity", Json::number(0.3));
        Json pts = Json::array();
        for (int i = 0; i < 2; ++i) {
            Json p = Json::object();
            p.set("x", Json::integer(64)); p.set("y", Json::integer(64));
            p.set("t", Json::number(i == 0 ? 0.0 : holdMs));
            pts.push(std::move(p));
        }
        j.set("points", std::move(pts));
        const Json r = s->execute(j);
        CHECK(r["ok"].asBool());
        return r["result"]["stamps"].asInt();
    };
    const i64 quick = holdStroke(1.0);
    CHECK(s->document()->undo().ok());
    const i64 held = holdStroke(3000.0);
    CHECK(held > quick + 50); // 30ms 마다 한 번 → 3초면 100번쯤
}

MARI_TEST(ungroup_moves_active_layer_off_the_removed_group) {
    std::unique_ptr<AgentSession> s = sessionWith(mari_ctx, 64, 64);
    CHECK(s != nullptr);
    if (s == nullptr) {
        return;
    }
    Json add = req("layer.add");
    add.set("name", Json::string("안"));
    const Json inner = s->execute(add);
    CHECK(inner["ok"].asBool());
    Json grp = req("layer.group");
    grp.set("layer", Json::string("안"));
    const Json g = s->execute(grp);
    CHECK(g["ok"].asBool());
    const i64 gid = g["result"]["group"]["id"].asInt();
    Json act = req("layer.setProps");
    act.set("layer", Json::integer(gid));
    act.set("active", Json::boolean(true));
    (void)s->execute(act);
    Json ung = req("layer.ungroup");
    ung.set("layer", Json::integer(gid));
    CHECK(s->execute(ung)["ok"].asBool());
    // 활성 레이어가 살아 있는 레이어여야 layer 없이 그리는 다음 획이 성공한다.
    Json st = req("stroke");
    Json pts = Json::array();
    for (int i = 0; i < 2; ++i) { Json p = Json::object(); p.set("x", Json::integer(10 + i * 20)); p.set("y", Json::integer(20)); pts.push(std::move(p)); }
    st.set("points", std::move(pts));
    CHECK(s->execute(st)["ok"].asBool());
}

// ── 브러시 라이브러리(.mbp) ───────────────────────────────────────────────

MARI_TEST(brush_preset_round_trips_through_mbp_json) {
    brush::MariBrushPreset p;
    p.name = "왕복 붓";
    p.sourceFormat = "native";
    p.tip.kind = brush::TipKind::Bitmap;
    p.tip.bitmap = brush::GrayImage{3, 2, {0, 64, 128, 192, 255, 7}};
    p.tip.diameter = 33.5f;
    p.tip.angle = 12.0f;
    p.tip.aspectRatio = 0.4f;
    p.tip.hardness = 0.7f;
    p.spacing = 0.15f;
    p.opacity = 0.8f;
    p.flow = 0.6f;
    p.blendMode = BlendMode::Multiply;
    brush::DynamicLink l;
    l.input = brush::DynamicInput::TiltX;
    l.output = brush::DynamicOutput::Rotation;
    l.amount = 0.5f;
    l.curve.points = {{0.0f, 0.1f}, {1.0f, 0.9f}};
    p.dynamics.push_back(l);
    p.texture = brush::BrushTexture{};
    p.texture->image = brush::GrayImage{2, 2, {1, 2, 3, 4}};
    p.texture->scale = 2.5f;
    p.texture->depth = 0.3f;
    p.texture->blendMode = BlendMode::Subtract;
    p.texture->anchoredToCanvas = false;
    p.dual = brush::DualBrush{};
    p.dual->tip.diameter = 9.0f;
    p.dual->count = 3;
    p.dual->blendMode = BlendMode::Darken;
    p.colorDynamics.hueJitter = 0.25f;
    p.colorDynamics.perTip = false;
    p.scatterCount = 4;
    p.wetEdges = true;
    p.noise = 0.5f;
    p.extraParams.emplace_back("abr/flip", 1.0f);

    const Json j = presetToJson(p);
    const Result<Json> re = Json::parse(j.dump());
    CHECK(re.ok());
    Result<brush::MariBrushPreset> back = presetFromJson(re.value());
    CHECK(back.ok());
    if (!back.ok()) return;
    const brush::MariBrushPreset& q = back.value();
    CHECK_EQ(q.name, p.name);
    CHECK(q.tip.kind == brush::TipKind::Bitmap);
    CHECK(q.tip.bitmap.pixels == p.tip.bitmap.pixels);
    CHECK_NEAR(q.tip.diameter, 33.5f, 1e-4);
    CHECK_NEAR(q.tip.aspectRatio, 0.4f, 1e-4);
    CHECK(q.blendMode == BlendMode::Multiply);
    CHECK_EQ(q.dynamics.size(), usize{1});
    CHECK(q.dynamics[0].input == brush::DynamicInput::TiltX);
    CHECK(q.dynamics[0].output == brush::DynamicOutput::Rotation);
    CHECK_NEAR(q.dynamics[0].curve.points[1].y, 0.9f, 1e-4);
    CHECK(q.texture.has_value());
    CHECK(q.texture->image.pixels == p.texture->image.pixels);
    CHECK(q.texture->blendMode == BlendMode::Subtract);
    CHECK(!q.texture->anchoredToCanvas);
    CHECK(q.dual.has_value());
    CHECK_EQ(q.dual->count, 3);
    CHECK(q.dual->blendMode == BlendMode::Darken);
    CHECK_NEAR(q.colorDynamics.hueJitter, 0.25f, 1e-4);
    CHECK(!q.colorDynamics.perTip);
    CHECK_EQ(q.scatterCount, 4);
    CHECK(q.wetEdges);
    CHECK_NEAR(q.noise, 0.5f, 1e-4);
    CHECK_EQ(q.extraParams.size(), usize{1});
}

MARI_TEST(brush_save_export_remove_persist_in_user_dir) {
    std::unique_ptr<AgentSession> s = sessionWith(mari_ctx, 64, 64);
    CHECK(s != nullptr);
    if (s == nullptr) return;
    const usize before = s->brushes().size();

    // 현재(내장) 브러시를 다른 이름으로 저장 → 사용자 브러시가 하나 는다.
    Json save = req("brush.save");
    save.set("name", Json::string("내 붓"));
    const Json saved = s->execute(save);
    CHECK(saved["ok"].asBool());
    CHECK_EQ(s->brushes().size(), before + 1);

    // export → 지름을 고쳐 → save(preset) → 같은 이름이라 덮어쓴다(개수 그대로).
    Json ex = req("brush.export");
    ex.set("brush", Json::string("내 붓"));
    const Json exported = s->execute(ex);
    CHECK(exported["ok"].asBool());
    Json preset = exported["result"]["preset"];
    Json tip = preset["tip"];
    tip.set("diameter", Json::number(77.0));
    preset.set("tip", std::move(tip));
    Json save2 = req("brush.save");
    save2.set("preset", std::move(preset));
    CHECK(s->execute(save2)["ok"].asBool());
    CHECK_EQ(s->brushes().size(), before + 1);

    // 새 세션이 같은 폴더를 읽는다 → 77px 로 보인다.
    std::unique_ptr<AgentSession> t = sessionWith(mari_ctx, 64, 64);
    CHECK(t != nullptr);
    if (t == nullptr) return;
    Json d = req("brush.describe");
    d.set("brush", Json::string("내 붓"));
    const Json desc = t->execute(d);
    CHECK(desc["ok"].asBool());
    CHECK_NEAR(desc["result"]["tip"]["diameter"].asNumber(), 77.0, 1e-6);

    // 지우면 파일도 사라져 다음 세션엔 없다. 내장은 못 지운다.
    Json rm = req("brush.remove");
    rm.set("brush", Json::string("내 붓"));
    CHECK(t->execute(rm)["ok"].asBool());
    std::unique_ptr<AgentSession> u = sessionWith(mari_ctx, 64, 64);
    CHECK(u != nullptr && u->brushes().size() == before);
    Json rmBuiltin = req("brush.remove");
    rmBuiltin.set("brush", Json::integer(static_cast<i64>(u->brushes().front().id)));
    CHECK(!u->execute(rmBuiltin)["ok"].asBool());
}

MARI_TEST_MAIN()
