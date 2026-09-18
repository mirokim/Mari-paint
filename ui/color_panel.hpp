// Mari Paint — 색 패널: 색상환 · 전경/배경 · 최근 색 · HEX
#ifndef MARI_UI_COLOR_PANEL_HPP
#define MARI_UI_COLOR_PANEL_HPP

#include <QColor>
#include <QWidget>

#include <functional>
#include <vector>

class QLineEdit;
class QToolButton;

namespace mari::ui {

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

signals:
    void foregroundChanged(const QColor& c);

private:
    void rebuildSwatches();
    void updateFgBgButtons();

    ColorWheel* wheel_ = nullptr;
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
