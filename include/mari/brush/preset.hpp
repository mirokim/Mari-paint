// Mari Paint — MariBrushPreset: 브러시 공통 중간 표현
//
// docs/02 5절 그대로다.
//     .abr ──┐
//     .sut ──┼──►  MariBrushPreset  ──►  IBrushEngine (libmypaint 등)
//     .myb ──┤        (공통 중간 표현)
//     .kpp ──┘
//
// 담는 것: 팁(비트맵/절차적, 크기·각도·종횡비) · spacing · 동적 반응 맵 {입력→출력} · 텍스처.
//
// **번역 원칙 — 정직하게 실패한다.**
// 포맷마다 표현력이 다르므로 100% 재현은 불가능하다. 번역할 수 없는 파라미터를
// 조용히 버리지 말고 ImportReport 에 남겨 사용자에게 보여준다.
// (Krita 의 .abr 임포트가 욕먹는 이유가 정확히 이걸 안 해서다.)
#ifndef MARI_BRUSH_PRESET_HPP
#define MARI_BRUSH_PRESET_HPP

#include <mari/core/types.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mari::brush {

// ── 팁 ───────────────────────────────────────────────────────────────────

/// 팁의 종류.
enum class TipKind : u8 {
    Procedural = 0, ///< 절차적 모양(원/다각형). 비트맵이 없다
    Bitmap,         ///< 비트맵 스탬프 이미지
};

/// 절차적 팁의 모양.
enum class ProceduralShape : u8 {
    Circle = 0,
    Square,
    Diamond,
};

/// 그레이스케일 스탬프 이미지. 값이 클수록 잉크가 많이 얹힌다(0 = 안 찍힘).
/// 픽셀은 행 우선, stride == width 로 빈틈 없이 채운다.
struct GrayImage {
    i32 width = 0;
    i32 height = 0;
    std::vector<u8> pixels;

    [[nodiscard]] bool empty() const { return width <= 0 || height <= 0 || pixels.empty(); }
};

/// 브러시 팁 — 한 번 찍히는 도장 하나의 모양.
struct BrushTip {
    TipKind kind = TipKind::Procedural;
    ProceduralShape shape = ProceduralShape::Circle;

    /// 비트맵 팁일 때의 스탬프 이미지. 절차적이면 비어 있다.
    GrayImage bitmap;

    /// 기본 지름(px). 동적 반응의 Size 출력이 여기에 곱해진다.
    f32 diameter = 20.0f;
    /// 기본 회전 각(도). 캔버스 기준 시계 방향(y가 아래로 증가하므로 화면상 시계 방향).
    f32 angle = 0.0f;
    /// 종횡비 = 짧은 축 / 긴 축. 1.0 이 원, 0.0 에 가까울수록 납작하다.
    /// (포토샵의 "roundness" 와 같은 의미다.)
    f32 aspectRatio = 1.0f;
    /// 가장자리 경도. 0 = 완전히 흐림, 1 = 딱딱한 경계. 절차적 팁에만 의미가 있다.
    f32 hardness = 1.0f;
};

// ── 동적 반응 ────────────────────────────────────────────────────────────

/// 동적 반응의 **입력**(장치·상태에서 오는 값).
/// 정규화 범위는 각 항목 주석에 적힌 대로다.
enum class DynamicInput : u8 {
    Pressure = 0, ///< 필압. 0..1
    TiltX,        ///< 기울기 X. -1..1 (좌 ↔ 우)
    TiltY,        ///< 기울기 Y. -1..1 (위 ↔ 아래)
    Azimuth,      ///< 펜 방위각. 0..1 이 0°..360°
    Velocity,     ///< 커서 속도. 0..1 로 정규화(엔진이 상한을 정한다)
    Random,       ///< 스탬프마다 새로 뽑는 난수. 0..1
    Direction,    ///< 스트로크 진행 방향. 0..1 이 0°..360°
    Fade,         ///< 스트로크 시작부터의 진행도. 0..1
};

inline constexpr int kDynamicInputCount = 8;

/// 동적 반응의 **출력**(브러시 파라미터).
enum class DynamicOutput : u8 {
    Size = 0,  ///< 팁 지름 배율
    Opacity,   ///< 스탬프 불투명도 배율
    Flow,      ///< 잉크 유량 배율
    Roundness, ///< 종횡비 배율
    Rotation,  ///< 팁 회전 가산(도)
    Scatter,   ///< 스탬프 위치 흩뿌림(팁 지름 대비 비율)
};

inline constexpr int kDynamicOutputCount = 6;

/// 안정 문자열(임포트 리포트·직렬화·로그용).
[[nodiscard]] constexpr const char* dynamicInputName(DynamicInput i) {
    switch (i) {
    case DynamicInput::Pressure: return "pressure";
    case DynamicInput::TiltX:    return "tilt-x";
    case DynamicInput::TiltY:    return "tilt-y";
    case DynamicInput::Azimuth:  return "azimuth";
    case DynamicInput::Velocity: return "velocity";
    case DynamicInput::Random:   return "random";
    case DynamicInput::Direction:return "direction";
    case DynamicInput::Fade:     return "fade";
    }
    return "unknown";
}

/// 안정 문자열(임포트 리포트·직렬화·로그용).
[[nodiscard]] constexpr const char* dynamicOutputName(DynamicOutput o) {
    switch (o) {
    case DynamicOutput::Size:      return "size";
    case DynamicOutput::Opacity:   return "opacity";
    case DynamicOutput::Flow:      return "flow";
    case DynamicOutput::Roundness: return "roundness";
    case DynamicOutput::Rotation:  return "rotation";
    case DynamicOutput::Scatter:   return "scatter";
    }
    return "unknown";
}

