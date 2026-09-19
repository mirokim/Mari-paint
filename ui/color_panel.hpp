// Mari Paint — 색 패널: 색상환 · 전경/배경 · 최근 색 · HEX
#ifndef MARI_UI_COLOR_PANEL_HPP
#define MARI_UI_COLOR_PANEL_HPP

#include <QColor>
#include <QWidget>

#include <functional>
#include <vector>

class QComboBox;
class QLineEdit;
class QStackedWidget;
class QToolButton;

namespace mari::ui {

class ColorBox;
class ColorSliders;
class ColorWheel;
class FgBgSwatch;

class ColorPanel final : public QWidget {
    Q_OBJECT
public:
    explicit ColorPanel(QWidget* parent = nullptr);

    [[nodiscard]] QColor foreground() const { return fg_; }
    [[nodiscard]] QColor background() const { return bg_; }
    /// 전경색을 바꾼다(스포이드 등). 신호를 낸다.
    void setForeground(const QColor& c);
    /// X — 전경↔배경. D — 검정/흰색.
    void swap();
    void resetDefaults();
    /// 획이 끝났을 때 부른다 — 최근 색 목록에 넣는다.
    void noteUsed(const QColor& c);
    [[nodiscard]] const std::vector<QColor>& recentColors() const { return recent_; }

signals:
    void foregroundChanged(const QColor& c);

private:
    void rebuildSwatches();
    void updateFgBgButtons();

    /// 선택 방식. 콤보 순서 = 이 순서. QSettings "color/mode" 에 남긴다.
    enum class Mode { Wheel = 0, Box, RGB, HSV, HSL };
    void setMode(Mode m);
    void pushToPickers();

    QComboBox* modeCombo_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    ColorWheel* wheel_ = nullptr;
    ColorBox* box_ = nullptr;
    ColorSliders* rgb_ = nullptr;
    ColorSliders* hsv_ = nullptr;
    ColorSliders* hsl_ = nullptr;
    FgBgSwatch* swatch_ = nullptr;
    QLineEdit* hex_ = nullptr;
    QWidget* recentBox_ = nullptr;
    std::vector<QColor> recent_;
    QColor fg_{0, 0, 0};
    QColor bg_{255, 255, 255};
    bool updating_ = false;
};

} // namespace mari::ui

#endif // MARI_UI_COLOR_PANEL_HPP
