// docs/03 10절 검증 계획 — `no_frame_drop`, `sigan_restart_survives`, `version_skew`
#include "fake_sink.hpp"

#include <mari/sigan/publisher.hpp>
#include <mari/test/harness.hpp>

#include <filesystem>
#include <string>

using namespace mari;
using namespace mari::sigan;
using mari::sigan::test::FakeSink;

namespace {

std::string tmpPath(const char* stem) {
    auto p = std::filesystem::temp_directory_path() / (std::string("mari_pub_") + stem + ".jrnl");
    return p.string();
}

StrokeSample sample(f32 x, u32 flags) {
    StrokeSample s;
    s.pos = PointF{x, x * 2.0f};
    s.pressure = 0.5f;
    s.layerId = 5;
    s.brushId = 9;
    s.flags = flags;
    return s;
}

/// 획 하나를 그린다. down → move*n → up.
void drawStroke(SiganPublisher& pub, int points, f32 base) {
    pub.publish(sample(base, frameFlags(FrameFlag::Down)));
    for (int i = 1; i < points - 1; ++i) {
        pub.publish(sample(base + static_cast<f32>(i), frameFlags(FrameFlag::Move)));
    }
    pub.publish(sample(base + static_cast<f32>(points), frameFlags(FrameFlag::Up)));
    pub.endStroke();
}

Handshake fullReply() {
    Handshake h;
    h.proto = kProtoVersion;
    h.app = "sigan/0.2.46";
    h.caps = {caps::kCanvasXy, caps::kLayer, caps::kBrush, caps::kView, caps::kResume};
    return h;
}

} // namespace

// 싱크를 인위적으로 막고 대량 발행 → **seq 구멍 0**, 전부 저널에 있다.
MARI_TEST(no_frame_drop) {
    const std::string path = tmpPath("nodrop");
    auto pub = SiganPublisher::open(PublisherConfig{path, "mari-paint/0.1.0", 1});
    CHECK(pub.ok());
    auto& p = *pub.value();

    FakeSink sink;
    sink.reply = fullReply();
    p.setSink(&sink); // 연결·핸드셰이크는 pump() 가 한다
    CHECK(p.pump().ok());

    sink.blocked = true; // 🔴 파이프가 막혔다. 여기서 버리면 증거에 구멍이 난다.
    for (int s = 0; s < 40; ++s) {
        drawStroke(p, 250, static_cast<f32>(s));
    }
    const u64 total = p.lastSeq();
    CHECK_EQ(total, 40ull * 250ull);
    CHECK_EQ(p.stats().published, total);
    CHECK_EQ(p.stats().spooled, total - p.stats().sent); // 스풀했을 뿐 버리지 않았다

    // 정본은 저널에 전부 있다. 구멍 0.
    auto scan = Journal::scan(path);
    CHECK(scan.ok());
    CHECK_EQ(scan.value().frames.size(), static_cast<usize>(total));
    CHECK_EQ(scan.value().seqGaps().size(), static_cast<usize>(0));
    for (usize i = 0; i < scan.value().frames.size(); ++i) {
        if (scan.value().frames[i].seq != static_cast<u64>(i) + 1) {
            CHECK_FAIL("seq 가 연속이 아니다: 인덱스 " + std::to_string(i));
            break;
        }
    }

    // 막힘이 풀리면 밀린 걸 전부 밀어넣는다.
    sink.blocked = false;
    CHECK(p.pump().ok());
    CHECK_EQ(p.sentSeq(), total);
    CHECK_EQ(sink.received.size(), static_cast<usize>(total));
    CHECK_EQ(p.stats().spooled, 0ull);
    pub.value().reset();
    std::filesystem::remove(path);
}

// Sigan 이 Velopack 업데이트로 재시작한다 → 밀린 프레임 전부 도착, **구간 분리 0**.
MARI_TEST(sigan_restart_survives) {
    const std::string path = tmpPath("restart");
    auto pub = SiganPublisher::open(PublisherConfig{path, "mari-paint/0.1.0", 0xABCD});
    CHECK(pub.ok());
    auto& p = *pub.value();

    FakeSink sink;
    sink.reply = fullReply();
    p.setSink(&sink); // 연결·핸드셰이크는 pump() 가 한다
    CHECK(p.pump().ok());

    drawStroke(p, 100, 0.0f);
    const u64 beforeCrash = p.lastSeq();
    CHECK_EQ(sink.received.size(), static_cast<usize>(beforeCrash));
    const u64 sessionBefore = p.sessionId();

    sink.disconnect(); // Sigan 이 업데이트로 죽었다
    drawStroke(p, 100, 100.0f);
    drawStroke(p, 100, 200.0f); // 그리기는 계속된다. 아무도 막지 않는다
    CHECK(p.lastSeq() > beforeCrash);

    // 복귀 → 재핸드셰이크 → 밀린 구간 재전송
    CHECK(p.pump().ok());
    CHECK_EQ(p.stats().reconnects, 2ull); // 최초 연결 + 재연결. **새 Segment 가 아니다**
    CHECK_EQ(p.sentSeq(), p.lastSeq());
    CHECK_EQ(sink.received.size(), static_cast<usize>(p.lastSeq()));
    CHECK(p.stats().resent > 0);

    // 🔴 구간 분리 0: 세션도 seq 도 이어진다.
    CHECK_EQ(p.sessionId(), sessionBefore);
    CHECK_EQ(sink.lastLocal.sessionId, sessionBefore);
    for (usize i = 0; i < sink.received.size(); ++i) {
        if (sink.received[i].seq != static_cast<u64>(i) + 1) {
            CHECK_FAIL("재연결 뒤 seq 가 끊겼다: 인덱스 " + std::to_string(i));
            break;
        }
    }
    pub.value().reset();
    std::filesystem::remove(path);
}

