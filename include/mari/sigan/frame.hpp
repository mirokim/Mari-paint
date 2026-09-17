// Mari Paint — Sigan 스트로크 프레임 (docs/03 4.1)
//
// 고정 길이 56바이트, **리틀엔디안 고정**. 호스트 엔디안과 무관하게
// 바이트를 직접 쌓는다 — 바이트 레이아웃이 Sigan 과의 계약이다.
//
// 🔴 경계선(docs/03 2절): 여기에는 해시체인·서명·등급 판정이 없다.
//    Mari 는 증거를 **발행**만 한다. 봉인·서명은 Sigan 의 몫이다.
#ifndef MARI_SIGAN_FRAME_HPP
#define MARI_SIGAN_FRAME_HPP

#include <mari/core/types.hpp>

#include <cstring>

namespace mari::sigan {

/// 프레임 플래그. 값은 직렬화된다 — **뒤에만 더해라.**
enum class FrameFlag : u32 {
    Down = 1u << 0,   ///< 획 시작
    Move = 1u << 1,   ///< 획 진행
    Up = 1u << 2,     ///< 획 끝 (여기서 저널을 flush 한다)
    Eraser = 1u << 3, ///< 지우개 모드
};

/// 플래그 합성/검사 헬퍼.
constexpr u32 frameFlags(FrameFlag f) noexcept { return static_cast<u32>(f); }
constexpr u32 operator|(FrameFlag a, FrameFlag b) noexcept {
    return static_cast<u32>(a) | static_cast<u32>(b);
}
constexpr u32 operator|(u32 a, FrameFlag b) noexcept { return a | static_cast<u32>(b); }
constexpr bool hasFlag(u32 flags, FrameFlag f) noexcept {
    return (flags & static_cast<u32>(f)) != 0u;
}

/// 스트로크 프레임 한 개. 필드 순서는 docs/03 4.1 표 그대로다.
///
/// 좌표는 **캔버스 좌표**(좌상단 원점, y 아래로 증가, 줌·회전·팬 불변)다.
/// `t` 는 세션 시작 기준 상대 ms — 단조 시계다(docs/03 5.6).
///
/// ⚠️ `layerId`/`brushId` 는 표가 둘 합쳐 8B 로 못박아서 **와이어에서는 u32** 다.
///    core 의 LayerId/BrushId(u64) 를 실을 때는 narrowLayerId() 로 좁히고,
///    범위를 넘으면 발행기가 리포트에 정직하게 남긴다.
struct StrokeFrame {
    u64 seq = 0;        ///< 단조 증가 시퀀스. 구멍 = 유실(docs/03 4.2)
    f64 tMs = 0.0;      ///< 세션 시작 기준 상대 ms (단조 시계)
    f32 cx = 0.0f;      ///< 캔버스 x
    f32 cy = 0.0f;      ///< 캔버스 y
    f32 pressure = 0.0f;///< 필압 0~1
    f32 tiltX = 0.0f;   ///< 기울기 x (°)
    f32 tiltY = 0.0f;   ///< 기울기 y (°)
    f32 rotation = 0.0f;///< 펜 회전 (°)
    f32 velocity = 0.0f;///< 속도 (px/ms)
    u32 layerId = 0;    ///< 레이어 (와이어 폭 u32)
    u32 brushId = 0;    ///< 브러시 프리셋 (와이어 폭 u32)
    u32 flags = 0;      ///< FrameFlag 비트합

    friend bool operator==(const StrokeFrame&, const StrokeFrame&) = default;
};

/// 와이어 레이아웃을 컴파일 타임에 못박는 패킹 구조체.
/// 직접 memcpy 하지 마라(엔디안 때문). 크기 계약을 고정하는 용도다.
#pragma pack(push, 1)
struct StrokeFrameWire {
    u64 seq;
    f64 t;
    f32 cx, cy;
    f32 p;
    f32 tiltX, tiltY;
    f32 rotation, velocity;
    u32 layerId, brushId;
    u32 flags;
};
#pragma pack(pop)

static_assert(sizeof(StrokeFrameWire) == 56, "Sigan 프레임은 56바이트 고정이다 (docs/03 4.1)");
static_assert(offsetof(StrokeFrameWire, seq) == 0, "seq 오프셋 고정");
static_assert(offsetof(StrokeFrameWire, t) == 8, "t 오프셋 고정");
static_assert(offsetof(StrokeFrameWire, cx) == 16, "cx 오프셋 고정");
static_assert(offsetof(StrokeFrameWire, p) == 24, "p 오프셋 고정");
static_assert(offsetof(StrokeFrameWire, tiltX) == 28, "tiltX 오프셋 고정");
static_assert(offsetof(StrokeFrameWire, rotation) == 36, "rotation 오프셋 고정");
static_assert(offsetof(StrokeFrameWire, layerId) == 44, "layerId 오프셋 고정");
static_assert(offsetof(StrokeFrameWire, flags) == 52, "flags 오프셋 고정");

/// 프레임 한 개의 와이어 크기(바이트).
inline constexpr usize kFrameSize = sizeof(StrokeFrameWire);

/// 프레임을 리틀엔디안 56바이트로 쓴다. 핫 패스 — 할당·예외 없다.
void encodeFrame(const StrokeFrame& in, u8* out) noexcept;

/// 리틀엔디안 56바이트를 프레임으로 읽는다. 핫 패스 — 할당·예외 없다.
void decodeFrame(const u8* in, StrokeFrame& out) noexcept;

/// u64 식별자를 와이어 폭(u32)으로 좁힌다. 넘치면 `truncated` 를 true 로 만든다.
/// 조용히 버리지 않는다 — 호출자가 리포트에 남긴다.
u32 narrowId(u64 id, bool& truncated) noexcept;

/// CRC-32 (IEEE 802.3, zlib 호환 다항식). 저널 레코드 무결성 검사용.
/// 서명이 아니다 — 잘린 레코드를 찾는 용도일 뿐이다.
u32 crc32(const u8* data, usize len, u32 seed = 0) noexcept;

} // namespace mari::sigan

#endif // MARI_SIGAN_FRAME_HPP
