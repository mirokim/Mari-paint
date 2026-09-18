// Mari Paint — 브러시 미리보기: 프리셋으로 S 곡선 획 하나를 실제 엔진으로 찍어 QImage 로
//
// 패널 썸네일과 편집기 미리보기가 같은 함수를 쓴다. 필압은 양 끝이 가는 taper-in-out.
#ifndef MARI_UI_BRUSH_PREVIEW_HPP
#define MARI_UI_BRUSH_PREVIEW_HPP

#include <mari/brush/preset.hpp>

#include <QColor>
#include <QImage>
#include <QSize>

namespace mari::ui {

/// `size` 픽셀의 미리보기. 팁 지름은 높이에 맞춰 잠깐 줄인다(fitDiameter=true).
[[nodiscard]] QImage renderBrushPreview(const brush::MariBrushPreset& preset, QSize size, QColor fg,
                                        QColor bg = QColor(255, 255, 255), bool fitDiameter = true,
                                        bool checker = false);

/// 팁 하나(도장)만 크게.
[[nodiscard]] QImage renderTipPreview(const brush::BrushTip& tip, int px, QColor fg);

} // namespace mari::ui

#endif // MARI_UI_BRUSH_PREVIEW_HPP
