// Mari Paint — 캔버스 위젯 구현 (ui/canvas_widget.hpp)
#include "canvas_widget.hpp"

#include <mari/core/compositor.hpp>
#include <mari/core/origin.hpp>
#include <mari/stroke/input.hpp>

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace mari::ui {

namespace {

constexpr f64 kMinZoom = 0.02;
constexpr f64 kMaxZoom = 64.0;

QBrush checkerBrush() {
    // 투명 영역 표시. 캔버스 좌표계에 그리므로 줌을 따라 커진다 — 그게 의도다
    // (픽셀 하나가 얼마나 큰지 보인다).
    static const QBrush brush = [] {
        QImage img(16, 16, QImage::Format_RGB32);
        img.fill(QColor(200, 200, 200));
        QPainter p(&img);
        p.fillRect(0, 0, 8, 8, QColor(160, 160, 160));
        p.fillRect(8, 8, 8, 8, QColor(160, 160, 160));
        return QBrush(img);
    }();
    return brush;
}

} // namespace

CanvasWidget::CanvasWidget(QWidget* parent) : QWidget(parent) {
    // 🔴 이 위젯은 자기 HWND 를 가진다. 그래야 WM_POINTER 가 이 위젯의 nativeEvent 로 온다
    //    (기본값이면 최상위 창의 HWND 하나뿐이고 자식은 메시지를 못 본다).
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_NoSystemBackground);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(64, 64);
}

CanvasWidget::~CanvasWidget() {
    if (live_ != nullptr) {
        // 창이 닫히는 중이다. 획을 up 없이 버리지 않는다 — 기록에 끝을 남긴다.
        finishStroke(nullptr);
    }
    pointer_.detach();
}

// ── 문서 ─────────────────────────────────────────────────────────────────

void CanvasWidget::setDocument(app::Document* doc) {
    if (live_ != nullptr) {
        finishStroke(nullptr);
    }
    doc_ = doc;
    if (doc_ == nullptr) {
        backing_ = QImage();
        pendingComposite_ = Rect{};
        update();
        return;
    }
    const Size sz = doc_->canvasSize();
    backing_ = QImage(sz.width, sz.height, QImage::Format_RGBA8888);
    backing_.fill(Qt::transparent);
    pendingComposite_ = Rect{0, 0, sz.width, sz.height};
    fitToView();
    update();
}

void CanvasWidget::invalidateCanvas(const Rect& canvasRect) {
    if (doc_ == nullptr) {
        return;
    }
    const Size sz = doc_->canvasSize();
    const Rect r = canvasRect.isEmpty() ? Rect{0, 0, sz.width, sz.height} : canvasRect;
    scheduleCanvasRepaint(r, 0);
}

void CanvasWidget::scheduleCanvasRepaint(const Rect& canvasRect, u64 inputNs) {
    if (canvasRect.isEmpty()) {
        return;
    }
    pendingComposite_ = pendingComposite_.isEmpty() ? canvasRect : pendingComposite_.united(canvasRect);
    if (inputNs != 0 && (pendingInputNs_ == 0 || inputNs < pendingInputNs_)) {
        pendingInputNs_ = inputNs;
    }
    update(canvasToWidgetRect(canvasRect));
}

void CanvasWidget::compositePending() {
    if (doc_ == nullptr || pendingComposite_.isEmpty() || backing_.isNull()) {
        pendingComposite_ = Rect{};
        return;
    }
    const Rect canvas{0, 0, backing_.width(), backing_.height()};
    const Rect r = pendingComposite_.intersected(canvas);
    pendingComposite_ = Rect{};
    if (r.isEmpty()) {
        return;
    }
    // 더티 영역만 평탄화한다. 전체 재합성은 없다(docs/08 3.1).
    u8* dst = backing_.scanLine(r.y) + static_cast<usize>(r.x) * 4u;
    const Result<void> ok = compositeArea(doc_->layers(), r, dst,
                                          static_cast<usize>(backing_.bytesPerLine()));
    (void)ok; // 합성 실패는 화면이 안 바뀌는 것으로 드러난다. 핫 패스에서 던지지 않는다.
}

// ── 그리기 ───────────────────────────────────────────────────────────────

