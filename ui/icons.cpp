// Mari Paint — 아이콘 구현 (ui/icons.hpp)
#include "icons.hpp"

#include <QFile>
#include <QGuiApplication>
#include <QPainter>
#include <QPixmap>
#include <QScreen>
#include <QSvgRenderer>

namespace mari::ui {

namespace {

QPixmap renderSvg(const QByteArray& svg, int logicalPx, const QColor& color, qreal dpr) {
    QByteArray tinted = svg;
    tinted.replace("#dcdcdc", color.name().toLatin1());
    QSvgRenderer r(tinted);
    QPixmap pm(QSize(logicalPx, logicalPx) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    r.render(&p, QRectF(0, 0, logicalPx, logicalPx));
    return pm;
}

qreal maxScreenDpr() {
    // 창이 어느 모니터로 가든 흐리지 않게, 가장 높은 배율로 한 번 굽는다. 2 KB 짜리라 값싸다.
    qreal dpr = 1.0;
    for (const QScreen* s : QGuiApplication::screens()) {
        dpr = std::max(dpr, s->devicePixelRatio());
    }
    return std::max(dpr, 2.0);
}

} // namespace

QIcon themedIcon(const QString& name, int px) {
    QFile f(":/icons/" + name + ".svg");
    if (!f.open(QIODevice::ReadOnly)) {
        return QIcon();
    }
    const QByteArray svg = f.readAll();
    const qreal dpr = maxScreenDpr();
    QIcon ic;
    ic.addPixmap(renderSvg(svg, px, QColor(0xdc, 0xdc, 0xdc), dpr), QIcon::Normal, QIcon::Off);
    ic.addPixmap(renderSvg(svg, px, Qt::white, dpr), QIcon::Normal, QIcon::On);   // 체크됨
    ic.addPixmap(renderSvg(svg, px, Qt::white, dpr), QIcon::Active, QIcon::Off);  // 호버
    ic.addPixmap(renderSvg(svg, px, Qt::white, dpr), QIcon::Active, QIcon::On);
    ic.addPixmap(renderSvg(svg, px, Qt::white, dpr), QIcon::Selected, QIcon::Off);
    ic.addPixmap(renderSvg(svg, px, Qt::white, dpr), QIcon::Selected, QIcon::On);
    ic.addPixmap(renderSvg(svg, px, QColor(0x6e, 0x6e, 0x6e), dpr), QIcon::Disabled, QIcon::Off);
    ic.addPixmap(renderSvg(svg, px, QColor(0x6e, 0x6e, 0x6e), dpr), QIcon::Disabled, QIcon::On);
    return ic;
}

} // namespace mari::ui
