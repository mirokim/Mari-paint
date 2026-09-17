// docs/03 4.1 — 프레임 인코딩 왕복 + **바이트 레이아웃 고정(골든 바이트)**
#include <mari/sigan/frame.hpp>
#include <mari/test/harness.hpp>

#include <cstring>
#include <vector>

using namespace mari;
using namespace mari::sigan;

MARI_TEST(frame_size_is_56) {
    CHECK_EQ(kFrameSize, static_cast<usize>(56));
    CHECK_EQ(sizeof(StrokeFrameWire), static_cast<usize>(56));
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

    const u8 golden[56] = {
        0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4A,
        0x93, 0x40, 0x00, 0x80, 0xC8, 0x42, 0x00, 0x00, 0x08, 0xC1, 0x00, 0x00, 0x40, 0x3F,
        0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x80, 0xBE, 0x00, 0x00, 0xF0, 0x41, 0x00, 0x00,
        0x08, 0x40, 0x09, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00};

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

    u8 wire[kFrameSize];
    encodeFrame(f, wire);
    StrokeFrame back;
    decodeFrame(wire, back);
    CHECK(f == back);
    CHECK(hasFlag(back.flags, FrameFlag::Eraser));
    CHECK(!hasFlag(back.flags, FrameFlag::Down));
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
