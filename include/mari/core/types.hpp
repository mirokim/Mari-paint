// Mari Paint — 공용 기반 타입
//
// ── 좌표계 규약 (전 모듈 공통, 어기면 리뷰에서 막는다) ─────────────────────
//   * **캔버스 좌표(canvas space)**: 원점은 캔버스의 **좌상단**, x는 오른쪽,
//     y는 **아래로 증가**한다. 단위는 픽셀. 줌·회전·팬과 **무관한 불변값**이다.
//     docs/03-sigan-integration.md 4.1절이 말하는 `cx, cy` 가 바로 이 좌표다.
//   * **화면 좌표(screen space)**: 위젯/윈도우 픽셀. 뷰 변환(줌·회전·팬)에 따라 변한다.
//     캔버스 좌표와 **절대 섞지 않는다.** 화면 좌표를 다루는 타입은 ui/ 모듈이 따로 둔다.
//   * 타일 좌표는 캔버스 좌표를 kTileSize 로 나눈 값이다(내림, 음수도 내림).
//   * 사각형 Rect 는 [x, x+w) × [y, y+h) 반열림 구간이다. 오른쪽/아래 경계는 포함하지 않는다.
// ───────────────────────────────────────────────────────────────────────
#ifndef MARI_CORE_TYPES_HPP
#define MARI_CORE_TYPES_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace mari {

// ── 스칼라 별칭 ──────────────────────────────────────────────────────────
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using f32 = float;
using f64 = double;
using usize = std::size_t;

/// 레이어 식별자. 0은 "없음". 문서 안에서 유일하며 재사용하지 않는다.
using LayerId = u64;
/// 브러시 프리셋 식별자. 0은 "없음".
using BrushId = u64;

inline constexpr LayerId kInvalidLayerId = 0;
inline constexpr BrushId kInvalidBrushId = 0;

/// 타일 한 변의 길이(px). docs/02 3절 — 64×64 고정이다.
inline constexpr i32 kTileSize = 64;

// ── 기하 ─────────────────────────────────────────────────────────────────

/// 정수 점 (캔버스 좌표, 좌상단 원점 · y 아래로 증가).
struct Point {
    i32 x = 0;
    i32 y = 0;

    friend constexpr bool operator==(const Point&, const Point&) = default;
};

/// 실수 점 (캔버스 좌표). 스트로크 보간처럼 서브픽셀이 필요한 곳에 쓴다.
struct PointF {
    f32 x = 0.0f;
    f32 y = 0.0f;

    friend constexpr bool operator==(const PointF&, const PointF&) = default;
};

/// 정수 크기. 음수는 허용하지 않는다(호출자가 보장).
struct Size {
    i32 width = 0;
    i32 height = 0;

    [[nodiscard]] constexpr bool isEmpty() const { return width <= 0 || height <= 0; }
    [[nodiscard]] constexpr i64 area() const {
        return static_cast<i64>(width) * static_cast<i64>(height);
    }

    friend constexpr bool operator==(const Size&, const Size&) = default;
};

/// 실수 크기.
struct SizeF {
    f32 width = 0.0f;
    f32 height = 0.0f;

    [[nodiscard]] constexpr bool isEmpty() const { return width <= 0.0f || height <= 0.0f; }

    friend constexpr bool operator==(const SizeF&, const SizeF&) = default;
};

/// 정수 사각형. 반열림 구간 [x, x+width) × [y, y+height).
/// 더티 영역·타일 범위 표현에 쓴다.
struct Rect {
    i32 x = 0;
    i32 y = 0;
    i32 width = 0;
    i32 height = 0;

    [[nodiscard]] constexpr i32 left() const { return x; }
    [[nodiscard]] constexpr i32 top() const { return y; }
    /// 배타적 오른쪽 경계 — 이 x는 포함되지 않는다.
    [[nodiscard]] constexpr i32 right() const { return x + width; }
    /// 배타적 아래쪽 경계 — 이 y는 포함되지 않는다.
    [[nodiscard]] constexpr i32 bottom() const { return y + height; }
    [[nodiscard]] constexpr bool isEmpty() const { return width <= 0 || height <= 0; }
    [[nodiscard]] constexpr Size size() const { return Size{width, height}; }