f64 CanvasWidget::dpr() const {
    return devicePixelRatioF();
}

QTransform CanvasWidget::canvasToWidget() const {
    // Affine2: x' = a x + b y + tx, y' = c x + d y + ty
    // QTransform(m11, m12, m21, m22, dx, dy): x' = m11 x + m21 y + dx, y' = m12 x + m22 y + dy
    const mari::win::Affine2& f = view_.canvasToScreen();
    const f64 s = 1.0 / dpr(); // 물리 px → 논리 px
    return QTransform(f.a, f.c, f.b, f.d, f.tx, f.ty) * QTransform::fromScale(s, s);
}

QRect CanvasWidget::canvasToWidgetRect(const Rect& r) const {
    const QRectF q(r.x, r.y, r.width, r.height);
    return canvasToWidget().mapRect(q).toAlignedRect().adjusted(-2, -2, 2, 2);
}

void CanvasWidget::paintEvent(QPaintEvent* e) {
    compositePending();

    QPainter p(this);
    p.fillRect(e->rect(), QColor(64, 64, 64));
    if (doc_ == nullptr || backing_.isNull()) {
        return;
    }

    const QTransform t = canvasToWidget();
    p.setTransform(t);
    p.setRenderHint(QPainter::SmoothPixmapTransform, view_.state().zoom < 1.0);

    // 화면의 갱신 영역만큼만 캔버스를 그린다 — 큰 캔버스에서 전체를 넘기지 않는다.
    const QRectF visibleCanvas =
        t.inverted().mapRect(QRectF(e->rect())).intersected(QRectF(backing_.rect()));
    if (visibleCanvas.isEmpty()) {
        return;
    }
    p.fillRect(visibleCanvas, checkerBrush());
    p.drawImage(visibleCanvas, backing_, visibleCanvas);
    p.end();

    // 펜→화면 지연: 이 프레임이 반영한 가장 이른 입력부터 지금까지.
    if (pendingInputNs_ != 0) {
        const u64 now = stroke::monotonicNowNs();
        const f64 ms = now > pendingInputNs_ ? static_cast<f64>(now - pendingInputNs_) / 1e6 : 0.0;
        pendingInputNs_ = 0;
        latency_.lastMs = ms;
        latency_.maxMs = std::max(latency_.maxMs, ms);
        latency_.avgMs += (ms - latency_.avgMs) / static_cast<f64>(latency_.samples + 1);
        ++latency_.samples;
        if (ms > 16.0) {
            ++latency_.over16;
        }
        emit latencyUpdated();
    }
}

void CanvasWidget::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
}

void CanvasWidget::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    if (!attached_) {
        auto hwnd = reinterpret_cast<HWND>(winId());
        const Result<void> ok = pointer_.attach(hwnd, this);
        attached_ = ok.ok();
        pointer_.setView(view_);
    }
}

// ── WM_POINTER ───────────────────────────────────────────────────────────

bool CanvasWidget::nativeEvent(const QByteArray& eventType, void* message, qintptr* result) {
    if (eventType != "windows_generic_MSG" || !attached_) {
        return false;
    }
    auto* msg = static_cast<MSG*>(message);
    switch (msg->message) {
    case WM_POINTERDOWN:
    case WM_POINTERUPDATE:
    case WM_POINTERUP:
    case WM_POINTERCAPTURECHANGED:
    case WM_POINTERLEAVE:
    case WM_KILLFOCUS:
    case WM_CANCELMODE:
        break;
    default:
        return false;
    }

    // 그리기 버튼이 아닌 것(가운데·오른쪽 버튼)은 Qt 에 넘겨 팬 등 UI 조작으로 쓴다.
    // 🔴 down 을 넘겼으면 그 포인터의 update/up 도 같이 넘겨야 Qt 의 버튼 상태가 안 꼬인다.
    static u32 bypassId = 0;
    const u32 id = static_cast<u32>(GET_POINTERID_WPARAM(msg->wParam));
    if (msg->message == WM_POINTERDOWN && !IS_POINTER_FIRSTBUTTON_WPARAM(msg->wParam)) {
        bypassId = id;
        return false;
    }
    if (bypassId != 0 && id == bypassId &&
        (msg->message == WM_POINTERUPDATE || msg->message == WM_POINTERUP)) {
        if (msg->message == WM_POINTERUP) {
            bypassId = 0;
        }
        return false;
    }

    bool handled = false;
    const LRESULT r = pointer_.handleMessage(msg->hwnd, msg->message, msg->wParam, msg->lParam, handled);
    if (handled) {
        *result = static_cast<qintptr>(r);
        return true;
    }
    return false;
}

