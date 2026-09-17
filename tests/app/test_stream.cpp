// 🔴 진짜 스트리밍 — 푸시 경로와 역압 (docs/05 2.7 · docs/07)
//
// 이 파일이 증명하려는 것 넷:
//   ① 구독자는 **밀어받는다.** 아무도 큐를 꺼내 달라고 부르지 않는데 콜백이 불린다.
//   ② 느린 구독자가 **그리는 쪽을 막지 못한다.**
//   ③ 느린 구독자가 넘치면 **그 구독자만** 요약·절단된다. 옆 구독자는 멀쩡하다.
//   ④ 🔴 그 드롭이 **Sigan 기록 경로에는 닿지 않는다**(docs/03 4.2 드롭 금지).
//      ④ 가 깨지면 인증서가 설명하지 못하는 획이 생긴다. 이 파일에서 제일 중요한 케이스다.
#include <mari/app/events.hpp>
#include <mari/sigan/publisher.hpp>
#include <mari/test/harness.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace mari;
using namespace mari::app;

namespace {

std::string tmpJournal(const char* stem) {
    const auto p =
        std::filesystem::temp_directory_path() / (std::string("mari_stream_") + stem + ".jrnl");
    std::error_code ec;
    std::filesystem::remove(p, ec);
    return p.string();
}

/// 그리기 쪽 흉내. 이벤트를 n 건 쏜다.
void fireMany(EventHub& hub, int n) {
    for (int i = 0; i < n; ++i) {
        hub.fireLayerChanged(static_cast<LayerId>(1), "pixels");
    }
}

/// 🔴 기록 경로에 붙는 리스너. 여기서 이벤트가 **한 건도 사라지면 안 된다.**
/// 실제 Sigan 배선이 할 일(프레임 발행)을 그대로 한다.
class RecordingToSigan final : public IAppEventListener {
public:
    explicit RecordingToSigan(sigan::SiganPublisher& pub) : pub_(pub) {}

    void onEvent(const AppEvent& ev) override {
        ++seen;
        lastSeq = ev.seq;
        // 사람 획 하나를 발행한다. 출처는 팩토리가 박는다 — 여기서 고를 수 없다.
        sigan::StrokeSample s(StrokeSource::humanPen());
        s.pos = PointF{static_cast<f32>(ev.seq), 0.0f};
        s.layerId = ev.layerId;
        s.flags = sigan::frameFlags(sigan::FrameFlag::Down);
        (void)pub_.publish(s);
        pub_.endStroke();
    }

    u64 seen = 0;
    u64 lastSeq = 0;

private:
    sigan::SiganPublisher& pub_;
};

} // namespace

// ── ① 푸시다. 폴링이 아니다 ──────────────────────────────────────────────

MARI_TEST(callback_subscriber_is_pushed_not_polled) {
    EventHub hub;
    std::atomic<int> got{0};
    std::atomic<u64> lastSeq{0};

    // 🔴 이 테스트에는 poll·drain 에 해당하는 호출이 **한 줄도 없다.**
    //    그래도 콜백이 불린다면 그것이 곧 서버 푸시다.
    const SubscriptionId id = hub.subscribe([&](const AppEvent& ev) {
        lastSeq.store(ev.seq);
        got.fetch_add(1);
    });
    CHECK(id != 0);
    CHECK_EQ(hub.subscriberCount(), static_cast<usize>(1));

    hub.fireDocumentOpened("/tmp/a.ora", "deadbeef", 640, 480);
    hub.firePaste(PasteSource::Clipboard);
    hub.fireProgress("8bf 필터", 0.5, false);
    hub.fireProgress("8bf 필터", 1.0, true);

    CHECK(hub.waitForSubscribers(2000));
    CHECK_EQ(got.load(), 4);
    CHECK_EQ(lastSeq.load(), static_cast<u64>(4)); // seq 는 버스 전역 단조다
    CHECK_EQ(hub.firedCount(), static_cast<u64>(4));

    CHECK(hub.unsubscribe(id));
    CHECK_EQ(hub.subscriberCount(), static_cast<usize>(0));
    hub.firePaste(PasteSource::File);
    CHECK_EQ(got.load(), 4); // 뗀 뒤에는 오지 않는다
}

