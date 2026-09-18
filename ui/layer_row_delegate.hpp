// Mari Paint — 레이어 행 그리기: [눈] [썸네일 48×36] [이름 …] [🔒][α]
//
// QListWidget 은 그대로 두고(드래그 재배열이 공짜다) 행 그리기와 눈 클릭만 여기서 맡는다.
// 데이터 역할: DecorationRole = 썸네일 QPixmap, kVisibleRole/kLockedRole/kAlphaRole = bool.
#ifndef MARI_UI_LAYER_ROW_DELEGATE_HPP
#define MARI_UI_LAYER_ROW_DELEGATE_HPP

#include <QIcon>
#include <QStyledItemDelegate>

namespace mari::ui {

constexpr int kLayerIdRole = Qt::UserRole;
constexpr int kVisibleRole = Qt::UserRole + 1;
constexpr int kLockedRole = Qt::UserRole + 2;
constexpr int kAlphaRole = Qt::UserRole + 3;
constexpr int kClipRole = Qt::UserRole + 4;
constexpr int kLayerRowHeight = 44;

class LayerRowDelegate final : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit LayerRowDelegate(QObject* parent = nullptr);

    [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem& o, const QModelIndex& i) const override;
    void paint(QPainter* p, const QStyleOptionViewItem& o, const QModelIndex& i) const override;
    bool editorEvent(QEvent* e, QAbstractItemModel* m, const QStyleOptionViewItem& o,
                     const QModelIndex& i) override;
    void updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& o,
                              const QModelIndex& i) const override;

signals:
    /// 눈 아이콘을 눌렀다. 패널이 레이어 가시성을 바꾸고 다시 그린다.
    void visibilityToggled(const QModelIndex& index);

private:
    [[nodiscard]] static QRect eyeRect(const QRect& r);
    [[nodiscard]] static QRect thumbRect(const QRect& r);
    [[nodiscard]] static QRect nameRect(const QRect& r);

    QIcon eye_, eyeOff_, lock_, alpha_, clip_;
};

} // namespace mari::ui

#endif // MARI_UI_LAYER_ROW_DELEGATE_HPP