/// 반응 곡선의 점 하나. x = 입력값, y = 출력 배율/가산값.
struct CurvePoint {
    f32 x = 0.0f;
    f32 y = 0.0f;
};

/// 입력 → 출력 반응 곡선. 점은 x 오름차순으로 정렬해 둔다.
/// 점이 0개면 "반응 없음"(항등), 1개면 상수다.
struct ResponseCurve {
    std::vector<CurvePoint> points;

    [[nodiscard]] bool empty() const { return points.empty(); }
};

/// 동적 반응 한 줄: {입력} → {출력} 을 curve 로 잇는다.
/// 같은 출력에 여러 입력이 걸리면 곱(Size/Opacity/Flow/Roundness) 또는
/// 합(Rotation/Scatter)으로 합친다 — 합치는 규칙은 엔진이 소유한다.
struct DynamicLink {
    DynamicInput input = DynamicInput::Pressure;
    DynamicOutput output = DynamicOutput::Size;
    ResponseCurve curve;
    /// 반응 세기. 0 이면 이 연결은 사실상 꺼진 것이다.
    f32 amount = 1.0f;
};

// ── 텍스처 ───────────────────────────────────────────────────────────────

/// 페이퍼/브러시 텍스처.
struct BrushTexture {
    GrayImage image;
    /// 텍스처 배율. 1.0 = 원본 크기.
    f32 scale = 1.0f;
    /// 텍스처가 잉크에 개입하는 세기. 0..1
    f32 depth = 1.0f;
    /// 텍스처 합성 모드.
    BlendMode blendMode = BlendMode::Multiply;
    /// 캔버스에 고정(true)인지 스탬프에 붙어 따라다니는지(false).
    bool anchoredToCanvas = true;
};

// ── 프리셋 ───────────────────────────────────────────────────────────────

/// 브러시 공통 중간 표현. .abr/.sut/.myb/.kpp 가 전부 이걸로 번역된다.
/// 엔진(IBrushEngine)은 이것만 보고 스탬프를 찍는다.
struct MariBrushPreset {
    /// 사람이 읽는 이름. 원본 파일의 이름을 그대로 쓴다.
    std::string name;
    /// 원본 포맷 표기: "abr" / "sut" / "myb" / "kpp" / "native".
    std::string sourceFormat;
    /// 원본 파일 안에서의 식별자(있으면). 재임포트 대조에 쓴다.
    std::string sourceId;

    /// 이 프리셋을 돌릴 엔진 이름. 비어 있으면 기본 엔진.
    std::string engine;

    BrushTip tip;

    /// 스탬프 간격. **팁 지름 대비 비율**이다 (0.1 = 지름의 10%마다 찍는다).
    /// 포토샵/CSP 의 "spacing %" 를 100 으로 나눈 값.
    f32 spacing = 0.1f;

    /// 기본 불투명도 0..1.
    f32 opacity = 1.0f;
    /// 기본 유량 0..1.
    f32 flow = 1.0f;

    /// 스트로크 합성 모드.
    BlendMode blendMode = BlendMode::Normal;

    /// 동적 반응 맵 {입력 → 출력}.
    std::vector<DynamicLink> dynamics;

    /// 텍스처. 없으면 비어 있다.
    std::optional<BrushTexture> texture;

    /// 엔진이 이해할 수도 있는 추가 파라미터(키 → 값). 번역기가 확신 없이 넘긴 것들.
    std::vector<std::pair<std::string, f32>> extraParams;
};

// ── 임포트 리포트 ────────────────────────────────────────────────────────

/// 리포트 항목의 심각도.
enum class ImportSeverity : u8 {
    Info = 0,  ///< 알려두면 좋은 것
    Degraded,  ///< 근사해서 가져왔다. 결과가 원본과 다를 수 있다
    Dropped,   ///< 번역하지 못해 버렸다
};

/// 번역하지 못했거나 근사한 파라미터 한 건.
struct ImportNote {
    ImportSeverity severity = ImportSeverity::Info;
    /// 원본 포맷에서의 파라미터 이름 (예: "BrushTipShape/flipX", "effector_size").
    std::string sourceKey;
    /// 사용자에게 보여줄 한 줄 설명. 한국어로 쓴다.
    std::string message;
};

/// 브러시 하나를 임포트한 결과 보고서.
/// **비어 있지 않으면 UI가 반드시 사용자에게 보여준다.** 조용히 버리지 않는 게 설계 원칙이다.
struct ImportReport {
    /// 임포트한 원본 파일 경로.
    std::string sourcePath;
    /// 읽어낸 프리셋 수.
    usize presetCount = 0;
    /// 항목별 기록.
    std::vector<ImportNote> notes;

    void add(ImportSeverity sev, std::string key, std::string msg) {
        notes.push_back(ImportNote{sev, std::move(key), std::move(msg)});
    }
    /// 버려진 파라미터가 하나라도 있으면 true.
    [[nodiscard]] bool hasDropped() const {
        for (const auto& n : notes)
            if (n.severity == ImportSeverity::Dropped)
                return true;
        return false;
    }
    [[nodiscard]] bool clean() const { return notes.empty(); }
};

/// 임포터가 돌려주는 한 벌: 프리셋 목록 + 리포트.
struct ImportResult {
    std::vector<MariBrushPreset> presets;
    ImportReport report;
};

} // namespace mari::brush

#endif // MARI_BRUSH_PRESET_HPP
