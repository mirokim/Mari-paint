// Mari Paint — 색 패널의 다른 선택 방식들. 색상환(color_wheel.hpp)과 같은 신호 규약이다:
//   setColor()  밖에서 바뀐 색을 반영한다. 신호를 내지 않는다.
//   colorChanged(c)  사용자가 만졌을 때만.
//
// · ColorBox     : SV 사각형 + 세로 휴 막대 (포토샵 색 피커 꼴). 사각형이라 도크 폭을 꽉 쓴다.
// · ColorSliders : 채널 슬라이더 3개(RGB · HSV · HSL). 각 슬라이더 배경은 "그 채널만 바꾸면
//                  어떤 색이 되는가"를 그린다 — 나머지 두 채널을 따라 실시간으로 변한다.
#ifndef MARI_UI_COLOR_MODES_HPP
#define MARI_UI_COLOR_MODES_HPP

#include <QColor>
#include <QImage>
#include <QWidget>

#include <array>

class QSpinBox;

namespace mari::ui {

class ColorBox final : public QWidget {
    Q_OBJECT
public:
    explicit ColorBox(QWidget* parent = nullptr);
    [[nodiscard]] QColor color() const { return QColor::fromHsvF(h_, s_, v_); }
    void setColor(const QColor& c);
    [[nodiscard]] QSize sizeHint() const override { return {220, 180}; }

signals:
    void colorChanged(const QColor& c);

protected:
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;

private:
    enum class Drag { None, Square, Bar };
    void rebuildSquare();
    void handle(const QPointF& p);
    [[nodiscard]] QRectF squareRect() const;
    [[nodiscard]] QRectF barRect() const;

    qreal h_ = 0.0, s_ = 0.0, v_ = 0.0;
    QImage square_;
    Drag drag_ = Drag::None;
};

/// 채널 슬라이더 하나. 값은 0..max 정수(RGB 255 · H 359 · S/V/L 100).
class ChannelSlider final : public QWidget {
    Q_OBJECT
public:
    explicit ChannelSlider(QWidget* parent = nullptr);
    void setRange(int max) { max_ = max; }
    [[nodiscard]] int value() const { return value_; }
    void setValue(int v);
    /// 배경 그라디언트: t(0..1) → 색. 나머지 채널이 바뀌면 다시 준다.
    void setGradient(const std::array<QColor, 17>& stops);
    [[nodiscard]] QSize sizeHint() const override { return {160, 18}; }
    [[nodiscard]] QSize minimumSizeHint() const override { return {60, 14}; }

signals:
    void valueChanged(int v);

protected:
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;

private:
    void handle(qreal x);
    int value_ = 0;
    int max_ = 255;
    std::array<QColor, 17> stops_{};
};

class ColorSliders final : public QWidget {
    Q_OBJECT
public:
    enum class Space { RGB, HSV, HSL };
    explicit ColorSliders(Space space, QWidget* parent = nullptr);
    [[nodiscard]] QColor color() const { return color_; }
    void setColor(const QColor& c);

signals:
    void colorChanged(const QColor& c);

private:
    void syncFromColor();
    void syncFromSliders();
    void repaintGradients();
    [[nodiscard]] QColor compose(int a, int b, int c) const;

    Space space_;
    QColor color_{0, 0, 0};
    std::array<ChannelSlider*, 3> sliders_{};
    std::array<QSpinBox*, 3> spins_{};
    bool updating_ = false;
};

} // namespace mari::ui

#endif // MARI_UI_COLOR_MODES_HPP
