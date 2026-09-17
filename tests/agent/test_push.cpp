// 🔴 세션 푸시 구독 — `events.poll` 없이 이벤트가 온다 (docs/05 2.7 · docs/07)
//
// 폴링(`events.subscribe` → `events.poll`)은 그대로 남아 있다. 여기서 증명하는 것은
// **그것을 한 번도 부르지 않고도** 에이전트가 이벤트를 밀어받는다는 사실이다.
#include <mari/agent/session.hpp>
#include <mari/test/harness.hpp>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace mari;
using namespace mari::agent;

namespace {

Json op(const char* name) {
    Json j = Json::object();
    j.set("op", Json::string(name));
    return j;
}

/// 문서 하나를 열고 세션을 돌려준다.
std::unique_ptr<AgentSession> openDoc(mari::test::Context& ctx) {
    auto s = AgentSession::open("push-test");
    if (!s.ok()) {
        ctx.fail(__FILE__, __LINE__, "세션이 안 열린다: " + s.message());
        return nullptr;
    }
    std::unique_ptr<AgentSession> session = std::move(s).value();
    Json create = op("doc.create");
    create.set("width", Json::integer(64));
    create.set("height", Json::integer(64));
    if (!session->execute(create)["ok"].asBool()) {
        ctx.fail(__FILE__, __LINE__, "doc.create 가 실패했다");
        return nullptr;
    }
    return session;
}

/// 캔버스를 채운다 — layerChanged 를 내는 연산이다.
Json fillAll() {
    Json j = op("fill");
    j.set("region", Json::string("canvas"));
    return j;
}

} // namespace

MARI_TEST(agent_receives_events_without_polling) {
    std::unique_ptr<AgentSession> s = openDoc(mari_ctx);
    if (!s) {
        return;
    }

    std::mutex m;
    std::vector<std::string> kinds;
    std::vector<i64> seqs;
    const u64 id = s->subscribePush({}, [&](const std::string& kind, const Json& data) {
        std::lock_guard<std::mutex> g(m);
        kinds.push_back(kind);
        seqs.push_back(data["seq"].asInt(0));
    });
    CHECK(id != 0);
    CHECK_EQ(s->pushSubscriberCount(), static_cast<usize>(1));

    CHECK(s->execute(fillAll())["ok"].asBool());
    CHECK(s->application().events().waitForSubscribers(5000));

    {
        std::lock_guard<std::mutex> g(m);
        bool sawLayerChanged = false;
        for (const std::string& k : kinds) {
            sawLayerChanged = sawLayerChanged || k == "layerChanged";
        }
        CHECK(sawLayerChanged);
        CHECK(!seqs.empty());
        CHECK(seqs.front() > 0); // 버스 전역 seq 가 실려 온다
    }

    // 🔴 여기까지 오는 동안 `events.poll` 도 `events.subscribe` 도 부르지 않았다.
    //    폴링 큐는 비어 있다 — 두 경로가 정말 독립이라는 뜻이다.
    CHECK_EQ(s->queuedEventCount(), static_cast<usize>(0));
    CHECK(!s->subscribed());

    CHECK(s->unsubscribePush(id));
    CHECK_EQ(s->pushSubscriberCount(), static_cast<usize>(0));
}

MARI_TEST(push_filter_selects_kinds) {
    std::unique_ptr<AgentSession> s = openDoc(mari_ctx);
    if (!s) {
        return;
    }
    std::atomic<int> got{0};
    const u64 id = s->subscribePush({"documentSaved"},
                                    [&](const std::string&, const Json&) { got.fetch_add(1); });
    CHECK(id != 0);
    CHECK(s->execute(fillAll())["ok"].asBool()); // layerChanged — 걸러진다
    CHECK(s->application().events().waitForSubscribers(5000));
    CHECK_EQ(got.load(), 0);
    CHECK(s->unsubscribePush(id));
}

MARI_TEST(slow_push_subscriber_does_not_slow_down_drawing) {
    std::unique_ptr<AgentSession> s = openDoc(mari_ctx);
    if (!s) {
        return;
    }
    const u64 id = s->subscribePush(
        {},
        [](const std::string&, const Json&) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        },
        4); // 아주 작은 큐

    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 60; ++i) {
        CHECK(s->execute(fillAll())["ok"].asBool());
    }
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();
    // 동기였다면 60건 × 20ms = 1200ms 를 넘겼어야 한다. 그리기는 기다리지 않았다.
    if (ms >= 800) {
        mari_ctx.fail(__FILE__, __LINE__,
                      "느린 푸시 구독자가 그리기를 붙잡았다: " + std::to_string(ms) + "ms");
    }
    // 버린 건 버렸다고 말한다. 숨기지 않는다.
    CHECK(s->pushStats(id).alive);
    CHECK(s->unsubscribePush(id));
}

MARI_TEST(push_subscription_dies_with_the_session) {
    // 🔴 세션이 죽을 때 구독자 스레드도 합류된다. 안 그러면 콜백이 죽은 스택을 건드린다.
    std::atomic<int> got{0};
    {
        std::unique_ptr<AgentSession> s = openDoc(mari_ctx);
        if (!s) {
            return;
        }
        const u64 id = s->subscribePush({}, [&](const std::string&, const Json&) {
            got.fetch_add(1);
        });
        CHECK(id != 0);
        CHECK(s->execute(fillAll())["ok"].asBool());
    } // 여기서 소멸자가 전부 뗀다
    CHECK(got.load() >= 1);
}

MARI_TEST_MAIN()
