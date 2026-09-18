// Mari Paint — 내비게이터: 캔버스 축소본 + 보이는 영역 사각형. 클릭/드래그로 뷰를 옮긴다.
#ifndef MARI_UI_NAVIGATOR_HPP
#define MARI_UI_NAVIGATOR_HPP

#include <QImage>
#include <QWidget>

namespace mari::ui {

class CanvasWidget;

class Navigator final : public QWidget {
    Q_OBJECT
public:
    explicit Navigator(CanvasWidget* canvas, QWidget* parent = nullptr);
    /// 캔버스 픽셀이 바뀌었다(획 끝·실행취소). 축소본을 다시 만든다.
    void refreshImage();
    [[nodiscard]] QSize sizeHint() const override { return {260, 150}; }

protected:
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;

private:
    [[nodiscard]] QRectF imageRect() const;
    void moveViewTo(const QPointF& widgetPos);

    CanvasWidget* canvas_;
    QImage thumb_;
};

} // namespace mari::ui

#endif // MARI_UI_NAVIGATOR_HPP
