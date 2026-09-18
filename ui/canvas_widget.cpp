// Mari Paint — 캔버스 위젯 구현 (ui/canvas_widget.hpp)
#include "canvas_widget.hpp"

#include <mari/core/compositor.hpp>
#include <mari/core/origin.hpp>
#include <mari/stroke/input.hpp>

#include <QCoreApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QWheelEvent>

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace mari::ui {

namespace {

constexpr f64 kMinZoom = 0.02;

bool traceOn() {
    static const bool on = qEnvironmentVariableIsSet("MARI_GUI_TRACE");
    return on;
}
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
    // ⚠️ WA_NativeWindow 를 주지 않는다 — 헤더 머리글 3번. 메시지는 최상위 창에서 필터로 받는다.
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_NoSystemBackground);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(64, 64);
    // 🔴 필터는 여기서 건다. showEvent 안에서 걸면 Qt 가 네이티브 메시지를 돌리며 필터 목록을
    //    순회하는 **도중에** 목록이 바뀌어 Qt6Widgets 안에서 죽는다(실측: 실행의 절반이 show() 에서
    //    AV). 생성자는 네이티브 이벤트 밖이다. attached_ 가 false 인 동안 필터는 아무것도 안 한다.
    QCoreApplication::instance()->installNativeEventFilter(this);
    if (traceOn()) {
        std::fprintf(stderr, "[mari-gui] sizeof(CanvasWidget) in canvas_widget.cpp = %zu (PointerInput %zu, QImage %zu)\n",
                     sizeof(CanvasWidget), sizeof(mari::win::PointerInput), sizeof(QImage));
        std::fflush(stderr);
    }
}

CanvasWidget::~CanvasWidget() {
    if (live_ != nullptr) {
        // 창이 닫히는 중이다. 획을 up 없이 버리지 않는다 — 기록에 끝을 남긴다.
        finishStroke(nullptr);
    }
    QCoreApplication::instance()->removeNativeEventFilter(this);
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
    // 🔴 premultiplied 로 둔다. straight alpha 를 QPainter 가 축소 샘플링하면 투명(0,0,0,0) 이
    //    섞여 갱신 사각형 가장자리에 검은 헤어라인이 생긴다(실측). 합성기는 straight 로 내므로
    //    compositePending() 이 더티 영역만 변환한다.
    backing_ = QImage(sz.width, sz.height, QImage::Format_ARGB32_Premultiplied);
    backing_.fill(Qt::transparent);
    straight_.clear();
    pendingComposite_ = Rect{0, 0, sz.width, sz.height};
    // 레이아웃 전(100×30)에 맞추면 캔버스가 구석에 작게 뜬다. 실제 크기가 오면 맞춘다.
    needFit_ = true;
    if (width() > 200 && height() > 200) {
        needFit_ = false;
        fitToView();
    }
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
    const usize stride = static_cast<usize>(r.width) * 4u;
    straight_.resize(stride * static_cast<usize>(r.height));
    const Result<void> ok = compositeArea(doc_->layers(), r, straight_.data(), stride);
    (void)ok; // 합성 실패는 화면이 안 바뀌는 것으로 드러난다. 핫 패스에서 던지지 않는다.
    // straight RGBA → premultiplied BGRA(ARGB32 리틀엔디언). 더티 영역만이다.
    for (i32 y = 0; y < r.height; ++y) {
        const u8* src = straight_.data() + static_cast<usize>(y) * stride;
        u8* dst = backing_.scanLine(r.y + y) + static_cast<usize>(r.x) * 4u;
        for (i32 x = 0; x < r.width; ++x, src += 4, dst += 4) {
            const u32 a = src[3];
            dst[0] = static_cast<u8>((src[2] * a + 127u) / 255u);
            dst[1] = static_cast<u8>((src[1] * a + 127u) / 255u);
            dst[2] = static_cast<u8>((src[0] * a + 127u) / 255u);
            dst[3] = static_cast<u8>(a);
        }
    }
}

// ── 그리기 ───────────────────────────────────────────────────────────────

f64 CanvasWidget::dpr() const {
    return devicePixelRatioF();
}

QPointF CanvasWidget::physOffset() const {
    const QPoint o = mapTo(window(), QPoint(0, 0));
    return QPointF(o) * dpr();
}

QRectF CanvasWidget::physRect() const {
    return QRectF(physOffset(), QSizeF(size()) * dpr());
}

