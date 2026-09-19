// Mari Paint — 색 보정 대화상자(색조/채도 · 밝기/대비 · 레벨 · 곡선 · 문턱값 · 포스터화), 실시간 미리보기
//
// 미리보기 방식: 값이 바뀔 때마다 (직전 미리보기를 undo) → app::adjustLayer 를 다시 적용. 확인이면 그대로 두고,
// 취소면 undo. 그래서 미리보기도 진짜 코어 경로를 타고, 확인 뒤엔 실행취소 항목이 정확히 하나 남는다.
#ifndef MARI_UI_ADJUST_DIALOG_HPP
#define MARI_UI_ADJUST_DIALOG_HPP

#include <mari/app/document.hpp>
#include <mari/app/image_ops.hpp>

#include <QDialog>

#include <functional>
#include <vector>

class QSlider;
class QSpinBox;
class QDoubleSpinBox;
class QTimer;
class QCheckBox;
class QFormLayout;

namespace mari::ui {

/// 곡선 편집 위젯: 점을 끌어 옮기고, 클릭으로 추가, 더블클릭으로 삭제.
class CurveWidget final : public QWidget {
    Q_OBJECT
public:
    explicit CurveWidget(QWidget* parent = nullptr);
    [[nodiscard]] std::vector<app::CurvePt> points() const { return pts_; }
    void setPoints(std::vector<app::CurvePt> p);
    void reset();
signals:
    void changed();
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
private:
    [[nodiscard]] QPointF toWidget(const app::CurvePt& p) const;
    [[nodiscard]] app::CurvePt fromWidget(const QPointF& q) const;
    std::vector<app::CurvePt> pts_{{0.0f, 0.0f}, {1.0f, 1.0f}};
    int drag_ = -1;
};

class AdjustDialog final : public QDialog {
    Q_OBJECT
public:
    /// `applyFn` 은 (params) → 성공 여부. 미리보기와 확인이 같은 함수를 쓴다. `undoFn` 은 직전 미리보기를 되돌린다.
    AdjustDialog(app::AdjustKind kind, std::function<bool(const app::AdjustParams&)> applyFn,
                 std::function<void()> undoFn, QWidget* parent = nullptr);
    [[nodiscard]] const app::AdjustParams& params() const noexcept { return params_; }

protected:
    void reject() override;
    void accept() override;

private:
    void buildHsl(QWidget* page);
    void buildBrightness(QWidget* page);
    void buildLevels(QWidget* page);
    void buildCurves(QWidget* page);
    void buildThreshold(QWidget* page);
    void buildPosterize(QWidget* page);
    QSlider* sliderRow(QWidget* page, QFormLayout* form, const QString& label, int lo, int hi, int value,
                       QSpinBox** spinOut = nullptr);
    void schedule();
    void applyPreview();
    void readParams();

    app::AdjustParams params_;
    std::function<bool(const app::AdjustParams&)> apply_;
    std::function<void()> undo_;
    bool previewApplied_ = false;
    bool loading_ = false;
    QTimer* timer_ = nullptr;
    QCheckBox* previewOn_ = nullptr;

    QSlider *hue_ = nullptr, *sat_ = nullptr, *light_ = nullptr;
    QSlider *bright_ = nullptr, *contrast_ = nullptr;
    QSlider *inB_ = nullptr, *inW_ = nullptr, *outB_ = nullptr, *outW_ = nullptr;
    QDoubleSpinBox* gamma_ = nullptr;
    CurveWidget* curve_ = nullptr;
    QSlider* threshold_ = nullptr;
    QSlider* levels_ = nullptr;
};

} // namespace mari::ui

#endif // MARI_UI_ADJUST_DIALOG_HPP
