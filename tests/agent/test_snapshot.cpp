// Mari Paint — 🔴 snapshot_is_cheap (docs/05 6절 표)
//
//   "4096² 캔버스 스냅샷 100개 < 50MB, 각 < 1ms"
//
// **실제로 재서 출력한다.** 숫자를 눈으로 볼 수 없으면 "O(1) 이다"는 주장일 뿐이다.
// 메모리는 /proc/self/statm 의 RSS 로, 시간은 steady_clock 으로 잰다.
//
// 이게 통과한다는 것의 의미(docs/05 2.2):
//   AI 의 작업 방식 자체가 **시도 → 평가 → 되돌리기**다. 그게 싸지 않으면
//   에이전트는 실험을 못 한다. 포토샵 스크립팅은 이걸 하려면 파일을 복사해야 한다.
#include <mari/agent/session.hpp>
#include <mari/agent/snapshot.hpp>
#include <mari/crypto/canvas_hash.hpp>
#include <mari/test/harness.hpp>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace mari;
using mari::agent::AgentSession;
using mari::agent::Json;

namespace {

Json req(const char* op) {
    Json j = Json::object();
    j.set("op", Json::string(op));
    j.set("view", Json::string("none")); // 측정에 PNG 인코딩을 섞지 않는다
    return j;
}

/// 현재 RSS(바이트). 못 읽으면 0.
usize rssBytes() {
    std::FILE* f = std::fopen("/proc/self/statm", "r");
    if (f == nullptr) {
        return 0;
    }
    unsigned long long total = 0;
    unsigned long long resident = 0;
    const int n = std::fscanf(f, "%llu %llu", &total, &resident);
    std::fclose(f);
    if (n != 2) {
        return 0;
    }
    return static_cast<usize>(resident) * 4096u; // 리눅스 x86-64 페이지 크기
}

f64 nowMs() {
    return std::chrono::duration<f64, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::unique_ptr<AgentSession> sessionWith(mari::test::Context& mari_ctx, i32 w, i32 h) {
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

} // namespace

// ── 🔴 핵심 ──────────────────────────────────────────────────────────────

MARI_TEST(snapshot_is_cheap) {
    std::unique_ptr<AgentSession> s = sessionWith(mari_ctx, 4096, 4096);
    CHECK(s != nullptr);
    if (s == nullptr) {
        return;
    }

    // 빈 캔버스로 재면 아무것도 증명하지 못한다. 실제로 픽셀을 깐다.
    // 2048² = 타일 1024장(64×64). 픽셀로는 16MB 다 — 스냅샷 100개가 이걸 복사하면 1.6GB 다.
    Json fill = req("fill");
    Json region = Json::array();
    region.push(Json::integer(0));
    region.push(Json::integer(0));
    region.push(Json::integer(2048));
    region.push(Json::integer(2048));
    fill.set("region", std::move(region));
    fill.set("color", Json::string("#3366CC"));
    const Json filled = s->execute(fill);
    CHECK(filled["ok"].asBool());

    const Json before = s->execute(req("layer.list"));
    CHECK(before["ok"].asBool());
    const i64 tiles = before["result"]["layers"].at(0)["tiles"].asInt();
    CHECK(tiles >= 1024);

    const usize rss0 = rssBytes();
    CHECK(rss0 > 0); // /proc 을 못 읽으면 이 테스트는 의미가 없다

    constexpr int kCount = 100;
    f64 worstMs = 0.0;
    f64 totalMs = 0.0;
    for (int i = 0; i < kCount; ++i) {
        Json take = req("snapshot");
        take.set("label", Json::string("시도 " + std::to_string(i)));
        const f64 t0 = nowMs();
        const Json r = s->execute(take);
        const f64 dt = nowMs() - t0;
        CHECK(r["ok"].asBool());
        // 픽셀을 공유한다고 **응답에도 적혀 있다.**
        CHECK(r["result"]["snapshot"]["sharesPixels"].asBool());
        totalMs += dt;
        worstMs = dt > worstMs ? dt : worstMs;
    }
    const usize rss1 = rssBytes();
    const usize grew = rss1 > rss0 ? rss1 - rss0 : 0;

    std::printf("      [snapshot_is_cheap] 4096x4096, 타일 %lld장, 스냅샷 %d개\n"
                "        메모리 증가: %.2f MB (상한 50MB)\n"
                "        스냅샷 1개: 평균 %.3f ms, 최악 %.3f ms (상한 1ms)\n",
                static_cast<long long>(tiles), kCount,
                static_cast<double>(grew) / (1024.0 * 1024.0),
                totalMs / static_cast<f64>(kCount), worstMs);

    CHECK_EQ(s->snapshots().size(), static_cast<usize>(kCount));
    // 🔴 판정 — docs/05 6절 표 그대로.
    CHECK(grew < 50u * 1024u * 1024u);
    CHECK(worstMs < 1.0);

    // 픽셀을 복사했다면 100개 × 16MB 가 나왔어야 한다. 실제로는 그 근처도 안 간다.
    CHECK(grew < 100u * 1024u * 1024u);
}

MARI_TEST(restore_is_o1_and_exact) {
    std::unique_ptr<AgentSession> s = sessionWith(mari_ctx, 512, 512);
    CHECK(s != nullptr);
    if (s == nullptr) {
        return;
    }
    auto hash = [&]() {
        const Result<std::string> h = s->document()->canvasHash();
        CHECK(h.ok());
        return h.ok() ? h.value() : std::string{};
    };

    Json fill = req("fill");
    fill.set("region", Json::string("canvas"));
    fill.set("color", Json::string("#202020"));
    CHECK(s->execute(fill)["ok"].asBool());
    const std::string base = hash();

    Json snap = req("snapshot");
    snap.set("label", Json::string("러프 완성"));
    const Json taken = s->execute(snap);
    CHECK(taken["ok"].asBool());

    // 세 가지 시도를 하고 매번 되돌린다 — docs/05 2.2 의 그 순환.
    for (int trial = 0; trial < 3; ++trial) {
        Json stroke = req("stroke");
        Json pts = Json::array();
        for (int i = 0; i < 6; ++i) {
            Json p = Json::object();
            p.set("x", Json::integer(50 + trial * 30 + i * 20));
            p.set("y", Json::integer(100 + i * 12));
            pts.push(std::move(p));
        }
        stroke.set("points", std::move(pts));
        stroke.set("color", Json::string("#FF8800"));
        const Json drew = s->execute(stroke);
        CHECK(drew["ok"].asBool());
        CHECK_NE(hash(), base); // 정말 바뀌었다

        Json back = req("restore");
        back.set("snapshot", Json::string("러프 완성"));
        const f64 t0 = nowMs();
        const Json restored = s->execute(back);
        const f64 dt = nowMs() - t0;
        CHECK(restored["ok"].asBool());
        // 🔴 픽셀 단위로 정확히 되돌아왔다.
        CHECK_EQ(hash(), base);
        CHECK(dt < 5.0);
        // 되돌린 것도 "바뀐 영역"이다 — 그래야 화면이 갱신된다.
        CHECK(restored["result"]["diff"]["changedTiles"].asInt() > 0);
    }
}

MARI_TEST(restore_brings_back_removed_layers_with_same_id) {
    // 🔴 id 가 살아 돌아와야 한다. 새로 만들면 에이전트가 들고 있던 주소가 전부 무효가 된다.
    std::unique_ptr<AgentSession> s = sessionWith(mari_ctx, 128, 128);
    CHECK(s != nullptr);
    if (s == nullptr) {
        return;
    }
    Json add = req("layer.add");
    add.set("name", Json::string("선화"));
    const Json added = s->execute(add);
    CHECK(added["ok"].asBool());
    const i64 id = added["result"]["layer"]["id"].asInt();

    Json fill = req("fill");
    fill.set("layer", Json::string("선화"));
    fill.set("region", Json::string("canvas"));
    fill.set("color", Json::string("#00FF00"));
    CHECK(s->execute(fill)["ok"].asBool());
    const Result<std::string> before = s->document()->canvasHash();
    CHECK(before.ok());

    Json snap = req("snapshot");
    snap.set("label", Json::string("선화 있음"));
    CHECK(s->execute(snap)["ok"].asBool());

    Json remove = req("layer.remove");
    remove.set("layer", Json::string("선화"));
    CHECK(s->execute(remove)["ok"].asBool());
    CHECK(s->document()->layers().find(static_cast<LayerId>(id)) == nullptr);

    Json back = req("restore");
    back.set("snapshot", Json::string("선화 있음"));
    CHECK(s->execute(back)["ok"].asBool());

    const LayerPtr again = s->document()->layers().find(static_cast<LayerId>(id));
    CHECK(again != nullptr);
    if (again) {
        CHECK_EQ(again->name(), std::string("선화"));
    }
    const Result<std::string> after = s->document()->canvasHash();
    CHECK(after.ok());
    if (before.ok() && after.ok()) {
        CHECK_EQ(after.value(), before.value()); // 픽셀까지 그대로다
    }
}

MARI_TEST(branch_and_diff_report_reality) {
    std::unique_ptr<AgentSession> s = sessionWith(mari_ctx, 256, 256);
    CHECK(s != nullptr);
    if (s == nullptr) {
        return;
    }
    Json snap = req("snapshot");
    snap.set("label", Json::string("바탕"));
    const Json base = s->execute(snap);
    CHECK(base["ok"].asBool());
    const i64 baseId = base["result"]["snapshot"]["id"].asInt();

    // 아무것도 안 했으면 diff 는 "같다" 여야 한다.
    Json same = req("diff");
    same.set("from", Json::integer(baseId));
    const Json sameR = s->execute(same);
    CHECK(sameR["ok"].asBool());
    CHECK(sameR["result"]["diff"]["identical"].asBool());
    CHECK_EQ(sameR["result"]["diff"]["changedTiles"].asInt(), 0);

    Json fill = req("fill");
    Json region = Json::array();
    region.push(Json::integer(10));
    region.push(Json::integer(10));
    region.push(Json::integer(40));
    region.push(Json::integer(40));
    fill.set("region", std::move(region));
    fill.set("color", Json::string("#FFFFFF"));
    CHECK(s->execute(fill)["ok"].asBool());

    const Json diffR = s->execute(same);
    CHECK(diffR["ok"].asBool());
    CHECK(!diffR["result"]["diff"]["identical"].asBool());
    CHECK(diffR["result"]["diff"]["changedTiles"].asInt() > 0);
    // 바뀐 영역은 칠한 자리를 덮는 타일 경계다.
    const Json& area = diffR["result"]["diff"]["area"];
    CHECK(area.at(0).asInt() <= 10);
    CHECK(area.at(0).asInt() + area.at(2).asInt() >= 50);

    // 분기: 바탕으로 되돌아가 새 갈래를 연다.
    Json branch = req("branch");
    branch.set("snapshot", Json::integer(baseId));
    branch.set("label", Json::string("갈래 B"));
    const Json branched = s->execute(branch);
    CHECK(branched["ok"].asBool());
    CHECK_EQ(branched["result"]["snapshot"]["branchedFrom"].asInt(), baseId);

    // 되돌아왔으니 바탕과 같아야 한다.
    const Json afterBranch = s->execute(same);
    CHECK(afterBranch["ok"].asBool());
    CHECK(afterBranch["result"]["diff"]["identical"].asBool());

    // compare 는 차이 영역의 그림까지 실어 준다(지금은 차이가 없으니 그림도 없다).
    Json cmp = Json::object();
    cmp.set("op", Json::string("compare"));
    cmp.set("snapshot", Json::integer(baseId));
    const Json cmpR = s->execute(cmp);
    CHECK(cmpR["ok"].asBool());
    CHECK(cmpR["result"]["identical"].asBool());
    CHECK(!cmpR.has("image"));
}

MARI_TEST(snapshot_store_evicts_oldest_and_says_so) {
    // 무한정 쌓이면 그게 누수다. 버린 사실을 숨기지 않는다.
    agent::SnapshotStore store(4);
    for (int i = 0; i < 10; ++i) {
        agent::DocSnapshot s;
        s.label = std::to_string(i);
        (void)store.put(std::move(s));
    }
    CHECK_EQ(store.size(), static_cast<usize>(4));
    CHECK_EQ(store.evicted(), static_cast<u64>(6));
    CHECK(store.findByLabel("0") == nullptr);
    CHECK(store.findByLabel("9") != nullptr);
}

MARI_TEST_MAIN()
