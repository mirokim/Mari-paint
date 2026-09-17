// docs/03 6절 — Sigan 미설치 시 .ora 에 넣을 무서명 로그.
// 🔴 등급은 "unsigned" 하나뿐이다. native/deferred 를 자칭하면 설계 위반이다.
#include <mari/sigan/prooflog.hpp>
#include <mari/sigan/publisher.hpp>
#include <mari/test/harness.hpp>

#include <filesystem>
#include <string>

using namespace mari;
using namespace mari::sigan;

namespace {

std::string tmpPath() {
    return (std::filesystem::temp_directory_path() / "mari_prooflog.jrnl").string();
}

bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

} // namespace

MARI_TEST(prooflog_is_unsigned_only) {
    const std::string path = tmpPath();
    auto pub = SiganPublisher::open(PublisherConfig{path, "mari-paint/0.1.0", 1234});
    CHECK(pub.ok());
    auto& p = *pub.value();

    StrokeSample s;
    s.layerId = 2;
    s.brushId = 8;
    for (int stroke = 0; stroke < 3; ++stroke) {
        s.flags = frameFlags(FrameFlag::Down);
        p.publish(s);
        for (int i = 0; i < 4; ++i) {
            s.flags = frameFlags(FrameFlag::Move);
            s.pos.x += 1.0f;
            p.publish(s);
        }
        s.flags = frameFlags(FrameFlag::Up);
        p.publish(s);
        p.endStroke();
    }
    const u64 sessionId = p.sessionId();
    const i64 anchor = p.clock().wallAnchorUnixMs();
    pub.value().reset();

    auto scan = recoverJournal(path);
    CHECK(scan.ok());
    CHECK_EQ(scan.value().frames.size(), static_cast<usize>(18));

    const auto strokes = summarizeStrokes(scan.value());
    CHECK_EQ(strokes.size(), static_cast<usize>(3));
    CHECK_EQ(strokes.front().seqFirst, 1ull);
    CHECK_EQ(strokes.front().seqLast, 6ull);
    CHECK_EQ(strokes.front().pointCount, 6ull);
    CHECK_EQ(strokes.back().seqLast, 18ull);

    ProofLogInput in;
    in.app = "mari-paint/0.1.0";
    in.sessionId = sessionId;
    in.wallAnchorUnixMs = anchor;
    in.scan = &scan.value();
    auto log = buildProofLog(in);
    CHECK(log.ok());
    const std::string& json = log.value();

    CHECK(contains(json, "\"grade\": \"unsigned\""));
    CHECK(contains(json, "\"signed\": false"));
    CHECK(contains(json, "인증서가 아니다"));
    CHECK(contains(json, "\"lostFrames\": 0"));
    CHECK(contains(json, "\"frameCount\": 18"));
    // 🔴 Mari 가 자기 등급을 올려 부르면 안 된다.
    CHECK(!contains(json, "native"));
    CHECK(!contains(json, "deferred"));
    CHECK(!contains(json, "rawinput"));
    // 🔴 서명·해시체인은 Mari 의 일이 아니다(docs/03 2절).
    CHECK(!contains(json, "signature"));
    CHECK(!contains(json, "ES256"));
    CHECK(!contains(json, "chain"));
    std::filesystem::remove(path);
}

MARI_TEST(prooflog_needs_a_scan) {
    ProofLogInput in;
    auto log = buildProofLog(in);
    CHECK(!log.ok());
    CHECK_EQ(static_cast<int>(log.code()), static_cast<int>(ErrorCode::InvalidArgument));
}

MARI_TEST_MAIN()