MARI_TEST(progress_travels_the_same_bus) {
    // docs/05 2.7 — 긴 작업의 진행률도 **같은 버스**로 흐른다. 두 번 만들지 않는다.
    EventHub hub;
    std::atomic<int> progress{0};
    std::atomic<int> other{0};
    const SubscriptionId id = hub.subscribe([&](const AppEvent& ev) {
        if (ev.kind == AppEventKind::Progress) {
            progress.fetch_add(1);
        } else {
            other.fetch_add(1);
        }
    });
    hub.fireProgress("큰 저장", 2.0, false); // 범위 밖은 잘린다
    hub.fireUndo(1);
    CHECK(hub.waitForSubscribers(2000));
    CHECK_EQ(progress.load(), 1);
    CHECK_EQ(other.load(), 1);
    CHECK(hub.unsubscribe(id));
}

// ── ② 느린 구독자가 그리기를 막지 못한다 ────────────────────────────────

MARI_TEST(slow_subscriber_does_not_block_the_drawing_thread) {
    EventHub hub;
    std::atomic<int> got{0};
    // 콜백 하나가 10ms 다. 200건이면 순진하게 동기 호출하면 2초 걸린다.
    SubscribeOptions opt;
    opt.capacity = 4;
    const SubscriptionId id = hub.subscribe(
        [&](const AppEvent&) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            got.fetch_add(1);
        },
        opt);

    const auto t0 = std::chrono::steady_clock::now();
    fireMany(hub, 200);
    const auto elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();

    // 🔴 그리는 쪽은 기다리지 않았다. 동기였다면 2000ms 는 썼어야 한다.
    if (elapsedMs >= 500) {
        mari_ctx.fail(__FILE__, __LINE__,
                      "느린 구독자가 그리기 스레드를 막았다: " + std::to_string(elapsedMs) + "ms");
    }
    CHECK(hub.firedCount() == 200);
    CHECK(hub.unsubscribe(id)); // 밀린 큐째로 떼어도 매달리지 않는다
}

// ── ③ 넘치면 그 구독자만 ────────────────────────────────────────────────

MARI_TEST(overflow_touches_only_the_slow_subscriber) {
    EventHub hub;
    std::atomic<int> fast{0};
    std::atomic<int> slow{0};

    SubscribeOptions slowOpt;
    slowOpt.capacity = 4;
    slowOpt.onOverflow = OverflowPolicy::Summarize;
    const SubscriptionId slowId = hub.subscribe(
        [&](const AppEvent&) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            slow.fetch_add(1);
        },
        slowOpt);
    // 빠른 구독자에게는 넉넉한 큐를 준다 — 이 테스트가 보려는 것은 "느린 쪽이 넘칠 때
    // 빠른 쪽이 멀쩡한가"이지, 빠른 쪽도 같이 넘치는 상황이 아니다.
    SubscribeOptions fastOpt;
    fastOpt.capacity = 4096;
    const SubscriptionId fastId =
        hub.subscribe([&](const AppEvent&) { fast.fetch_add(1); }, fastOpt);

    fireMany(hub, 300);
    CHECK(hub.waitForSubscribers(5000));

    // 빠른 구독자는 **전부** 받았다. 남의 느림이 내 이벤트를 지우지 못한다.
    CHECK_EQ(fast.load(), 300);
    const SubscriberStats ss = hub.subscriberStats(slowId);
    CHECK(ss.alive);
    CHECK(ss.summarized > 0);                  // 느린 쪽은 요약당했다
    CHECK(ss.delivered + ss.summarized == 300); // 🔴 받은 것 + 버린 것 = 전부. 사라진 칸이 없다
    CHECK_EQ(hub.subscriberStats(fastId).summarized, static_cast<u64>(0));

    CHECK(hub.unsubscribe(slowId));
    CHECK(hub.unsubscribe(fastId));
}