    [[nodiscard]] constexpr bool contains(Point p) const {
        return p.x >= x && p.x < right() && p.y >= y && p.y < bottom();
    }
    [[nodiscard]] constexpr bool intersects(const Rect& o) const {
        return !isEmpty() && !o.isEmpty() && x < o.right() && o.x < right() && y < o.bottom() &&
               o.y < bottom();
    }
    /// 교집합. 겹치지 않으면 빈 사각형을 준다.
    [[nodiscard]] constexpr Rect intersected(const Rect& o) const {
        const i32 l = std::max(x, o.x);
        const i32 t = std::max(y, o.y);
        const i32 r = std::min(right(), o.right());
        const i32 b = std::min(bottom(), o.bottom());
        if (r <= l || b <= t)
            return Rect{};
        return Rect{l, t, r - l, b - t};
    }
    /// 합집합(경계 상자). 한쪽이 비어 있으면 다른 쪽을 그대로 준다.
    [[nodiscard]] constexpr Rect united(const Rect& o) const {
        if (isEmpty())
            return o;
        if (o.isEmpty())
            return *this;
        const i32 l = std::min(x, o.x);
        const i32 t = std::min(y, o.y);
        const i32 r = std::max(right(), o.right());
        const i32 b = std::max(bottom(), o.bottom());
        return Rect{l, t, r - l, b - t};
    }

    static constexpr Rect fromBounds(i32 l, i32 t, i32 r, i32 b) {
        return Rect{l, t, r - l, b - t};
    }

    friend constexpr bool operator==(const Rect&, const Rect&) = default;
};

/// 실수 사각형.
struct RectF {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 width = 0.0f;
    f32 height = 0.0f;

    [[nodiscard]] constexpr f32 right() const { return x + width; }
    [[nodiscard]] constexpr f32 bottom() const { return y + height; }
    [[nodiscard]] constexpr bool isEmpty() const { return width <= 0.0f || height <= 0.0f; }

    friend constexpr bool operator==(const RectF&, const RectF&) = default;
};

// ── 색 ───────────────────────────────────────────────────────────────────

/// 8비트 RGBA. **비프리멀티플라이(straight alpha)** 가 기본이다.
/// 프리멀티플라이가 필요한 곳(합성 내부)은 그 사실을 함수 주석에 명시한다.
struct Color8 {
    u8 r = 0, g = 0, b = 0, a = 255;

    static constexpr Color8 rgba(u8 r_, u8 g_, u8 b_, u8 a_ = 255) { return Color8{r_, g_, b_, a_}; }
    /// 0xAARRGGBB 로부터.
    static constexpr Color8 fromArgb32(u32 v) {
        return Color8{static_cast<u8>((v >> 16) & 0xFFu), static_cast<u8>((v >> 8) & 0xFFu),
                      static_cast<u8>(v & 0xFFu), static_cast<u8>((v >> 24) & 0xFFu)};
    }
    /// 0xAARRGGBB 로.
    [[nodiscard]] constexpr u32 toArgb32() const {
        return (static_cast<u32>(a) << 24) | (static_cast<u32>(r) << 16) |
               (static_cast<u32>(g) << 8) | static_cast<u32>(b);
    }

    friend constexpr bool operator==(const Color8&, const Color8&) = default;
};

/// 16비트 RGBA. 고비트 문서(.psd 16bit 등)용.
struct Color16 {
    u16 r = 0, g = 0, b = 0, a = 65535;

    friend constexpr bool operator==(const Color16&, const Color16&) = default;
};

/// 32비트 float RGBA. 채널 범위는 보통 [0,1] 이지만 HDR 을 위해 벗어날 수 있다.
struct ColorF {
    f32 r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;

    friend constexpr bool operator==(const ColorF&, const ColorF&) = default;
};

// ── 픽셀 포맷 ────────────────────────────────────────────────────────────