void CanvasWidget::onPointerDown(const mari::win::PointerSample& s) {
    if (doc_ == nullptr) {
        return;
    }
    if (live_ != nullptr) {
        finishStroke(nullptr);
    }
    app::LiveStrokeConfig cfg = cfgProvider_ ? cfgProvider_() : app::LiveStrokeConfig{};
    if (s.eraser) {
        cfg.eraser = true; // 펜을 뒤집었다. 툴바 설정보다 우선한다
    }
    cfg.seed = (++strokeSeed_) ^ s.event.timestampNs;
    cfg.undoText = cfg.eraser ? "지우개" : "붓질";

    // 🔴 출처: 사람 펜. 팩토리가 준 것을 넘길 뿐이다. 여기서 만들지 않는다(core/origin.hpp).
    Result<std::unique_ptr<app::LiveStroke>> begun = app::LiveStroke::begin(
        *doc_, StrokeSource::humanPen(), doc_->layers().activeLayer(), cfg, s.event);
    if (!begun.ok()) {
        emit strokeRefused(QString::fromStdString(begun.message()));
        return;
    }
    live_ = std::move(begun).value();
    scheduleCanvasRepaint(live_->takeDisplayDirty(), s.event.timestampNs);
}

void CanvasWidget::onPointerMove(const mari::win::PointerSample& s) {
    if (live_ == nullptr) {
        return;
    }
    live_->extend(s.event);
    scheduleCanvasRepaint(live_->takeDisplayDirty(), s.event.timestampNs);
}

void CanvasWidget::onPointerUp(const mari::win::PointerSample& s) {
    finishStroke(&s.event);
}

void CanvasWidget::onPointerCancel(u32 /*pointerId*/) {
    finishStroke(nullptr);
}

void CanvasWidget::finishStroke(const stroke::RawInputEvent* last) {
    if (live_ == nullptr) {
        return;
    }
    const u64 inputNs = last != nullptr ? last->timestampNs : 0;
    Result<app::LiveStrokeOutcome> ended = live_->end(last);
    Rect dirty = live_->takeDisplayDirty();
    live_.reset();
    if (!ended.ok()) {
        emit strokeRefused(QString::fromStdString(ended.message()));
        invalidateCanvas();
        return;
    }
    if (ended.value().rolledBack) {
        // 되돌렸다 — 픽셀이 바뀌었다가 원래대로 갔으니 그 범위를 다시 그린다.
        dirty = dirty.isEmpty() ? ended.value().dirtyBounds : dirty.united(ended.value().dirtyBounds);
    }
    scheduleCanvasRepaint(dirty, inputNs);
    emit strokeFinished(ended.value());
}

// ── 뷰 ───────────────────────────────────────────────────────────────────

void CanvasWidget::applyView() {
    // 🔴 획 중간에는 뷰를 바꾸지 않는다 — 같은 획이 두 좌표계에 걸친다(pointer_input.hpp).
    if (live_ != nullptr) {
        return;
    }
    pointer_.setView(view_);
    update();
    emit viewChanged();
}

void CanvasWidget::zoomBy(f64 factor, QPointF logicalCenter) {
    if (live_ != nullptr) {
        return;
    }
    mari::win::ViewState st = view_.state();
    const f64 s = dpr();
    const PointF screen{static_cast<f32>(logicalCenter.x() * s), static_cast<f32>(logicalCenter.y() * s)};
    st.anchorCanvas = view_.toCanvas(screen);
    st.anchorScreen = screen;
    st.zoom = std::clamp(st.zoom * factor, kMinZoom, kMaxZoom);
    view_.setState(st);
    applyView();
}

