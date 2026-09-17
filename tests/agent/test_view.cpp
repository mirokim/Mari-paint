// Mari Paint — 🔴 agent_can_see (docs/05 6절 표)
//
//   "획 후 응답에 더티 영역 이미지가 실제로 들어있다"
//
// **실제로** 들어있는지를 본다: base64 를 풀고 PNG 를 디코드해서 **픽셀을 확인한다.**
// 응답에 "image" 키가 있는 것만 보는 테스트는 눈 없는 AI 를 못 막는다.
#include <mari/agent/session.hpp>
#include <mari/agent/view.hpp>
#include <mari/ora/image.hpp>
#include <mari/test/harness.hpp>

#include <string>

using namespace mari;
using mari::agent::AgentSession;
using mari::agent::Json;

namespace {

Json req(const char* op) {
    Json j = Json::object();
    j.set("op", Json::string(op));
    return j;
}

/// 점 몇 개짜리 붉은 획 하나.
Json redStroke(const char* view) {
    Json j = req("stroke");
    Json pts = Json::array();
    for (int i = 0; i < 5; ++i) {
        Json p = Json::object();
        p.set("x", Json::integer(40 + i * 10));
        p.set("y", Json::integer(60));
        p.set("p", Json::number(1.0));
        pts.push(std::move(p));
    }
    j.set("points", std::move(pts));
    j.set("color", Json::string("#FF0000"));
    j.set("size", Json::number(12.0));
    if (view != nullptr) {
        j.set("view", Json::string(view));
    }
    return j;
}

std::unique_ptr<AgentSession> newSession(mari::test::Context& mari_ctx, i32 w = 256, i32 h = 256) {
    auto s = AgentSession::open("claude/opus-5");
    CHECK(s.ok());
    if (!s.ok()) {
        return nullptr;
    }
    std::unique_ptr<AgentSession> session = std::move(s).value();
    Json create = req("doc.create");
    create.set("width", Json::integer(w));
    create.set("height", Json::integer(h));
    const Json r = session->execute(create);
    CHECK(r["ok"].asBool());
    return session;
}

/// 응답에 실린 이미지를 진짜 픽셀로 되돌린다.
Result<ora::Image8> decodeImage(const Json& image) {
    if (!image.isObject() || !image["png"].isString()) {
        return Err("응답에 이미지가 없다", ErrorCode::NotFound);
    }
    Result<std::vector<u8>> bytes = agent::base64Decode(image["png"].asString());
    if (!bytes.ok()) {
        return bytes.error();
    }
    return ora::decodePng(bytes.value().data(), bytes.value().size());
}

} // namespace

// ── 🔴 핵심 ──────────────────────────────────────────────────────────────

MARI_TEST(agent_can_see) {
    std::unique_ptr<AgentSession> s = newSession(mari_ctx);
    CHECK(s != nullptr);
    if (s == nullptr) {
        return;
    }

    // view 를 주지 않았다 → 기본값 dirty. **보여 주는 게 기본이어야 한다.**
    const Json r = s->execute(redStroke(nullptr));
    CHECK(r["ok"].asBool());
    CHECK(r.has("dirtyRect"));
    CHECK(r.has("image"));

    const Json& img = r["image"];
    CHECK_EQ(img["format"].asString(), std::string("png"));
    CHECK_EQ(img["encoding"].asString(), std::string("base64"));
    CHECK(img["bytes"].asInt() > 0);

    // 이미지 영역이 더티 영역과 같다(= 바뀐 부분만 보내고 있다).
    CHECK_EQ(img["x"].asInt(), r["dirtyRect"].at(0).asInt());
    CHECK_EQ(img["y"].asInt(), r["dirtyRect"].at(1).asInt());
    CHECK_EQ(img["srcW"].asInt(), r["dirtyRect"].at(2).asInt());
    CHECK_EQ(img["srcH"].asInt(), r["dirtyRect"].at(3).asInt());
    // 더티 영역은 캔버스보다 **작아야** 한다. 전체를 보내면 증분이 아니다.
    CHECK(img["srcW"].asInt() < 256);
    CHECK(img["srcH"].asInt() < 256);

    // 🔴 여기가 진짜다 — PNG 를 풀어서 붉은 픽셀이 실제로 있는지 본다.
    const Result<ora::Image8> decoded = decodeImage(img);
    CHECK(decoded.ok());
    if (!decoded.ok()) {
        return;
    }
    CHECK_EQ(decoded.value().width, static_cast<i32>(img["w"].asInt()));
    CHECK_EQ(decoded.value().height, static_cast<i32>(img["h"].asInt()));

    usize red = 0;
    usize opaque = 0;
    for (usize i = 0; i + 3 < decoded.value().pixels.size(); i += 4) {
        const u8 a = decoded.value().pixels[i + 3];
        if (a == 0) {
            continue;
        }
        ++opaque;
        if (decoded.value().pixels[i] > 200 && decoded.value().pixels[i + 1] < 80 &&
            decoded.value().pixels[i + 2] < 80) {
            ++red;
        }
    }
    CHECK(opaque > 0);
    CHECK(red > 0);
    std::printf("      [agent_can_see] 더티 %lldx%lld px, PNG %lld 바이트, 불투명 %zu px(붉은 %zu)\n",
                static_cast<long long>(img["srcW"].asInt()),
                static_cast<long long>(img["srcH"].asInt()),
                static_cast<long long>(img["bytes"].asInt()), opaque, red);

    // 획이 지나간 자리의 픽셀을 좌표로 직접 짚어 본다.
    const i32 ox = static_cast<i32>(img["x"].asInt());
    const i32 oy = static_cast<i32>(img["y"].asInt());
    const i32 px = 60 - ox;
    const i32 py = 60 - oy;
    CHECK(px >= 0 && px < decoded.value().width);
    CHECK(py >= 0 && py < decoded.value().height);
    if (px >= 0 && px < decoded.value().width && py >= 0 && py < decoded.value().height) {
        const usize idx =
            (static_cast<usize>(py) * static_cast<usize>(decoded.value().width) +
             static_cast<usize>(px)) * 4u;
        CHECK(decoded.value().pixels[idx + 3] > 0); // 획이 지나간 자리는 비어 있지 않다
        CHECK(decoded.value().pixels[idx] > 200);   // 그리고 붉다
    }
}