/// 타일/레이어 픽셀 저장 형식. 채널 순서는 이름 그대로 메모리 순서다.
enum class PixelFormat : u8 {
    Unknown = 0,
    Gray8,     ///< 8비트 그레이 1채널 (마스크용)
    Gray16,    ///< 16비트 그레이 1채널
    RGBA8,     ///< 8비트 ×4. 코어 기본값
    RGBA16,    ///< 16비트 ×4
    RGBA32F,   ///< float ×4
};

/// 한 픽셀의 바이트 수. Unknown 은 0.
[[nodiscard]] constexpr usize bytesPerPixel(PixelFormat f) {
    switch (f) {
    case PixelFormat::Gray8:
        return 1;
    case PixelFormat::Gray16:
        return 2;
    case PixelFormat::RGBA8:
        return 4;
    case PixelFormat::RGBA16:
        return 8;
    case PixelFormat::RGBA32F:
        return 16;
    case PixelFormat::Unknown:
        break;
    }
    return 0;
}

/// 채널 개수. Unknown 은 0.
[[nodiscard]] constexpr int channelCount(PixelFormat f) {
    switch (f) {
    case PixelFormat::Gray8:
    case PixelFormat::Gray16:
        return 1;
    case PixelFormat::RGBA8:
    case PixelFormat::RGBA16:
    case PixelFormat::RGBA32F:
        return 4;
    case PixelFormat::Unknown:
        break;
    }
    return 0;
}

// ── 블렌드 모드 ──────────────────────────────────────────────────────────

/// 레이어/스탬프 합성 모드. 값은 직렬화에 쓰이므로 **중간에 끼워 넣지 말고 뒤에만 더한다.**
/// 이름은 OpenRaster/PSD 관례를 따른다(io/ora 가 문자열 ↔ enum 매핑을 소유한다).
enum class BlendMode : u8 {
    Normal = 0,
    Multiply,
    Screen,
    Overlay,
    Darken,
    Lighten,
    ColorDodge,
    ColorBurn,
    HardLight,
    SoftLight,
    Difference,
    Exclusion,
    Hue,
    Saturation,
    Color,
    Luminosity,
    Add,      ///< 선형 닷지(가산)
    Subtract, ///< 감산
    Erase,    ///< 대상 알파를 깎는다(지우개 스탬프용)
};

/// 직렬화/로그용 안정 문자열. 모르는 값은 "normal".
[[nodiscard]] constexpr const char* blendModeName(BlendMode m) {
    switch (m) {
    case BlendMode::Normal:
        return "normal";
    case BlendMode::Multiply:
        return "multiply";
    case BlendMode::Screen:
        return "screen";
    case BlendMode::Overlay:
        return "overlay";
    case BlendMode::Darken:
        return "darken";
    case BlendMode::Lighten:
        return "lighten";
    case BlendMode::ColorDodge:
        return "color-dodge";
    case BlendMode::ColorBurn:
        return "color-burn";
    case BlendMode::HardLight:
        return "hard-light";
    case BlendMode::SoftLight:
        return "soft-light";
    case BlendMode::Difference:
        return "difference";
    case BlendMode::Exclusion:
        return "exclusion";
    case BlendMode::Hue:
        return "hue";
    case BlendMode::Saturation:
        return "saturation";
    case BlendMode::Color:
        return "color";
    case BlendMode::Luminosity:
        return "luminosity";
    case BlendMode::Add:
        return "add";
    case BlendMode::Subtract:
        return "subtract";
    case BlendMode::Erase:
        return "erase";
    }
    return "normal";
}

/// 기본 색 타입 별칭. 코어 합성 경로의 기준 표현이다.
using Color = Color8;

// ── 타일 좌표 헬퍼 ───────────────────────────────────────────────────────

/// 캔버스 좌표가 속한 타일 인덱스. 음수도 아래로 내림한다(-1 → -1번 타일).
[[nodiscard]] constexpr i32 tileIndexFor(i32 canvasCoord) {
    return canvasCoord >= 0 ? canvasCoord / kTileSize : -(((-canvasCoord) + kTileSize - 1) / kTileSize);
}

/// 타일 인덱스의 좌상단 캔버스 좌표.
[[nodiscard]] constexpr i32 tileOrigin(i32 tileIndex) { return tileIndex * kTileSize; }

} // namespace mari

#endif // MARI_CORE_TYPES_HPP
