// Mari Paint — 레이어 패널: 블렌드 · 불투명도 · 잠금/알파 잠금 · 썸네일 · 드래그 재배열 · 이름 편집
//
// 목록은 위가 위다(인덱스 0 = 가장 아래인 core 규약을 화면에서만 뒤집는다).
#ifndef MARI_UI_LAYER_PANEL_HPP
#define MARI_UI_LAYER_PANEL_HPP

#include <mari/app/document.hpp>

#include <QWidget>

class QComboBox;
class QListWidget;
class QListWidgetItem;
class QSlider;
class QToolButton;

namespace mari::ui {

class LayerPanel final : public QWidget {
    Q_OBJECT
public:
    explicit LayerPanel(QWidget* parent = nullptr);

    void setDocument(app::Document* doc);
    /// 목록 전체를 다시 만든다(문서 교체·실행취소·구조 변경).
    void refresh();
    /// 한 레이어의 썸네일만 갱신한다(획이 끝났을 때).
    void refreshThumbnail(LayerId id);
    [[nodiscard]] bool busy() const noexcept { return busy_; }

    void addLayer();
    void removeLayer();
    void moveActive(int delta); ///< +1 위로, -1 아래로

signals:
    /// 픽셀에 영향을 주는 변경(가시성·불투명도·블렌드·삭제·이동). 캔버스를 다시 그려라.
    void layersChanged();
    void activeLayerChanged(mari::LayerId id);

private:
    void onRowChanged(int row);
    void onItemChanged(QListWidgetItem* item);
    void onRowsMoved();
    void syncControlsToActive();
    [[nodiscard]] LayerPtr activeLayer() const;
    [[nodiscard]] QIcon thumbnailFor(const Layer& l) const;
    void markDirty();

    app::Document* doc_ = nullptr;
    QComboBox* blend_ = nullptr;
    QSlider* opacity_ = nullptr;
    QToolButton* lock_ = nullptr;
    QToolButton* alphaLock_ = nullptr;
    QListWidget* list_ = nullptr;
    bool busy_ = false;
};

} // namespace mari::ui

#endif // MARI_UI_LAYER_PANEL_HPP
