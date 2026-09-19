// Mari Paint — 색상환 (휴 링 + SV 사각형). Qt Widgets 만으로 그린다 — 추가 모듈 없음.
//
// QColorDialog(모달)는 그리기 흐름을 끊는다. 페인팅 앱은 전부 도크에 색상환을 둔다
// (Krita 고급 색 선택기 · SAI 색상환 · CSP 컬러 서클). 이건 그 최소형이다.
// SV 사각형 이미지는 휴가 바뀔 때만 다시 만든다(256×256, 1ms 미만).
#ifndef MARI_UI_COLOR_WHEEL_HPP
#define MARI_UI_COLOR_WHEEL_HPP

#include <QColor>
#include <QImage>
#include <QWidget>

namespace mari::ui {

class ColorWheel final : public QWidget {
    Q_OBJECT
public:
    explicit ColorWheel(QWidget* parent = nullptr);

    [[nodiscard]] QColor color() const { return QColor::fromHsvF(h_, s_, v_); }
    /// 밖에서 색이 바뀌었다(스포이드·교환). 신호를 내지 않는다.
    void setColor(const QColor& c);

    /// 정사각형을 강제하지 않는다 — 도크가 어떤 모양이든 안에서 min(w,h) 원을 가운데 그린다.
    [[nodiscard]] QSize sizeHint() const override { return {220, 220}; }

signals:
    void colorChanged(const QColor& c);

protected:
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    enum class Drag { None, Ring, Square };
    void rebuildSquare();
    void handle(const QPointF& p);
    [[nodiscard]] QRectF ringRect() const;
    [[nodiscard]] QRectF squareRect() const;

    qreal h_ = 0.0, s_ = 0.0, v_ = 0.0; ///< 0..1
    QImage square_;
    Drag drag_ = Drag::None;
};

} // namespace mari::ui

#endif // MARI_UI_COLOR_WHEEL_HPP
