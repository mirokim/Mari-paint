// Mari Paint — 이미지 연산: 색 보정 · 자유 변형 · 캔버스 연산
//
// GUI(이미지/편집 메뉴)와 에이전트(`adjust` · `transform` · `canvas.*`)가 **같은 함수**를 부른다.
// 전부 실행취소가 붙고(paintPixels 경로), 영역 직접쓰기로 기록된다(docs/06 결정 ①: 붓질 수에 섞이지 않는다).
// 선택이 있으면 선택 안만 바뀐다(가장자리는 선택값으로 섞는다).
#ifndef MARI_APP_IMAGE_OPS_HPP
#define MARI_APP_IMAGE_OPS_HPP

#include <mari/app/document.hpp>
#include <mari/core/origin.hpp>
#include <mari/ora/image.hpp>

#include <vector>

namespace mari::app {

// ── 색 보정 ───────────────────────────────────────────────────────────────

enum class AdjustKind : u8 {
    HueSaturation = 0, ///< hue(−180..180) · saturation(−100..100) · lightness(−100..100)
    BrightnessContrast, ///< brightness(−100..100) · contrast(−100..100)
    Levels,             ///< inBlack/inWhite(0..255) · gamma(0.1..10) · outBlack/outWhite(0..255)
    Curves,             ///< curve: (x,y) 0..1 점들(마스터). 채널별은 curveR/G/B(비면 마스터만)
    Invert,
    Desaturate,
    Threshold,          ///< threshold 0..255
    Posterize,          ///< levels 2..255
};

struct CurvePt {
    f32 x = 0.0f, y = 0.0f;
};

struct AdjustParams {
    AdjustKind kind = AdjustKind::HueSaturation;
    f32 hue = 0.0f, saturation = 0.0f, lightness = 0.0f;
    f32 brightness = 0.0f, contrast = 0.0f;
    i32 inBlack = 0, inWhite = 255, outBlack = 0, outWhite = 255;
    f32 gamma = 1.0f;
    std::vector<CurvePt> curve, curveR, curveG, curveB;
    i32 threshold = 128;
    i32 levels = 4;
};

/// 색 보정을 RGBA 버퍼에 그 자리에서 적용한다(알파는 건드리지 않는다). 미리보기·테스트용.
void applyAdjust(const AdjustParams& p, u8* rgba, usize pixelCount);

/// 레이어(선택 안)에 색 보정을 적용한다. 실행취소·기록 포함. 돌려주는 값 = 바뀐 타일 수.
[[nodiscard]] Result<u32> adjustLayer(Document& doc, const StrokeSource& src, LayerId layerId,
                                      const AdjustParams& p);

// ── 자유 변형 ─────────────────────────────────────────────────────────────

struct TransformParams {
    f64 dx = 0.0, dy = 0.0;       ///< 이동(px)
    f64 scaleX = 1.0, scaleY = 1.0;
    f64 rotateDeg = 0.0;          ///< 시계 방향(화면 좌표)
    f64 pivotX = 0.0, pivotY = 0.0; ///< 회전·확대 중심(캔버스 좌표). usePivot=false 면 원본 중심
    bool usePivot = false;
    bool flipH = false, flipV = false;
    bool bilinear = true;
};

/// 변형 대상(선택 안·알파>0 로 조인 영역)의 픽셀을 꺼낸다(선택값을 알파에 곱한 것). GUI 미리보기용.
[[nodiscard]] Result<ora::Image8> extractForTransform(Document& doc, LayerId layerId, Rect& outArea);
/// 같은 영역에서 "남는" 픽셀(선택 밖 부분). 미리보기 동안 잘라 낸 자리에 놓는다.
[[nodiscard]] ora::Image8 remainderForTransform(Document& doc, LayerId layerId, const Rect& area);

/// 레이어(선택이 있으면 선택 안 픽셀만)를 변형한다. 선택 밖은 그대로, 선택 안 원본은 지워진다(잘라 붙이기).
/// 돌려주는 값 = 바뀐 영역(원본 ∪ 결과).
[[nodiscard]] Result<Rect> transformLayer(Document& doc, const StrokeSource& src, LayerId layerId,
                                          const TransformParams& p);

// ── 캔버스 연산 (모든 레이어) ─────────────────────────────────────────────

/// 캔버스 전체를 좌우/상하로 뒤집는다(모든 래스터 레이어, 실행취소 하나).
[[nodiscard]] Result<void> flipCanvas(Document& doc, const StrokeSource& src, bool horizontal);
/// 캔버스를 90°(시계) · 180° · 270° 돌린다.
[[nodiscard]] Result<void> rotateCanvas(Document& doc, const StrokeSource& src, int quarterTurns);
/// 캔버스 크기를 바꾼다(내용은 anchor 기준으로 놓는다: 0..2 × 0..2, 1,1 = 가운데). 픽셀 재샘플 없음.
[[nodiscard]] Result<void> resizeCanvas(Document& doc, const StrokeSource& src, Size newSize, int anchorX,
                                        int anchorY);
/// 캔버스를 사각형으로 자른다(선택 경계 등).
[[nodiscard]] Result<void> cropCanvas(Document& doc, const StrokeSource& src, const Rect& area);
/// 이미지 크기 조절(모든 레이어 재샘플, 쌍선형).
[[nodiscard]] Result<void> scaleImage(Document& doc, const StrokeSource& src, Size newSize);

} // namespace mari::app

#endif // MARI_APP_IMAGE_OPS_HPP