// 수신자가 "seq N 부터 다시 달라"고 하면 그 말을 따른다(중복은 괜찮고 구멍은 안 된다).
MARI_TEST(reconnect_honors_peer_resume_point) {
    const std::string path = tmpPath("resume");
    auto pub = SiganPublisher::open(PublisherConfig{path, "mari-paint/0.1.0", 1});
    CHECK(pub.ok());
    auto& p = *pub.value();

    FakeSink sink;
    sink.reply = fullReply();
    p.setSink(&sink); // 연결·핸드셰이크는 pump() 가 한다
    CHECK(p.pump().ok());
    drawStroke(p, 50, 0.0f);

    sink.disconnect();
    sink.received.clear();
    sink.reply = fullReply();
    sink.reply.resumeFromSeq = 31; // "31번부터 못 받았다"
    CHECK(p.pump().ok());
    CHECK_EQ(sink.received.front().seq, 31ull);
    CHECK_EQ(sink.received.back().seq, p.lastSeq());
    pub.value().reset();
    std::filesystem::remove(path);
}

// 구버전 Sigan(caps 가 적다 + proto 가 낮다) → **연결 유지**, 미지원 필드만 생략.
MARI_TEST(version_skew_keeps_recording) {
    const std::string path = tmpPath("skew");
    auto pub = SiganPublisher::open(PublisherConfig{path, "mari-paint/0.9.0", 1});
    CHECK(pub.ok());
    auto& p = *pub.value();

    FakeSink sink;
    sink.reply.proto = 0;                                   // 더 낮은 프로토콜
    sink.reply.app = "sigan/0.1.0";
    sink.reply.caps = {caps::kCanvasXy, "알-수-없는-능력"};  // layer/brush 를 모른다
    p.setSink(&sink); // 연결·핸드셰이크는 pump() 가 한다
    CHECK(p.pump().ok());

    CHECK(sink.connected());                    // 🔴 끊지 않는다
    CHECK_EQ(p.negotiated().proto, 0u);         // 공통 최소로 내려간다
    CHECK(p.negotiated().protoDowngraded);
    CHECK(p.negotiated().hasCap(caps::kCanvasXy));
    CHECK(!p.negotiated().hasCap(caps::kLayer));
    CHECK(p.negotiated().dropped.size() >= 3);  // 정직하게 남긴다

    drawStroke(p, 10, 0.0f);
    CHECK_EQ(sink.received.size(), static_cast<usize>(p.lastSeq())); // 기록은 계속된다
    // 미지원 필드는 생략(0)되지만 좌표는 그대로 간다.
    CHECK_EQ(sink.received.back().layerId, 0u);
    CHECK_EQ(sink.received.back().brushId, 0u);
    CHECK_NEAR(sink.received.back().cx, 10.0, 1e-6);

    // 🔴 저널의 정본은 깎이지 않는다 — 생략은 와이어에서만 일어난다.
    auto scan = Journal::scan(path);
    CHECK(scan.ok());
    CHECK_EQ(scan.value().frames.back().layerId, 5u);
    CHECK_EQ(scan.value().frames.back().brushId, 9u);
    pub.value().reset();
    std::filesystem::remove(path);
}

// Sigan 이 아예 없을 때(로컬 모드) — 그래도 전부 기록된다.
MARI_TEST(local_mode_records_everything) {
    const std::string path = tmpPath("local");
    auto pub = SiganPublisher::open(PublisherConfig{path, "mari-paint/0.1.0", 1});
    CHECK(pub.ok());
    auto& p = *pub.value();
    CHECK(p.pump().ok()); // 싱크가 없어도 실패가 아니다
    drawStroke(p, 20, 0.0f);
    CHECK_EQ(p.stats().published, 20ull);
    CHECK_EQ(p.stats().sent, 0ull);
    auto scan = Journal::scan(path);
    CHECK(scan.ok());
    CHECK_EQ(scan.value().frames.size(), static_cast<usize>(20));
    pub.value().reset();
    std::filesystem::remove(path);
}

MARI_TEST_MAIN()
