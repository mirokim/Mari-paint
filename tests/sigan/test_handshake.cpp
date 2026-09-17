// docs/03 5.5 — 버전 협상 규칙 3개를 코드가 지키는지 본다.
#include <mari/sigan/handshake.hpp>
#include <mari/test/harness.hpp>

using namespace mari;
using namespace mari::sigan;

MARI_TEST(handshake_roundtrip) {
    Handshake h;
    h.proto = 1;
    h.app = "mari-paint/0.1.0";
    h.caps = {caps::kCanvasXy, caps::kLayer, caps::kPixelHash};
    h.sessionId = 0xFEEDFACEull;
    h.resumeFromSeq = 991;
    h.wallAnchorUnixMs = 1'700'000'000'000;

    auto back = decodeHandshake(encodeHandshake(h));
    CHECK(back.ok());
    CHECK_EQ(back.value().proto, 1u);
    CHECK_EQ(back.value().app, std::string("mari-paint/0.1.0"));
    CHECK_EQ(back.value().caps.size(), static_cast<usize>(3));
    CHECK(back.value().hasCap(caps::kPixelHash));
    CHECK_EQ(back.value().sessionId, 0xFEEDFACEull);
    CHECK_EQ(back.value().resumeFromSeq, 991ull);
    CHECK_EQ(back.value().wallAnchorUnixMs, 1'700'000'000'000);
}

// 규칙 2: **모르는 필드는 무시한다. 에러가 아니다.**
MARI_TEST(unknown_fields_are_ignored_not_errors) {
    const std::string json =
        R"({"proto":1,"app":"sigan/9.9.9","accepts":["canvas-xy","view"],)"
        R"("futureThing":{"nested":[1,2,{"deep":true}]},"anotherOne":42,"weird":null,)"
        R"("sessionId":77})";
    auto h = decodeHandshake(json);
    CHECK(h.ok());
    CHECK_EQ(h.value().app, std::string("sigan/9.9.9"));
    CHECK_EQ(h.value().caps.size(), static_cast<usize>(2)); // accepts 도 caps 로 받는다
    CHECK(h.value().hasCap(caps::kView));
    CHECK_EQ(h.value().sessionId, 77ull);
}

MARI_TEST(malformed_handshake_is_a_parse_error) {
    auto h = decodeHandshake("이건 JSON 이 아니다");
    CHECK(!h.ok());
    CHECK_EQ(static_cast<int>(h.code()), static_cast<int>(ErrorCode::ParseError));
}

// 규칙 3: proto 가 달라도 **끊지 않는다.** 공통 최소로 내려간다.
MARI_TEST(proto_mismatch_downgrades_without_disconnect) {
    Handshake local;
    local.proto = 3;
    local.caps = {caps::kCanvasXy, caps::kLayer, caps::kBrush, caps::kPixelHash};
    Handshake remote;
    remote.proto = 1;
    remote.caps = {caps::kCanvasXy, caps::kLayer};

    const Negotiated n = negotiate(local, remote);
    CHECK_EQ(n.proto, 1u);
    CHECK(n.protoDowngraded);
    CHECK_EQ(n.caps.size(), static_cast<usize>(2));
    CHECK(n.hasCap(caps::kCanvasXy));
    CHECK(!n.hasCap(caps::kPixelHash));
    // 상대가 모르는 능력은 조용히 사라지지 않고 목록에 남는다.
    CHECK_EQ(n.dropped.size(), static_cast<usize>(2));
}

MARI_TEST(same_version_keeps_everything) {
    Handshake local;
    local.caps = {caps::kCanvasXy, caps::kLayer};
    Handshake remote = local;
    const Negotiated n = negotiate(local, remote);
    CHECK(!n.protoDowngraded);
    CHECK_EQ(n.caps.size(), static_cast<usize>(2));
    CHECK_EQ(n.dropped.size(), static_cast<usize>(0));
}

MARI_TEST_MAIN()