void CanvasWidget::pushViewToPointer() {
    // PointerInput 은 최상위 창 클라이언트 물리 px 를 본다. 위젯 오프셋만큼 옮긴 사본을 준다.
    mari::win::ViewState st = view_.state();
    const QPointF o = physOffset();
    st.anchorScreen.x += static_cast<f32>(o.x());
    st.anchorScreen.y += static_cast<f32>(o.y());
    pointer_.setView(mari::win::ViewTransform(st));
}

QTransform CanvasWidget::canvasToWidget() const {
    // view_ 는 "캔버스 → 이 위젯의 물리 px" 다.
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
    // 정수 정렬 + 1px 여유: 소스 사각형이 소수면 bilinear 가 경계 밖 픽셀을 섞어 이음새가 보인다.
    const QRectF visibleCanvas = QRectF(t.inverted()
                                            .mapRect(QRectF(e->rect()))
                                            .toAlignedRect()
                                            .adjusted(-1, -1, 1, 1)
                                            .intersected(backing_.rect()));
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
    if (needFit_ && doc_ != nullptr && width() > 200 && height() > 200 && live_ == nullptr) {
        needFit_ = false;
        fitToView();
    } else if (attached_) {
        pushViewToPointer();
    }
}

void CanvasWidget::moveEvent(QMoveEvent* e) {
    QWidget::moveEvent(e);
    if (attached_) {
        pushViewToPointer();
    }
}

void CanvasWidget::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    if (!attached_) {
        // 최상위 창의 HWND. 이 위젯은 HWND 가 없다(머리글 3번).
        topHwnd_ = reinterpret_cast<HWND>(window()->winId());
        const Result<void> ok = pointer_.attach(topHwnd_, this);
        attached_ = ok.ok();
        if (traceOn()) {
            std::fprintf(stderr, "[mari-gui] pointer attach hwnd=%p ok=%d mouseInPointer=%d\n",
                         static_cast<void*>(topHwnd_), attached_ ? 1 : 0,
                         ::IsMouseInPointerEnabled() ? 1 : 0);
        }
        if (attached_) {
            pushViewToPointer();
        }
    }
}

// ── WM_POINTER ───────────────────────────────────────────────────────────

bool CanvasWidget::nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) {
    if (eventType != "windows_generic_MSG" || !attached_) {
        return false;
    }
    auto* msg = static_cast<MSG*>(message);
    if (traceOn() && msg->message >= WM_POINTERUPDATE && msg->message <= WM_POINTERCAPTURECHANGED) {
        static int n = 0;
        if (n < 12) {
            ++n;
            std::fprintf(stderr, "[mari-gui] WM_POINTER 0x%03X hwnd=%p (top=%p) id=%u\n", msg->message,
                         static_cast<void*>(msg->hwnd), static_cast<void*>(topHwnd_),
                         static_cast<unsigned>(GET_POINTERID_WPARAM(msg->wParam)));
        }
    }
    if (msg->hwnd != topHwnd_) {
        return false;
    }
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

    const u32 id = static_cast<u32>(GET_POINTERID_WPARAM(msg->wParam));
    if (msg->message == WM_POINTERDOWN) {
        // 캔버스 밖(툴바·도크)이거나 그리기 버튼이 아니면(가운데·오른쪽) Qt 의 것이다.
        // 🔴 down 을 넘겼으면 그 포인터의 update/up 도 같이 넘겨야 Qt 의 버튼 상태가 안 꼬인다.
        POINT pt{GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam)};
        ::ScreenToClient(topHwnd_, &pt);
        const bool inside = physRect().contains(QPointF(pt.x, pt.y));
        if (!inside || !IS_POINTER_FIRSTBUTTON_WPARAM(msg->wParam)) {
            bypassId_ = id;
            return false;
        }
    }
    if (bypassId_ != 0 && id == bypassId_ &&
        (msg->message == WM_POINTERUPDATE || msg->message == WM_POINTERUP)) {
        if (msg->message == WM_POINTERUP) {
            bypassId_ = 0;
        }
        return false;
    }

    bool handled = false;
    const LRESULT r = pointer_.handleMessage(msg->hwnd, msg->message, msg->wParam, msg->lParam, handled);
    if (handled) {
        // ⚠️ Qt 는 result 에 nullptr 을 넘기기도 한다(실측: 그리기 첫 WM_POINTERDOWN 에서 죽었다).
        if (result != nullptr) {
            *result = static_cast<qintptr>(r);
        }
        return true;
    }
    return false;
}

void CanvasWidget::onPointerDown(const mari::win::PointerSample& s) {
    if (traceOn()) {
        std::fprintf(stderr, "[mari-gui] onPointerDown canvas=(%.1f,%.1f) kind=%d\n", s.event.x, s.event.y,
                     static_cast<int>(s.kind));
    }
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
    pushViewToPointer();
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