MARI_TEST(view_none_is_silent) {
    std::unique_ptr<AgentSession> s = newSession(mari_ctx);
    CHECK(s != nullptr);
    if (s == nullptr) {
        return;
    }
    const Json r = s->execute(redStroke("none"));
    CHECK(r["ok"].asBool());
    CHECK(!r.has("image")); // 빠른 경로 — 이미지를 만들지 않는다
    CHECK(r.has("dirtyRect"));
}

MARI_TEST(view_modes_cover_the_table) {
    // docs/05 2.1 표의 다섯 모드가 전부 실제로 동작한다.
    std::unique_ptr<AgentSession> s = newSession(mari_ctx);
    CHECK(s != nullptr);
    if (s == nullptr) {
        return;
    }
    CHECK(s->execute(redStroke("none"))["ok"].asBool());

    const Json full = s->execute([] {
        Json j = req("render");
        j.set("view", Json::string("full"));
        return j;
    }());
    CHECK(full["ok"].asBool());
    CHECK_EQ(full["image"]["srcW"].asInt(), 256);
    CHECK_EQ(full["image"]["srcH"].asInt(), 256);

    const Json region = s->execute([] {
        Json j = req("render");
        j.set("view", Json::string("region:[0,0,64,32]"));
        return j;
    }());
    CHECK(region["ok"].asBool());
    CHECK_EQ(region["image"]["srcW"].asInt(), 64);
    CHECK_EQ(region["image"]["srcH"].asInt(), 32);

    // layer:<id> — 합성이 아니라 그 레이어의 픽셀만.
    const Json list = s->execute(req("layer.list"));
    CHECK(list["ok"].asBool());
    const i64 id = list["result"]["layers"].at(0)["id"].asInt();
    const Json layerView = s->execute([id] {
        Json j = req("render");
        j.set("view", Json::string("layer:" + std::to_string(id)));
        return j;
    }());
    CHECK(layerView["ok"].asBool());
    CHECK(layerView.has("image"));
    CHECK_EQ(layerView["image"]["mode"].asString(), std::string("layer"));

    // 모르는 모드는 **거절**한다. 조용히 기본값으로 떨어지면 AI 가 오타를 못 찾는다.
    const Json bad = s->execute([] {
        Json j = req("render");
        j.set("view", Json::string("layers"));
        return j;
    }());
    CHECK(!bad["ok"].asBool());
    CHECK_EQ(bad["error"]["code"].asString(), std::string("InvalidArgument"));

    const Json badKey = s->execute([] {
        Json j = req("render");
        Json v = Json::object();
        v.set("mode", Json::string("full"));
        v.set("maxSize", Json::integer(64)); // 오타
        j.set("view", std::move(v));
        return j;
    }());
    CHECK(!badKey["ok"].asBool());
}

MARI_TEST(view_max_downscales_and_reports_scale) {
    std::unique_ptr<AgentSession> s = newSession(mari_ctx, 512, 512);
    CHECK(s != nullptr);
    if (s == nullptr) {
        return;
    }
    Json j = req("render");
    Json v = Json::object();
    v.set("mode", Json::string("full"));
    v.set("max", Json::integer(64));
    j.set("view", std::move(v));
    const Json r = s->execute(j);
    CHECK(r["ok"].asBool());
    CHECK_EQ(r["image"]["w"].asInt(), 64);
    CHECK_EQ(r["image"]["srcW"].asInt(), 512);
    CHECK(r["image"]["scale"].asNumber() > 0.12 && r["image"]["scale"].asNumber() < 0.13);

    const Result<ora::Image8> decoded = decodeImage(r["image"]);
    CHECK(decoded.ok());
    if (decoded.ok()) {
        CHECK_EQ(decoded.value().width, 64); // 진짜로 줄어든 PNG 가 실려 왔다
    }
}

MARI_TEST(dirty_view_is_empty_when_nothing_changed) {
    // 안 바뀌었으면 그림을 보내지 않는다. "변화 없음"도 정보다.
    std::unique_ptr<AgentSession> s = newSession(mari_ctx);
    CHECK(s != nullptr);
    if (s == nullptr) {
        return;
    }
    Json j = req("layer.list");
    j.set("view", Json::string("dirty"));
    const Json r = s->execute(j);
    CHECK(r["ok"].asBool());
    CHECK(!r.has("image"));
    CHECK(!r.has("dirtyRect"));
}

MARI_TEST(base64_round_trips) {
    // 이미지 전송의 밑바닥. 여기가 틀리면 위의 검사가 전부 무의미해진다.
    for (usize n = 0; n < 200; ++n) {
        std::vector<u8> src(n);
        for (usize i = 0; i < n; ++i) {
            src[i] = static_cast<u8>((i * 37u + n * 11u) & 0xFFu);
        }
        const std::string enc = agent::base64Encode(src.data(), src.size());
        CHECK_EQ(enc.size() % 4u, static_cast<usize>(0));
        const Result<std::vector<u8>> dec = agent::base64Decode(enc);
        CHECK(dec.ok());
        if (dec.ok()) {
            CHECK(dec.value() == src);
        }
    }
    CHECK(!agent::base64Decode("!!!!").ok());
}

MARI_TEST_MAIN()
