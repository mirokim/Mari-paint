// Mari Paint — 태블릿 설정: 전역 압력 곡선 + 테스터 (Krita 설정 › 태블릿의 최소형)
//
// 곡선은 5개 제어점의 꺾은선(0..1 → 0..1). 캔버스가 LUT(256)로 적용한다.
// 테스터는 마지막 펜 샘플(압력·기울기)과 펜→화면 지연 통계를 실시간으로 보여 준다.
#ifndef MARI_UI_TABLET_DIALOG_HPP
#define MARI_UI_TABLET_DIALOG_HPP

#include <QDialog>
#include <QPointF>

#include <array>
#include <vector>

class QLabel;

namespace mari::ui {

class CanvasWidget;

/// 압력 곡선 편집 위젯. 점을 끌어 옮긴다. 양 끝 점은 x 가 고정이다.
class PressureCurveWidget final : public QWidget {
    Q_OBJECT
public:
    explicit PressureCurveWidget(QWidget* parent = nullptr);
    [[nodiscard]] const std::vector<QPointF>& points() const { return pts_; }
    void setPoints(std::vector<QPointF> pts);
    void reset();
    [[nodiscard]] std::array<float, 256> lut() const;
    /// 테스터: 지금 압력을 곡선 위에 표시한다.
    void setLivePressure(float p);

signals:
    void curveChanged();

protected:
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;

private:
    [[nodiscard]] QRectF plotRect() const;
    [[nodiscard]] QPointF toWidget(const QPointF& p) const;
    [[nodiscard]] QPointF toCurve(const QPointF& w) const;
    [[nodiscard]] float evaluate(float x) const;

    std::vector<QPointF> pts_;
    int drag_ = -1;
    float live_ = -1.0f;
};

class TabletDialog final : public QDialog {
    Q_OBJECT
public:
    explicit TabletDialog(CanvasWidget* canvas, QWidget* parent = nullptr);

private:
    void refresh();
    CanvasWidget* canvas_;
    PressureCurveWidget* curve_ = nullptr;
    QLabel* readout_ = nullptr;
};

} // namespace mari::ui

#endif // MARI_UI_TABLET_DIALOG_HPP