void CanvasWidget::rotateBy(f64 degrees) {
    if (live_ != nullptr) {
        return;
    }
    mari::win::ViewState st = view_.state();
    const f64 s = dpr();
    const QPointF c = QRectF(rect()).center();
    const PointF screen{static_cast<f32>(c.x() * s), static_cast<f32>(c.y() * s)};
    st.anchorCanvas = view_.toCanvas(screen);
    st.anchorScreen = screen;
    st.rotationDeg = std::fmod(st.rotationDeg + degrees + 360.0, 360.0);
    view_.setState(st);
    applyView();
}

void CanvasWidget::panBy(QPointF logicalDelta) {
    if (live_ != nullptr) {
        return;
    }
    mari::win::ViewState st = view_.state();
    const f64 s = dpr();
    st.anchorScreen.x += static_cast<f32>(logicalDelta.x() * s);
    st.anchorScreen.y += static_cast<f32>(logicalDelta.y() * s);
    view_.setState(st);
    applyView();
}

void CanvasWidget::resetView() {
    if (live_ != nullptr || doc_ == nullptr) {
        return;
    }
    const Size sz = doc_->canvasSize();
    const f64 s = dpr();
    mari::win::ViewState st;
    st.anchorCanvas = PointF{static_cast<f32>(sz.width) * 0.5f, static_cast<f32>(sz.height) * 0.5f};
    st.anchorScreen = PointF{static_cast<f32>(width() * s * 0.5), static_cast<f32>(height() * s * 0.5)};
    view_.setState(st);
    applyView();
}

void CanvasWidget::fitToView() {
    if (live_ != nullptr || doc_ == nullptr) {
        return;
    }
    const Size sz = doc_->canvasSize();
    const f64 s = dpr();
    const f64 pw = std::max(1, width()) * s;
    const f64 ph = std::max(1, height()) * s;
    mari::win::ViewState st;
    st.zoom = std::clamp(std::min(pw / std::max(1, sz.width), ph / std::max(1, sz.height)) * 0.95,
                         kMinZoom, kMaxZoom);
    st.anchorCanvas = PointF{static_cast<f32>(sz.width) * 0.5f, static_cast<f32>(sz.height) * 0.5f};
    st.anchorScreen = PointF{static_cast<f32>(pw * 0.5), static_cast<f32>(ph * 0.5)};
    view_.setState(st);
    applyView();
}

// ── 마우스·키보드 (그리기가 아닌 조작만) ────────────────────────────────
//
// 🔴 그리기는 여기로 오지 않는다. 왼쪽 버튼·펜 접촉은 nativeEvent 가 먼저 먹는다.

void CanvasWidget::wheelEvent(QWheelEvent* e) {
    const f64 steps = e->angleDelta().y() / 120.0;
    if (steps != 0.0) {
        zoomBy(std::pow(1.25, steps), e->position());
    }
    e->accept();
}

void CanvasWidget::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::MiddleButton) {
        panning_ = true;
        panLast_ = e->position();
        setCursor(Qt::ClosedHandCursor);
        e->accept();
        return;
    }
    QWidget::mousePressEvent(e);
}

void CanvasWidget::mouseMoveEvent(QMouseEvent* e) {
    if (panning_) {
        panBy(e->position() - panLast_);
        panLast_ = e->position();
        e->accept();
        return;
    }
    QWidget::mouseMoveEvent(e);
}

void CanvasWidget::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() == Qt::MiddleButton && panning_) {
        panning_ = false;
        unsetCursor();
        e->accept();
        return;
    }
    QWidget::mouseReleaseEvent(e);
}

void CanvasWidget::keyPressEvent(QKeyEvent* e) {
    switch (e->key()) {
    case Qt::Key_BracketLeft: rotateBy(-15.0); break;
    case Qt::Key_BracketRight: rotateBy(15.0); break;
    case Qt::Key_0: resetView(); break;
    case Qt::Key_F: fitToView(); break;
    case Qt::Key_Plus:
    case Qt::Key_Equal: zoomBy(1.25, QRectF(rect()).center()); break;
    case Qt::Key_Minus: zoomBy(0.8, QRectF(rect()).center()); break;
    default: QWidget::keyPressEvent(e); return;
    }
    e->accept();
}

} // namespace mari::ui
