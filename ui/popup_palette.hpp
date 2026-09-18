// Mari Paint — 온캔버스 팝업 팔레트 (우클릭 · 펜 배럴 버튼). Krita 팝업 팔레트의 최소형.
//
// 붓 프리셋 · 작은 색상환 · 최근 색. 커서 자리에 뜨고, 밖을 누르거나 고르면 사라진다.
#ifndef MARI_UI_POPUP_PALETTE_HPP
#define MARI_UI_POPUP_PALETTE_HPP

#include <mari/brush/preset.hpp>

#include <QColor>
#include <QWidget>

#include <vector>

class QGridLayout;

namespace mari::ui {

class ColorWheel;

class PopupPalette final : public QWidget {
    Q_OBJECT
public:
    explicit PopupPalette(QWidget* parent = nullptr);

    void setBrushes(const std::vector<brush::MariBrushPreset>& brushes, int current);
    void setRecentColors(const std::vector<QColor>& colors);
    void setColor(const QColor& c);
    /// 화면 좌표(전역)에 중심을 맞춰 띄운다.
    void popupAt(const QPoint& globalCenter);

signals:
    void brushChosen(int index);
    void colorChosen(const QColor& c);

protected:
    void paintEvent(QPaintEvent* e) override;

private:
    ColorWheel* wheel_ = nullptr;
    QWidget* brushBox_ = nullptr;
    QWidget* recentBox_ = nullptr;
};

} // namespace mari::ui

#endif // MARI_UI_POPUP_PALETTE_HPP
