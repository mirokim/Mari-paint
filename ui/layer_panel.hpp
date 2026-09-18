// Mari Paint — 레이어 패널: 트리(그룹) · 블렌드 · 불투명도 · 잠금/알파 잠금/클리핑 · 마스크 · 썸네일 · 드래그 재배열
//
// 트리는 위가 위다(인덱스 0 = 가장 아래인 core 규약을 화면에서만 뒤집는다).
// 구조 변경(추가·삭제·이동·그룹)은 core 트리에 바로 반영하고 refresh() 로 다시 그린다.
#ifndef MARI_UI_LAYER_PANEL_HPP
#define MARI_UI_LAYER_PANEL_HPP

#include <mari/app/document.hpp>

#include <QPixmap>
#include <QWidget>

class QComboBox;
class QSlider;
class QSpinBox;
class QToolButton;
class QTreeWidgetItem;

namespace mari::ui {

class LayerTreeWidget;

class LayerPanel final : public QWidget {
    Q_OBJECT
public:
    explicit LayerPanel(QWidget* parent = nullptr);

    void setDocument(app::Document* doc);
    /// 트리 전체를 다시 만든다(문서 교체·실행취소·구조 변경).
    void refresh();
    /// 한 레이어의 썸네일만 갱신한다(획이 끝났을 때).
    void refreshThumbnail(LayerId id);
    [[nodiscard]] bool busy() const noexcept { return busy_; }

    void addLayer();
    void addGroup();
    /// 활성 레이어를 새 그룹으로 감싼다(Ctrl+G).
    void groupActive();
    void duplicateLayer();
    void removeLayer();
    void moveActive(int delta); ///< +1 위로, -1 아래로
    void flattenAll();
    /// 활성 그룹을 풀어 자식을 제자리에 놓는다(Ctrl+Shift+G).
    void ungroupActive();
    /// 화면 순서(위→아래)로 한 칸 위/아래 레이어를 활성으로(Alt+] / Alt+[).
    void selectAdjacent(int delta);
    void toggleClipActive();

    // 레이어 마스크(app::layer_commands — 실행취소가 붙는다)
    void addMask(bool fromSelection);
    void applyMask();
    void removeMask();
    void paintMaskWithSelection(bool reveal);

signals:
    /// 픽셀에 영향을 주는 변경(가시성·불투명도·블렌드·삭제·이동). 캔버스를 다시 그려라.
    void layersChanged();
    void activeLayerChanged(mari::LayerId id);
    /// 아래와 병합 버튼/메뉴. 실행취소가 붙은 연산은 MainWindow(app::mergeLayerDown)가 한다.
    void mergeDownRequested();

private:
    void onCurrentChanged(QTreeWidgetItem* cur);
    void onItemChanged(QTreeWidgetItem* item);
    void onDropped();
    void syncControlsToActive();
    void showContextMenu(const QPoint& pos);
    [[nodiscard]] LayerPtr activeLayer() const;
    [[nodiscard]] QPixmap thumbnailFor(const Layer& l) const;
    void fillItem(QTreeWidgetItem& item, const Layer& l) const;
    void buildItems(QTreeWidgetItem* parent, const std::vector<LayerPtr>& list, LayerId active,
                    QTreeWidgetItem*& activeItem);
    [[nodiscard]] QTreeWidgetItem* itemFor(LayerId id) const;
    [[nodiscard]] static LayerId idOf(const QTreeWidgetItem* item);
    /// 활성 레이어의 부모와 core 인덱스(없으면 루트/-1).
    void locateActive(LayerId& parent, int& index) const;
    void markDirty();
    /// 실행취소 스택에 들어간 연산 뒤처리(다시 그리기 + 캔버스 갱신).
    void afterCommand(const Result<void>& r);

    app::Document* doc_ = nullptr;
    QComboBox* blend_ = nullptr;
    QSlider* opacity_ = nullptr;
    QSpinBox* opacitySpin_ = nullptr;
    QToolButton* lock_ = nullptr;
    QToolButton* alphaLock_ = nullptr;
    QToolButton* clip_ = nullptr;
    LayerTreeWidget* tree_ = nullptr;
    bool busy_ = false;
};

} // namespace mari::ui

#endif // MARI_UI_LAYER_PANEL_HPP
