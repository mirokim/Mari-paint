// docs/03 4.1 — 프레임 인코딩 왕복 + **바이트 레이아웃 고정(골든 바이트)**
#include <mari/sigan/frame.hpp>
#include <mari/test/harness.hpp>

#include <cstring>
#include <vector>

using namespace mari;
using namespace mari::sigan;

MARI_TEST(frame_size_is_64) {
    // 🔴 2026-09-17 (A0): 56 → 64. origin(1B) + reserved(3B) + agentId(4B) 를 꼬리에 더했다.
    //    앞 56바이트는 오프셋·의미가 그대로다(docs/03 5.5 규칙 1 "필드는 더하기만 한다").
    CHECK_EQ(kFrameSize, static_cast<usize>(64));
    CHECK_EQ(sizeof(StrokeFrameWire), static_cast<usize>(64));
    CHECK_EQ(offsetof(StrokeFrameWire, origin), static_cast<usize>(56));
    CHECK_EQ(offsetof(StrokeFrameWire, agentId), static_cast<usize>(60));
}

MARI_TEST(frame_golden_bytes) {
    // 이 바이트열이 Sigan 과의 계약이다. 바뀌면 수신부가 깨진다.
    StrokeFrame f;
    f.seq = 7;
    f.tMs = 1234.5;
    f.cx = 100.25f;
    f.cy = -8.5f;
    f.pressure = 0.75f;
    f.tiltX = 0.5f;
    f.tiltY = -0.25f;
    f.rotation = 30.0f;
    f.velocity = 2.125f;
    f.layerId = 9;
    f.brushId = 4;
    f.flags = FrameFlag::Down | FrameFlag::Move;
    f.origin = StrokeOrigin::HumanPen;
    f.agentId = 0;

    // 🔴 골든 바이트가 바뀐 이유 (2026-09-17 · A0 · docs/05 3.1):
    //    획의 출처(origin)가 Sigan 의 **서명 정본** 안에 들어가야 하기 때문이다.
    //    서명 밖이면 사후에 고칠 수 있어 아무 의미가 없다 — docs/03 8절의 `:src` 사고가
    //    정확히 그것이었다("붙여넣기가 있었다"는 서명 안, "어디서 왔나"는 서명 밖).
    //    서명 포맷은 한 번 배포되면 하위 호환 때문에 영원히 못 바꾸므로, 릴리스 전인
    //    지금이 유일한 창문이었다.
    //    **앞 56바이트는 한 바이트도 움직이지 않았다.** 꼬리 8바이트만 더했다:
    //      오프셋 56 = origin(StrokeOrigin, 여기서는 HumanPen = 1)
    //      오프셋 57~59 = reserved, 0 고정
    //      오프셋 60~63 = agentId 다이제스트(리틀엔디안 u32, 사람 획이면 0)
    const u8 golden[64] = {
        0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4A,
        0x93, 0x40, 0x00, 0x80, 0xC8, 0x42, 0x00, 0x00, 0x08, 0xC1, 0x00, 0x00, 0x40, 0x3F,
        0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x80, 0xBE, 0x00, 0x00, 0xF0, 0x41, 0x00, 0x00,
        0x08, 0x40, 0x09, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

    u8 wire[kFrameSize];
    encodeFrame(f, wire);
    for (usize i = 0; i < kFrameSize; ++i) {
        if (wire[i] != golden[i]) {
            CHECK_FAIL("골든 바이트 불일치: 오프셋 " + std::to_string(i) + " 기대 " +
                       std::to_string(golden[i]) + " 실제 " + std::to_string(wire[i]));
            break;
        }
    }
}

MARI_TEST(frame_roundtrip) {
    StrokeFrame f;
    f.seq = 0xDEADBEEFCAFEull;
    f.tMs = 987654.321;
    f.cx = -1.5f;
    f.cy = 2048.75f;
    f.pressure = 1.0f;
    f.tiltX = -60.0f;
    f.tiltY = 45.0f;
    f.rotation = 359.5f;
    f.velocity = 0.125f;
    f.layerId = 0xFFFFFFFFu;
    f.brushId = 123456;
    f.flags = FrameFlag::Up | FrameFlag::Eraser;
    f.origin = StrokeOrigin::Agent;
    f.agentId = 0xABCD1234u;

    u8 wire[kFrameSize];
    encodeFrame(f, wire);
    StrokeFrame back;
    decodeFrame(wire, back);
    CHECK(f == back);
    CHECK(hasFlag(back.flags, FrameFlag::Eraser));
    CHECK(!hasFlag(back.flags, FrameFlag::Down));
    CHECK(back.origin == StrokeOrigin::Agent);
    CHECK_EQ(back.agentId, 0xABCD1234u);
}

// 🔴 docs/05 6절 `origin_in_signature`:
//    origin 만 바꿔도 **프레임 바이트가 바뀐다.** 다른 건 전부 같은데 바이트가 다르다면
//    origin 은 서명될 바이트열 안에 있다는 뜻이고, 사후에 고치면 서명이 깨진다.
MARI_TEST(origin_in_signature) {
    StrokeFrame human;
    human.seq = 42;
    human.tMs = 100.0;
    human.cx = 10.0f;
    human.cy = 20.0f;
    human.pressure = 0.5f;
    human.layerId = 3;
    human.brushId = 1;
    human.flags = frameFlags(FrameFlag::Down);
    human.origin = StrokeOrigin::HumanPen;

    StrokeFrame ai = human;
    ai.origin = StrokeOrigin::Agent; // 바뀐 건 출처 하나뿐이다

    u8 a[kFrameSize];
    u8 b[kFrameSize];
    encodeFrame(human, a);
    encodeFrame(ai, b);
    CHECK(std::memcmp(a, b, kFrameSize) != 0);
    // 정확히 오프셋 56 한 바이트만 다르다 — 나머지는 한 바이트도 안 움직였다.
    usize diffs = 0;
    usize where = 0;
    for (usize i = 0; i < kFrameSize; ++i) {
        if (a[i] != b[i]) {
            ++diffs;
            where = i;
        }
    }
    CHECK_EQ(diffs, static_cast<usize>(1));
    CHECK_EQ(where, static_cast<usize>(56));

    // agentId 도 마찬가지다. 같은 Agent 획이라도 누가 그렸는지가 바이트를 바꾼다.
    StrokeFrame ai2 = ai;
    ai2.agentId = 7;
    u8 c[kFrameSize];
    encodeFrame(ai2, c);
    CHECK(std::memcmp(b, c, kFrameSize) != 0);
}

// 출처를 채우지 않은 프레임은 **사람 획으로 보이지 않는다.**
// 값 초기화(0)는 "미지정"이지 HumanPen 이 아니다.
MARI_TEST(frame_default_origin_is_not_human) {
    const StrokeFrame f{};
    CHECK_EQ(static_cast<int>(static_cast<u8>(f.origin)), static_cast<int>(kOriginUnspecified));
    CHECK(!isKnownOrigin(f.origin));
    CHECK(!isHumanOrigin(f.origin));
    CHECK_EQ(std::string(strokeOriginName(f.origin)), std::string("unspecified"));
}

MARI_TEST(frame_narrow_id_reports_truncation) {
    bool truncated = false;
    CHECK_EQ(narrowId(42, truncated), 42u);
    CHECK(!truncated);
    // u64 식별자가 와이어 폭을 넘으면 **조용히 넘어가지 않는다.**
    CHECK_EQ(narrowId(0x1'0000'0001ull, truncated), 1u);
    CHECK(truncated);
}

MARI_TEST(crc32_matches_known_vector) {
    const char* s = "123456789";
    CHECK_EQ(crc32(reinterpret_cast<const u8*>(s), 9), 0xCBF43926u);
}

MARI_TEST_MAIN()
