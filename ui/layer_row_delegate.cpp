// Mari Paint — 레이어 행 위임 구현 (ui/layer_row_delegate.hpp)
#include "layer_row_delegate.hpp"

#include "icons.hpp"

#include <QApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>

namespace mari::ui {

namespace {
constexpr int kEye = 16;
constexpr int kThumbW = 48;
constexpr int kThumbH = 36;
constexpr int kMark = 14;
} // namespace

LayerRowDelegate::LayerRowDelegate(QObject* parent)
    : QStyledItemDelegate(parent),
      eye_(themedIcon("eye", kEye)),
      eyeOff_(themedIcon("eye-off", kEye)),
      lock_(themedIcon("lock", kMark)),
      alpha_(themedIcon("square-half", kMark)),
      clip_(themedIcon("arrow-bar-to-down", kMark)) {}

QRect LayerRowDelegate::eyeRect(const QRect& r) {
    return QRect(r.left() + 6, r.center().y() - kEye / 2, kEye, kEye);
}
QRect LayerRowDelegate::thumbRect(const QRect& r) {
    return QRect(r.left() + 30, r.center().y() - kThumbH / 2, kThumbW, kThumbH);
}
QRect LayerRowDelegate::nameRect(const QRect& r) {
    return QRect(r.left() + 86, r.top(), r.width() - 86 - 44, r.height());
}

QSize LayerRowDelegate::sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const {
    return {0, kLayerRowHeight};
}

void LayerRowDelegate::paint(QPainter* p, const QStyleOptionViewItem& o, const QModelIndex& i) const {
    const bool sel = o.state & QStyle::State_Selected;
    const QRect r = o.rect;
    p->fillRect(r, sel ? QColor(0x3d, 0x7b, 0xd9) : (i.row() % 2 ? QColor(0x2b, 0x2b, 0x2b) : QColor(0x2e, 0x2e, 0x2e)));

    const bool visible = i.data(kVisibleRole).toBool();
    (visible ? eye_ : eyeOff_).paint(p, eyeRect(r), Qt::AlignCenter, visible ? QIcon::Normal : QIcon::Disabled);

    // 클립 레이어는 들여쓰고(CSP 관례) 왼쪽에 클립 화살표.
    const bool clipped = i.data(kClipRole).toBool();
    QRect tr = thumbRect(r);
    if (clipped) {
        clip_.paint(p, QRect(tr.left(), r.center().y() - kMark / 2, kMark, kMark));
        tr.translate(kMark + 2, 0);
    }
    const QPixmap thumb = i.data(Qt::DecorationRole).value<QPixmap>();
    if (!thumb.isNull()) {
        p->drawPixmap(tr, thumb);
    }
    p->setPen(QColor(0, 0, 0, 150));
    p->drawRect(tr.adjusted(0, 0, -1, -1));

    p->setPen(sel ? Qt::white : QColor(0xdc, 0xdc, 0xdc));
    const QString name = o.fontMetrics.elidedText(i.data(Qt::DisplayRole).toString(), Qt::ElideRight,
                                                  nameRect(r).width());
    p->drawText(nameRect(r).adjusted(clipped ? kMark + 2 : 0, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, name);

    int x = r.right() - 8 - kMark;
    if (i.data(kAlphaRole).toBool()) {
        alpha_.paint(p, QRect(x, r.center().y() - kMark / 2, kMark, kMark));
        x -= kMark + 4;
    }
    if (i.data(kLockedRole).toBool()) {
        lock_.paint(p, QRect(x, r.center().y() - kMark / 2, kMark, kMark));
    }
}

bool LayerRowDelegate::editorEvent(QEvent* e, QAbstractItemModel* m, const QStyleOptionViewItem& o,
                                   const QModelIndex& i) {
    if (e->type() == QEvent::MouseButtonRelease) {
        auto* me = static_cast<QMouseEvent*>(e);
        if (me->button() == Qt::LeftButton && eyeRect(o.rect).adjusted(-4, -4, 4, 4).contains(me->pos())) {
            Q_EMIT visibilityToggled(i);
            return true;
        }
    }
    if (e->type() == QEvent::MouseButtonPress) {
        auto* me = static_cast<QMouseEvent*>(e);
        if (me->button() == Qt::LeftButton && eyeRect(o.rect).adjusted(-4, -4, 4, 4).contains(me->pos())) {
            return true; // 눈 클릭이 선택을 바꾸지 않게 한다
        }
    }
    return QStyledItemDelegate::editorEvent(e, m, o, i);
}

void LayerRowDelegate::updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& o,
                                            const QModelIndex&) const {
    // 이름 편집기는 이름 자리에만 뜬다(행 전체를 덮지 않는다).
    const QRect nr = nameRect(o.rect);
    editor->setGeometry(QRect(nr.left() - 2, nr.top() + 10, nr.width() + 2, nr.height() - 20));
}

} // namespace mari::ui