MARI_TEST(detach_policy_cuts_only_that_subscriber) {
    EventHub hub;
    std::atomic<int> fast{0};
    SubscribeOptions opt;
    opt.capacity = 2;
    opt.onOverflow = OverflowPolicy::Detach; // 요약 말고 끊는다
    const SubscriptionId slowId = hub.subscribe(
        [](const AppEvent&) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); }, opt);
    SubscribeOptions fastOpt;
    fastOpt.capacity = 4096; // 빠른 쪽은 넘칠 일이 없어야 비교가 성립한다
    const SubscriptionId fastId =
        hub.subscribe([&](const AppEvent&) { fast.fetch_add(1); }, fastOpt);

    fireMany(hub, 200);
    CHECK(hub.waitForSubscribers(5000));

    CHECK(hub.subscriberStats(slowId).detached); // 끊긴 사실을 숨기지 않는다
    CHECK_EQ(fast.load(), 200);                  // 옆 구독자는 한 건도 안 잃었다
    CHECK(hub.unsubscribe(slowId));
    CHECK(hub.unsubscribe(fastId));
}

// ── ④ 🔴 드롭은 Sigan 기록에 닿지 않는다 ────────────────────────────────

MARI_TEST(slow_subscriber_drop_never_touches_the_sigan_record) {
    // docs/03 4.2 — 네이티브 경로는 샘플링이 아니라 **정본**이다. 드롭 금지.
    // 관찰 경로가 아무리 막혀도 기록 경로는 한 건도 잃으면 안 된다.
    const std::string path = tmpJournal("sigan_untouched");
    auto pub = sigan::SiganPublisher::open(sigan::PublisherConfig{path, "mari-paint/0.1.0", 1});
    CHECK(pub.ok());
    if (!pub.ok()) {
        return;
    }
    sigan::SiganPublisher& publisher = *pub.value();

    EventHub hub;
    RecordingToSigan recorder(publisher);
    hub.addListener(&recorder); // ① 기록 경로 — 큐가 없다

    SubscribeOptions opt;
    opt.capacity = 2;
    const SubscriptionId slowId = hub.subscribe( // ② 관찰 경로 — 일부러 막는다
        [](const AppEvent&) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }, opt);

    constexpr int kEvents = 300;
    fireMany(hub, kEvents);

    // 🔴 기록 경로: 동기 호출이라 fire 가 돌아온 시점에 이미 전부 발행돼 있다.
    CHECK_EQ(recorder.seen, static_cast<u64>(kEvents));
    CHECK_EQ(recorder.lastSeq, static_cast<u64>(kEvents));
    CHECK_EQ(publisher.stats().published, static_cast<u64>(kEvents));
    CHECK_EQ(publisher.lastSeq(), static_cast<u64>(kEvents));
    // 발행기의 약속도 그대로다 — 버린 칸이 없다(published == sent + spooled).
    CHECK_EQ(publisher.stats().published,
             publisher.stats().sent + publisher.stats().spooled);
    CHECK_EQ(publisher.stats().origins.human(), static_cast<u64>(kEvents));
    CHECK_EQ(publisher.stats().origins.agent(), static_cast<u64>(0));

    CHECK(hub.waitForSubscribers(5000));
    // 관찰 경로는 버렸다 — 그리고 그 사실을 센다.
    const SubscriberStats ss = hub.subscriberStats(slowId);
    CHECK(ss.summarized > 0);
    CHECK(ss.delivered < static_cast<u64>(kEvents));

    CHECK(hub.unsubscribe(slowId));
    CHECK(hub.removeListener(&recorder));
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

MARI_TEST(recording_listener_has_no_queue_to_overflow) {
    // 🔴 구조 검사. 기록 경로에는 **용량 인자가 없다** — 넘칠 큐가 없으니
    //    "드롭 정책"이라는 것이 존재할 수 없다. 두 정책이 섞이지 않았다는 증거다.
    EventHub hub;
    struct Counter final : IAppEventListener {
        void onEvent(const AppEvent&) override { ++n; }
        u64 n = 0;
    } c;
    hub.addListener(&c);
    fireMany(hub, 10000); // 용량을 넘길 수가 없다
    CHECK_EQ(c.n, static_cast<u64>(10000));
    CHECK(hub.removeListener(&c));
}

MARI_TEST_MAIN()
