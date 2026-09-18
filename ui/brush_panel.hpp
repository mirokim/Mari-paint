// Mari Paint — 브러시 패널(도크): 획 미리보기 격자 · 검색 · 편집/복제/삭제/가져오기
#ifndef MARI_UI_BRUSH_PANEL_HPP
#define MARI_UI_BRUSH_PANEL_HPP

#include <mari/brush/preset.hpp>

#include <QColor>
#include <QWidget>

#include <vector>

class QLineEdit;
class QListWidget;
class QListWidgetItem;

namespace mari::ui {

class BrushPanel final : public QWidget {
    Q_OBJECT
public:
    explicit BrushPanel(QWidget* parent = nullptr);

    /// 목록을 다시 만든다. 앞 `builtinCount` 개는 내장(편집은 "다른 이름으로" 만).
    void setBrushes(const std::vector<brush::MariBrushPreset>& brushes, usize builtinCount, int current);
    void setCurrent(int index);
    /// 미리보기 색(전경색이 바뀌면 다시 그린다 — 색 변화 브러시가 보이게).
    void setPreviewColor(QColor fg);

signals:
    void brushSelected(int index);
    void editRequested(int index);
    void duplicateRequested(int index);
    void deleteRequested(int index);
    void newRequested();
    void importRequested();

private:
    void rebuild();
    void applyFilter(const QString& text);
    [[nodiscard]] int indexOf(const QListWidgetItem* item) const;

    std::vector<brush::MariBrushPreset> brushes_;
    usize builtinCount_ = 0;
    int current_ = -1;
    QColor fg_ = Qt::black;
    QLineEdit* search_ = nullptr;
    QListWidget* list_ = nullptr;
    bool busy_ = false;
};

} // namespace mari::ui

#endif // MARI_UI_BRUSH_PANEL_HPP
