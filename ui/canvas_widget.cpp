#define _USE_MATH_DEFINES
// Mari Paint — 캔버스 위젯 구현 (ui/canvas_widget.hpp)
#include "canvas_widget.hpp"

#include <mari/core/compositor.hpp>
#include <mari/core/origin.hpp>
#include <mari/core/undo.hpp>
#include <mari/stroke/input.hpp>

#include <QCoreApplication>
#include <QCursor>
#include <QFont>
#include <QEnterEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QLineF>
#include <QPainter>
#include <QPolygonF>
#include <QTimer>
#include <QBitmap>
#include <QRegion>

#include <mari/app/image_ops.hpp>
#include <mari/app/layer_commands.hpp>
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
    airbrushTimer_ = new QTimer(this);
    airbrushTimer_->setInterval(30);
    connect(airbrushTimer_, &QTimer::timeout, this, [this] {
        if (live_ == nullptr || !live_->airbrush()) return;
        const u64 now = stroke::monotonicNowNs();
        if (now - lastMoveNs_ < 40'000'000ull) return; // 움직이는 동안은 보간기가 찍는다
        const f64 tMs = static_cast<f64>(now - strokeStartNs_) / 1e6;
        live_->hold(tMs);
        scheduleCanvasRepaint(live_->takeDisplayDirty(), now);
        for (auto& m : mirrors_) { m->hold(tMs); scheduleCanvasRepaint(m->takeDisplayDirty(), now); }
    });

    // ⚠️ WA_NativeWindow 를 주지 않는다 — 헤더 머리글 3번. 메시지는 최상위 창에서 필터로 받는다.
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_NoSystemBackground);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(64, 64);
    setMouseTracking(true);  // 호버 윤곽
    setCursor(Qt::CrossCursor);
    // 🔴 필터는 여기서 건다. showEvent 안에서 걸면 Qt 가 네이티브 메시지를 돌리며 필터 목록을
    //    순회하는 **도중에** 목록이 바뀌어 Qt6Widgets 안에서 죽는다(실측: 실행의 절반이 show() 에서
    //    AV). 생성자는 네이티브 이벤트 밖이다. attached_ 가 false 인 동안 필터는 아무것도 안 한다.
    QCoreApplication::instance()->installNativeEventFilter(this);
    antsTimer_ = new QTimer(this);
    antsTimer_->setInterval(120);
    connect(antsTimer_, &QTimer::timeout, this, [this] {
        antsPhase_ = (antsPhase_ + 1) % 8;
        if (hasSelection_) {
            // 점선만 다시 그린다 — 경계 상자 영역(캐시 블릿이라 값싸다).
            update(canvasToWidget().mapRect(selOutline_.boundingRect()).toAlignedRect().adjusted(-2, -2, 2, 2));
        }
    });
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
        invalidateView();
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
    rebuildSelectionOutline();
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
    // 캔버스 크기가 바뀌었으면(캔버스 연산·그 실행취소) 백킹을 새로 잡는다 — 뷰(줌·위치)는 건드리지 않는다.
    if (backing_.width() != sz.width || backing_.height() != sz.height) {
        backing_ = QImage(sz.width, sz.height, QImage::Format_ARGB32_Premultiplied);
        backing_.fill(Qt::transparent);
        pendingComposite_ = Rect{0, 0, sz.width, sz.height};
        rebuildSelectionOutline();
        invalidateView(); // 캔버스 테두리 자체가 바뀌었다 — 뷰 캐시 전체를 다시
        return;
    }
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
    const QRect wr = canvasToWidgetRect(canvasRect);
    viewDirty_ += wr;
    update(wr);
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

void CanvasWidget::invalidateView() {
    viewDirty_ = QRegion(rect());
    update();
}

void CanvasWidget::renderViewCache() {
    const f64 s = dpr();
    const QSize phys(static_cast<int>(std::lround(width() * s)), static_cast<int>(std::lround(height() * s)));
    if (viewCache_.size() != phys) {
        viewCache_ = QImage(phys, QImage::Format_ARGB32_Premultiplied);
        viewCache_.setDevicePixelRatio(s);
        viewDirty_ = QRegion(rect());
    }
    if (viewDirty_.isEmpty()) {
        return;
    }
    QPainter p(&viewCache_);
    p.setClipRegion(viewDirty_);
    const QRect bounds = viewDirty_.boundingRect();
    p.fillRect(bounds, QColor(64, 64, 64));
    if (doc_ != nullptr && !backing_.isNull()) {
        const QTransform t = canvasToWidget();
        p.setTransform(t);
        // 정수 배율(100·200·300%…)만 픽셀을 그대로 찍는다. 137% 같은 어중간한 배율을 최근접으로
        // 찍으면 픽셀이 1칸·2칸 섞여 선이 울퉁불퉁 찌그러져 보인다(실측). 축소도 마찬가지로 보간.
        const f64 z = view_.state().zoom;
        const bool integerZoom = z >= 1.0 && std::abs(z - std::round(z)) < 1e-3;
        p.setRenderHint(QPainter::SmoothPixmapTransform, !integerZoom);
        // 더러운 영역만큼만 캔버스를 그린다. 정수 정렬 + 1px 여유: 소수 소스 사각형은 이음새를 만든다.
        const QRectF visibleCanvas = QRectF(
            t.inverted().mapRect(QRectF(bounds)).toAlignedRect().adjusted(-1, -1, 1, 1).intersected(backing_.rect()));
        if (!visibleCanvas.isEmpty()) {
            p.fillRect(visibleCanvas, checkerBrush());
            p.drawImage(visibleCanvas, backing_, visibleCanvas);
        }
    }
    viewDirty_ = QRegion();
}

void CanvasWidget::paintEvent(QPaintEvent* e) {
    const u64 tp0 = stroke::monotonicNowNs();
    compositePending();
    const u64 tp1 = stroke::monotonicNowNs();
    renderViewCache();
    const u64 tp2 = stroke::monotonicNowNs();
    if (traceOn() && pendingInputNs_ != 0 && tp0 > pendingInputNs_ && (tp0 - pendingInputNs_) > 16'000'000ull) {
        std::fprintf(stderr, "[mari-gui] slow frame: wait-before-paint %.1f ms, composite %.1f ms, viewcache %.1f ms, rect %dx%d\n",
                     static_cast<f64>(tp0 - pendingInputNs_) / 1e6, static_cast<f64>(tp1 - tp0) / 1e6,
                     static_cast<f64>(tp2 - tp1) / 1e6, e->rect().width(), e->rect().height());
    }

    QPainter p(this);
    // 캐시 블릿. 논리 좌표 = 논리 좌표(캐시의 devicePixelRatio 가 물리 px 로 옮긴다).
    p.drawImage(e->rect(), viewCache_, QRectF(QPointF(e->rect().topLeft()) * dpr(), QSizeF(e->rect().size()) * dpr()));

    // 호버 윤곽: 붓 지름 × 줌. 흰/검 두 겹이라 어떤 배경에서도 보인다. 논리 좌표에서 그린다.
    if (hoverVisible_ && (tool_ == Tool::Brush || tool_ == Tool::Eraser) && !viewDragActive()) {
        p.setRenderHint(QPainter::Antialiasing, true);
        const qreal r = std::max(1.5, brushDiameter_ * view_.state().zoom / dpr() * 0.5);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(0, 0, 0, 160), 1.0));
        p.drawEllipse(hoverPos_, r + 1.0, r + 1.0);
        p.setPen(QPen(QColor(255, 255, 255, 220), 1.0));
        p.drawEllipse(hoverPos_, r, r);
    }
    // 크기 제스처 HUD: 시작점에 현재 지름 원 + 숫자.
    if (sizeDrag_) {
        p.setRenderHint(QPainter::Antialiasing, true);
        const qreal r = std::max(1.5, brushDiameter_ * view_.state().zoom / dpr() * 0.5);
        p.setBrush(QColor(0x3d, 0x7b, 0xd9, 60));
        p.setPen(QPen(QColor(0x3d, 0x7b, 0xd9), 1.5));
        p.drawEllipse(sizeDragStart_, r, r);
        const QString txt = QString("%1 px").arg(brushDiameter_, 0, 'f', 1);
        QFont f = p.font();
        f.setPointSize(11);
        p.setFont(f);
        const QRectF box(sizeDragStart_.x() + r + 10, sizeDragStart_.y() - 12, 90, 24);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x1e, 0x1e, 0x1e, 220));
        p.drawRoundedRect(box, 4, 4);
        p.setPen(Qt::white);
        p.drawText(box, Qt::AlignCenter, txt);
    }
    paintOverlays(p);
    if (xf_.active) paintTransform(p);
    // 선택 점선(marching ants): 캔버스 좌표 경로를 뷰 변환으로 그린다. 코스메틱 펜이라 줌과 무관하게 1px.
    if (hasSelection_ && !xf_.active) {
        p.setRenderHint(QPainter::Antialiasing, false);
        p.setTransform(canvasToWidget());
        QPen white(Qt::white, 0);
        p.setPen(white);
        p.setBrush(Qt::NoBrush);
        p.drawPath(selOutline_);
        QPen black(Qt::black, 0, Qt::CustomDashLine);
        black.setDashPattern({4, 4});
        black.setDashOffset(antsPhase_);
        p.setPen(black);
        p.drawPath(selOutline_);
        p.resetTransform();
    }
    // 드래그 중인 선택 도형 미리보기(논리 좌표).
    if (selDrag_) {
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setBrush(QColor(0x3d, 0x7b, 0xd9, 40));
        p.setPen(QPen(QColor(0x3d, 0x7b, 0xd9), 1, Qt::DashLine));
        if (tool_ == Tool::SelectRect) {
            p.drawRect(QRectF(selStart_, selCur_).normalized());
        } else if (tool_ == Tool::SelectEllipse) {
            p.drawEllipse(QRectF(selStart_, selCur_).normalized());
        } else if (tool_ == Tool::SelectLasso && lasso_.size() > 1) {
            QPainterPath path;
            const QTransform t = canvasToWidget();
            path.moveTo(t.map(lasso_.front()));
            for (std::size_t i = 1; i < lasso_.size(); ++i) path.lineTo(t.map(lasso_[i]));
            path.closeSubpath();
            p.drawPath(path);
        }
    }
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
        Q_EMIT latencyUpdated();
    }
}

void CanvasWidget::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    invalidateView();
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
        // 뷰 드래그(Space·손 도구)와 스포이드는 Qt 마우스 이벤트로 처리한다 — 획이 아니다.
        const bool notAStroke = viewDragActive() || tool_ == Tool::Eyedropper || altEyedropper_ || shiftDown_ ||
                                isSelectionTool(tool_) || isShapeTool(tool_) || tool_ == Tool::Fill || xf_.active;
        if (!inside || !IS_POINTER_FIRSTBUTTON_WPARAM(msg->wParam) || notAStroke) {
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

stroke::RawInputEvent CanvasWidget::shapedEvent(const mari::win::PointerSample& s) {
    stroke::RawInputEvent e = s.event;
    const f32 maxP = e.pressureMax > 0.0f ? e.pressureMax : 1.0f;
    const f32 raw = std::clamp(e.pressure / maxP, 0.0f, 1.0f);
    f32 shaped = raw;
    if (hasPressureLut_ && e.hasPressure) {
        shaped = pressureLut_[static_cast<std::size_t>(std::lround(raw * 255.0f))];
        e.pressure = shaped;
        e.pressureMax = 1.0f;
    }
    lastPen_.valid = true;
    lastPen_.pen = s.kind == mari::win::PointerKind::Pen;
    lastPen_.rawPressure = raw;
    lastPen_.curvedPressure = shaped;
    lastPen_.tiltX = e.tiltXDeg / 90.0f;
    lastPen_.tiltY = e.tiltYDeg / 90.0f;
    lastPen_.rotation = e.rotationDeg;
    return e;
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
    if (tool_ == Tool::Eraser) {
        cfg.eraser = true;
    }
    if (s.eraser) {
        cfg.eraser = true; // 펜을 뒤집었다. 도구 설정보다 우선한다
    }
    cfg.seed = (++strokeSeed_) ^ s.event.timestampNs;
    cfg.undoText = cfg.eraser ? "지우개" : "붓질";

    // 🔴 출처: 사람 펜. 팩토리가 준 것을 넘길 뿐이다. 여기서 만들지 않는다(core/origin.hpp).
    const stroke::RawInputEvent first = shapedEvent(s);
    const u64 t0 = stroke::monotonicNowNs();
    Result<std::unique_ptr<app::LiveStroke>> begun = app::LiveStroke::begin(
        *doc_, StrokeSource::humanPen(), doc_->layers().activeLayer(), cfg, first);
    if (traceOn()) {
        std::fprintf(stderr, "[mari-gui] LiveStroke::begin %.2f ms\n",
                     static_cast<f64>(stroke::monotonicNowNs() - t0) / 1e6);
    }
    if (!begun.ok()) {
        Q_EMIT strokeRefused(QString::fromStdString(begun.message()));
        return;
    }
    live_ = std::move(begun).value();
    scheduleCanvasRepaint(live_->takeDisplayDirty(), s.event.timestampNs);
    strokeStartNs_ = lastMoveNs_ = stroke::monotonicNowNs();
    if (live_->airbrush()) airbrushTimer_->start();
    // 대칭: 축마다 획을 하나 더 시작한다(같은 설정, 다른 시드). 각각 따로 기록·실행취소된다.
    mirrors_.clear();
    mirrorAxes_.clear();
    if (symmetry_ != 0) {
        for (int axis : {1, 2, 3}) {
            // 1 세로축 · 2 가로축 · 3 둘 다(점대칭). 둘 다 켜면 세 획이 더 생긴다.
            if (axis == 3 ? symmetry_ != 3 : (symmetry_ & axis) == 0) continue;
            app::LiveStrokeConfig mc = cfg;
            mc.seed = cfg.seed ^ (0x9E3779B97F4A7C15ull * static_cast<u64>(axis));
            Result<std::unique_ptr<app::LiveStroke>> mb = app::LiveStroke::begin(
                *doc_, StrokeSource::humanPen(), doc_->layers().activeLayer(), mc, mirrored(first, axis));
            if (mb.ok()) {
                mirrors_.push_back(std::move(mb).value());
                mirrorAxes_.push_back(axis); // 시작에 실패한 축은 빠진다 — 인덱스를 축으로 짝지어 둔다
                scheduleCanvasRepaint(mirrors_.back()->takeDisplayDirty(), s.event.timestampNs);
            }
        }
    }
}

void CanvasWidget::onPointerMove(const mari::win::PointerSample& s) {
    if (live_ == nullptr) {
        return;
    }
    const stroke::RawInputEvent e = shapedEvent(s);
    live_->extend(e);
    lastMoveNs_ = stroke::monotonicNowNs();
    scheduleCanvasRepaint(live_->takeDisplayDirty(), e.timestampNs);
    for (usize i = 0; i < mirrors_.size(); ++i) {
        mirrors_[i]->extend(mirrored(e, mirrorAxes_[i]));
        scheduleCanvasRepaint(mirrors_[i]->takeDisplayDirty(), e.timestampNs);
    }
}

void CanvasWidget::onPointerUp(const mari::win::PointerSample& s) {
    const stroke::RawInputEvent e = shapedEvent(s);
    finishStroke(&e);
}

void CanvasWidget::onPointerCancel(u32 /*pointerId*/) {
    finishStroke(nullptr);
}

void CanvasWidget::finishStroke(const stroke::RawInputEvent* last) {
    airbrushTimer_->stop();
    if (live_ == nullptr) {
        return;
    }
    const u64 inputNs = last != nullptr ? last->timestampNs : 0;
    const u64 t0 = stroke::monotonicNowNs();
    // 대칭 획 먼저 마무리(결과는 주 획으로만 보고한다). 실행취소는 아래에서 하나로 묶는다.
    const usize undoBefore = doc_ != nullptr ? doc_->undoStack().undoCount() : 0;
    const bool hadMirrors = !mirrors_.empty();
    for (usize i = 0; i < mirrors_.size(); ++i) {
        std::unique_ptr<app::LiveStroke>& m = mirrors_[i];
        if (last != nullptr) {
            const stroke::RawInputEvent me = mirrored(*last, mirrorAxes_[i]);
            (void)m->end(&me);
        } else {
            (void)m->end(nullptr);
        }
        scheduleCanvasRepaint(m->takeDisplayDirty(), 0);
    }
    mirrors_.clear();
    mirrorAxes_.clear();
    Result<app::LiveStrokeOutcome> ended = live_->end(last);
    // 🔴 같은 레이어에 동시에 달린 획들은 타일 스냅샷이 서로의 잉크를 물고 있다 — 따로 되돌리면 뒤섞인다.
    //    한 항목으로 묶으면 되돌리기/다시하기가 반드시 시작 전/끝난 뒤 상태로만 간다.
    if (hadMirrors && doc_ != nullptr) {
        UndoStack& st = doc_->undoStack();
        const usize pushed = st.undoCount() > undoBefore ? st.undoCount() - undoBefore : 0;
        if (pushed > 1) {
            std::vector<UndoCommandPtr> taken;
            for (usize i = 0; i < pushed; ++i) taken.push_back(st.takeLast());
            auto compound = std::make_unique<CompoundCommand>("붓질(대칭)");
            for (auto it = taken.rbegin(); it != taken.rend(); ++it) compound->add(std::move(*it));
            st.push(std::move(compound));
        }
    }
    if (traceOn()) {
        std::fprintf(stderr, "[mari-gui] LiveStroke::end %.2f ms\n",
                     static_cast<f64>(stroke::monotonicNowNs() - t0) / 1e6);
    }
    Rect dirty = live_->takeDisplayDirty();
    live_.reset();
    if (!ended.ok()) {
        Q_EMIT strokeRefused(QString::fromStdString(ended.message()));
        invalidateCanvas();
        return;
    }
    if (ended.value().rolledBack) {
        // 되돌렸다 — 픽셀이 바뀌었다가 원래대로 갔으니 그 범위를 다시 그린다.
        dirty = dirty.isEmpty() ? ended.value().dirtyBounds : dirty.united(ended.value().dirtyBounds);
    }
    scheduleCanvasRepaint(dirty, inputNs);
    const u64 t1 = stroke::monotonicNowNs();
    Q_EMIT strokeFinished(ended.value());
    if (traceOn()) {
        std::fprintf(stderr, "[mari-gui] strokeFinished handlers %.2f ms\n",
                     static_cast<f64>(stroke::monotonicNowNs() - t1) / 1e6);
    }
}

// ── 뷰 ───────────────────────────────────────────────────────────────────

void CanvasWidget::applyView() {
    // 🔴 획 중간에는 뷰를 바꾸지 않는다 — 같은 획이 두 좌표계에 걸친다(pointer_input.hpp).
    if (live_ != nullptr) {
        return;
    }
    pushViewToPointer();
    invalidateView();
    Q_EMIT viewChanged();
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

QRectF CanvasWidget::visibleCanvasRect() const {
    return canvasToWidget().inverted().mapRect(QRectF(rect()));
}

void CanvasWidget::centerOn(const QPointF& canvasPt) {
    if (live_ != nullptr) return;
    mari::win::ViewState st = view_.state();
    const f64 s = dpr();
    st.anchorCanvas = PointF{static_cast<f32>(canvasPt.x()), static_cast<f32>(canvasPt.y())};
    st.anchorScreen = PointF{static_cast<f32>(width() * s * 0.5), static_cast<f32>(height() * s * 0.5)};
    view_.setState(st);
    applyView();
}

void CanvasWidget::toggleMirror() {
    if (live_ != nullptr) {
        return;
    }
    mari::win::ViewState st = view_.state();
    st.mirrorX = !st.mirrorX;
    // 미러 중심은 화면 중앙: 앵커를 화면 중앙으로 옮겨 놓고 뒤집어야 그림이 제자리에서 뒤집힌다.
    const f64 s = dpr();
    const PointF center{static_cast<f32>(width() * s * 0.5), static_cast<f32>(height() * s * 0.5)};
    st.anchorCanvas = view_.toCanvas(center);
    st.anchorScreen = center;
    view_.setState(st);
    applyView();
}

void CanvasWidget::resetRotation() {
    if (live_ != nullptr) {
        return;
    }
    mari::win::ViewState st = view_.state();
    const f64 s = dpr();
    const PointF center{static_cast<f32>(width() * s * 0.5), static_cast<f32>(height() * s * 0.5)};
    st.anchorCanvas = view_.toCanvas(center);
    st.anchorScreen = center;
    st.rotationDeg = 0.0;
    view_.setState(st);
    applyView();
}

// ── 도구 ─────────────────────────────────────────────────────────────────

void CanvasWidget::setTool(Tool t) {
    if (tool_ == t) {
        return;
    }
    tool_ = t;
    setCursor(t == Tool::Hand ? Qt::OpenHandCursor : Qt::CrossCursor);
    selDrag_ = false;
    lasso_.clear();
    update();
    Q_EMIT toolChanged(t);
}

void CanvasWidget::setBrushDiameter(f64 px) {
    brushDiameter_ = px;
    updateHoverRect();
}

QColor CanvasWidget::pickColorAt(const QPointF& logicalPos) const {
    if (backing_.isNull()) {
        return QColor();
    }
    const QPointF c = canvasToWidget().inverted().map(logicalPos);
    const QPoint px(static_cast<int>(std::floor(c.x())), static_cast<int>(std::floor(c.y())));
    if (!backing_.rect().contains(px)) {
        return QColor();
    }
    // premultiplied → straight. 알파 0 이면 "투명" 이라 배경(흰색)으로 본다.
    const QRgb v = backing_.pixel(px);
    const int a = qAlpha(v);
    if (a == 0) {
        return QColor(255, 255, 255);
    }
    return QColor(qRed(v) * 255 / a, qGreen(v) * 255 / a, qBlue(v) * 255 / a);
}

void CanvasWidget::updateHoverRect() {
    const qreal r = std::max(1.5, brushDiameter_ * view_.state().zoom / dpr() * 0.5) + 3.0;
    update(QRectF(hoverPos_.x() - r, hoverPos_.y() - r, 2 * r, 2 * r).toAlignedRect());
}

// ── 선택 · 채우기 ─────────────────────────────────────────────────────────

void CanvasWidget::rebuildSelectionOutline() {
    selOutline_ = QPainterPath();
    hasSelection_ = false;
    if (doc_ == nullptr) {
        antsTimer_->stop();
        return;
    }
    const SelectionMask& sel = doc_->selectionMask();
    if (sel.isAll() || sel.isEmpty()) {
        antsTimer_->stop();
        update();
        return;
    }
    // 마스크(>127) → 1비트 이미지 → QRegion → 사각형 합집합 경로. 선택이 바뀔 때만 한다.
    const Rect b = sel.bounds().intersected(Rect{0, 0, doc_->canvasSize().width, doc_->canvasSize().height});
    if (b.isEmpty()) {
        antsTimer_->stop();
        update();
        return;
    }
    QImage bits(b.width, b.height, QImage::Format_MonoLSB);
    bits.fill(0);
    for (i32 y = 0; y < b.height; ++y) {
        u8* row = bits.scanLine(y);
        for (i32 x = 0; x < b.width; ++x) {
            if (sel.valueAt(b.x + x, b.y + y) > 127) {
                row[x >> 3] = static_cast<u8>(row[x >> 3] | (1u << (x & 7)));
            }
        }
    }
    QRegion region(QBitmap::fromImage(bits));
    region.translate(b.x, b.y);
    for (const QRect& r : region) {
        selOutline_.addRect(QRectF(r));
    }
    selOutline_ = selOutline_.simplified();
    hasSelection_ = true;
    antsTimer_->start();
    update();
}

void CanvasWidget::applySelection(SelectionMask mask, Qt::KeyboardModifiers mods) {
    if (doc_ == nullptr) return;
    // 수식키 관례: Shift 더하기 · Alt 빼기 · Shift+Alt 교집합 · 없으면 교체.
    SelectionOp op = SelectionOp::Replace;
    const bool shift = mods & Qt::ShiftModifier;
    const bool alt = mods & Qt::AltModifier;
    if (shift && alt) op = SelectionOp::Intersect;
    else if (shift) op = SelectionOp::Union;
    else if (alt) op = SelectionOp::Subtract;
    if (op == SelectionOp::Replace || doc_->selectionMask().isAll()) {
        if (op == SelectionOp::Subtract) {
            // 전체 선택에서 빼기 = 반전한 새 마스크
            mask.invert();
        }
        doc_->setSelectionMask(std::move(mask));
    } else {
        SelectionMask cur = doc_->selectionMask();
        const Result<void> r = cur.combine(mask, op);
        if (!r.ok()) {
            Q_EMIT strokeRefused(QString::fromStdString(r.message()));
            return;
        }
        doc_->setSelectionMask(std::move(cur));
    }
    rebuildSelectionOutline();
    Q_EMIT selectionChanged();
}

void CanvasWidget::finishSelectionDrag(const QPointF& logicalPos, Qt::KeyboardModifiers mods) {
    if (doc_ == nullptr) return;
    const Size cs = doc_->canvasSize();
    const QTransform inv = canvasToWidget().inverted();
    Result<SelectionMask> made = Err("선택 도구가 아니다", ErrorCode::InvalidArgument);
    if (tool_ == Tool::SelectRect || tool_ == Tool::SelectEllipse) {
        const QRectF cr = inv.mapRect(QRectF(selStart_, logicalPos).normalized());
        const Rect r{static_cast<i32>(std::floor(cr.left())), static_cast<i32>(std::floor(cr.top())),
                     static_cast<i32>(std::ceil(cr.width())), static_cast<i32>(std::ceil(cr.height()))};
        if (r.width < 1 || r.height < 1) return;
        made = tool_ == Tool::SelectRect ? SelectionMask::fromRect(cs, r) : SelectionMask::fromEllipse(cs, r, true);
    } else if (tool_ == Tool::SelectLasso) {
        if (lasso_.size() < 3) { lasso_.clear(); return; }
        std::vector<PointF> pts;
        pts.reserve(lasso_.size());
        for (const QPointF& q : lasso_) pts.push_back(PointF{static_cast<f32>(q.x()), static_cast<f32>(q.y())});
        lasso_.clear();
        made = SelectionMask::fromPolygon(cs, pts, true);
    } else if (tool_ == Tool::SelectWand) {
        const QPointF c = inv.map(logicalPos);
        const LayerPtr l = doc_->layers().find(doc_->layers().activeLayer());
        if (!l || l->tiles() == nullptr) return;
        made = SelectionMask::fromFlood(cs, *l->tiles(), static_cast<i32>(std::floor(c.x())),
                                        static_cast<i32>(std::floor(c.y())), floodTolerance_, floodGap_);
    }
    if (!made.ok()) {
        Q_EMIT strokeRefused(QString::fromStdString(made.message()));
        return;
    }
    applySelection(std::move(made).value(), mods);
}

void CanvasWidget::bucketFill(const QPointF& logicalPos) {
    if (doc_ == nullptr || live_ != nullptr) return;
    const QPointF c = canvasToWidget().inverted().map(logicalPos);
    const LayerId lid = doc_->layers().activeLayer();
    const LayerPtr l = doc_->layers().find(lid);
    if (!l || l->tiles() == nullptr) return;
    const Size cs = doc_->canvasSize();
    Result<SelectionMask> region = SelectionMask::fromFlood(cs, *l->tiles(), static_cast<i32>(std::floor(c.x())),
                                                            static_cast<i32>(std::floor(c.y())), floodTolerance_, floodGap_);
    if (!region.ok()) {
        Q_EMIT strokeRefused(QString::fromStdString(region.message()));
        return;
    }
    app::LiveStrokeConfig cfg = cfgProvider_ ? cfgProvider_() : app::LiveStrokeConfig{};
    // 🔴 출처: 사람. 팩토리가 준 것을 넘길 뿐이다.
    const Result<u32> r = app::fillWithMask(*doc_, StrokeSource::humanPen(), lid, region.value(), cfg.color, false);
    if (!r.ok()) {
        Q_EMIT strokeRefused(QString::fromStdString(r.message()));
        return;
    }
    const Rect b = region.value().bounds();
    scheduleCanvasRepaint(b, 0);
    Q_EMIT regionFilled();
}

// ── 격자 · 대칭 · 도형 ────────────────────────────────────────────────────

void CanvasWidget::paintOverlays(QPainter& p) {
    if (doc_ == nullptr) return;
    const Size cs = doc_->canvasSize();
    const QTransform t = canvasToWidget();
    if (gridOn_) {
        p.save();
        p.setRenderHint(QPainter::Antialiasing, false);
        p.setTransform(t);
        QPen minor(QColor(0x3d, 0x7b, 0xd9, 70), 0);
        QPen major(QColor(0x3d, 0x7b, 0xd9, 140), 0);
        for (int x = 0, i = 0; x <= cs.width; x += gridPx_, ++i) {
            p.setPen(i % 5 == 0 ? major : minor);
            p.drawLine(QLineF(x, 0, x, cs.height));
        }
        for (int y = 0, i = 0; y <= cs.height; y += gridPx_, ++i) {
            p.setPen(i % 5 == 0 ? major : minor);
            p.drawLine(QLineF(0, y, cs.width, y));
        }
        p.restore();
    }
    if (symmetry_ != 0) {
        p.save();
        p.setTransform(t);
        QPen axis(QColor(0xff, 0x60, 0x60, 200), 0, Qt::DashLine);
        p.setPen(axis);
        if (symmetry_ & 1) p.drawLine(QLineF(cs.width * 0.5, 0, cs.width * 0.5, cs.height));
        if (symmetry_ & 2) p.drawLine(QLineF(0, cs.height * 0.5, cs.width, cs.height * 0.5));
        p.restore();
    }
    if (shapeDrag_) {
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(0x3d, 0x7b, 0xd9), 1.5, Qt::DashLine));
        const QRectF r = QRectF(shapeStart_, shapeCur_).normalized();
        if (tool_ == Tool::Line || tool_ == Tool::Gradient) p.drawLine(shapeStart_, shapeCur_);
        else if (tool_ == Tool::Rectangle) p.drawRect(r);
        else if (tool_ == Tool::Ellipse) p.drawEllipse(r);
        if (tool_ == Tool::Gradient) {
            p.setBrush(Qt::white);
            p.drawEllipse(shapeStart_, 4, 4);
            p.setBrush(Qt::black);
            p.drawEllipse(shapeCur_, 4, 4);
        }
    }
}

stroke::RawInputEvent CanvasWidget::mirrored(const stroke::RawInputEvent& e, int axis) const {
    stroke::RawInputEvent m = e;
    const Size cs = doc_->canvasSize();
    if (axis & 1) {
        m.x = static_cast<f64>(cs.width) - e.x;
        m.tiltXDeg = -e.tiltXDeg;
    }
    if (axis & 2) {
        m.y = static_cast<f64>(cs.height) - e.y;
        m.tiltYDeg = -e.tiltYDeg;
    }
    return m;
}

void CanvasWidget::strokePolyline(const std::vector<QPointF>& pts, bool closed) {
    if (doc_ == nullptr || live_ != nullptr || pts.size() < 2) return;
    app::LiveStrokeConfig cfg = cfgProvider_ ? cfgProvider_() : app::LiveStrokeConfig{};
    cfg.eraser = tool_ == Tool::Eraser ? true : cfg.eraser;
    cfg.smoothing = stroke::SmoothingMode::Off; // 도형은 정확해야 한다
    cfg.deadZone = 0.0f;
    cfg.seed = (++strokeSeed_) ^ stroke::monotonicNowNs();
    cfg.undoText = tool_ == Tool::Line ? "선" : tool_ == Tool::Rectangle ? "사각형" : "타원";
    // 점들을 1.5px 간격으로 촘촘히(보간기가 붓 간격에 맞춰 찍는다).
    std::vector<QPointF> dense;
    const usize n = pts.size() + (closed ? 1 : 0);
    for (usize i = 0; i + 1 < n; ++i) {
        const QPointF a = pts[i], b = pts[(i + 1) % pts.size()];
        const double len = QLineF(a, b).length();
        const int steps = std::max(1, static_cast<int>(len / 1.5));
        for (int k = 0; k < steps; ++k) dense.push_back(a + (b - a) * (static_cast<double>(k) / steps));
    }
    dense.push_back(closed ? pts.front() : pts.back());
    const u64 t0 = stroke::monotonicNowNs();
    const auto ev = [&](usize i) {
        stroke::RawInputEvent e{};
        e.x = dense[i].x();
        e.y = dense[i].y();
        e.pressure = 1.0f;
        e.pressureMax = 1.0f;
        e.hasPressure = true;
        e.hasTilt = false;
        e.timestampNs = t0 + static_cast<u64>(i) * 1'000'000ull;
        return e;
    };
    Result<std::unique_ptr<app::LiveStroke>> begun =
        app::LiveStroke::begin(*doc_, StrokeSource::humanPen(), doc_->layers().activeLayer(), cfg, ev(0));
    if (!begun.ok()) {
        Q_EMIT strokeRefused(QString::fromStdString(begun.message()));
        return;
    }
    std::unique_ptr<app::LiveStroke> ls = std::move(begun).value();
    for (usize i = 1; i + 1 < dense.size(); ++i) ls->extend(ev(i));
    const stroke::RawInputEvent last = ev(dense.size() - 1);
    Result<app::LiveStrokeOutcome> ended = ls->end(&last);
    if (ended.ok()) {
        scheduleCanvasRepaint(ended.value().dirtyBounds, 0);
        Q_EMIT strokeFinished(ended.value());
    } else {
        Q_EMIT strokeRefused(QString::fromStdString(ended.message()));
    }
}

void CanvasWidget::finishShapeDrag(const QPointF& logicalEnd, Qt::KeyboardModifiers mods) {
    if (doc_ == nullptr) return;
    const QTransform inv = canvasToWidget().inverted();
    QPointF a = inv.map(shapeStart_);
    QPointF b = inv.map(logicalEnd);
    if (mods & Qt::ShiftModifier) {
        // Shift: 선은 45° 단위, 사각형/타원은 정사각/정원, 그라데이션은 45°.
        const QPointF d = b - a;
        if (tool_ == Tool::Line || tool_ == Tool::Gradient) {
            const double ang = std::round(std::atan2(d.y(), d.x()) / (M_PI / 4)) * (M_PI / 4);
            const double len = std::hypot(d.x(), d.y());
            b = a + QPointF(std::cos(ang) * len, std::sin(ang) * len);
        } else {
            const double sz = std::max(std::fabs(d.x()), std::fabs(d.y()));
            b = a + QPointF(d.x() < 0 ? -sz : sz, d.y() < 0 ? -sz : sz);
        }
    }
    if (tool_ == Tool::Gradient) {
        app::LiveStrokeConfig cfg = cfgProvider_ ? cfgProvider_() : app::LiveStrokeConfig{};
        app::GradientParams g;
        g.from = PointF{static_cast<f32>(a.x()), static_cast<f32>(a.y())};
        g.to = PointF{static_cast<f32>(b.x()), static_cast<f32>(b.y())};
        g.colorA = cfg.color;
        g.colorB = gradToTransparent_ ? Color8{cfg.color.r, cfg.color.g, cfg.color.b, 0}
                                      : Color8::rgba(static_cast<u8>(bgColor_.red()), static_cast<u8>(bgColor_.green()),
                                                     static_cast<u8>(bgColor_.blue()), 255);
        g.radial = gradRadial_;
        const Result<u32> r = app::fillGradient(*doc_, StrokeSource::humanPen(), doc_->layers().activeLayer(), g);
        if (!r.ok()) {
            Q_EMIT strokeRefused(QString::fromStdString(r.message()));
            return;
        }
        invalidateCanvas();
        Q_EMIT regionFilled();
        return;
    }
    std::vector<QPointF> pts;
    if (tool_ == Tool::Line) {
        pts = {a, b};
        strokePolyline(pts, false);
    } else if (tool_ == Tool::Rectangle) {
        const QRectF r = QRectF(a, b).normalized();
        pts = {r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft()};
        strokePolyline(pts, true);
    } else if (tool_ == Tool::Ellipse) {
        const QRectF r = QRectF(a, b).normalized();
        const int n = std::max(24, static_cast<int>((r.width() + r.height()) * 2));
        for (int i = 0; i < n; ++i) {
            const double th = 2.0 * M_PI * i / n;
            pts.push_back(QPointF(r.center().x() + r.width() * 0.5 * std::cos(th), r.center().y() + r.height() * 0.5 * std::sin(th)));
        }
        strokePolyline(pts, true);
    }
}

// ── 자유 변형 ─────────────────────────────────────────────────────────────

bool CanvasWidget::beginTransform() {
    if (doc_ == nullptr || live_ != nullptr || xf_.active) return false;
    const LayerId lid = doc_->layers().activeLayer();
    Rect S{};
    Result<ora::Image8> ex = app::extractForTransform(*doc_, lid, S);
    if (!ex.ok() || S.isEmpty()) {
        Q_EMIT strokeRefused(ex.ok() ? QString("변형할 내용이 없다") : QString::fromStdString(ex.message()));
        return false;
    }
    // 잘라 낸 자리를 비운다(실행취소 항목 하나 — 적용/취소 때 되돌린다).
    ora::Image8 remain = app::remainderForTransform(*doc_, lid, S);
    if (!doc_->paintPixels(lid, S, remain.pixels.data(), remain.pixels.size(), kTransformPrepareText).ok()) return false;
    xf_ = Transforming{};
    xf_.active = true;
    xf_.layer = lid;
    xf_.S = S;
    QImage img(ex.value().pixels.data(), S.width, S.height, static_cast<int>(ex.value().stride()), QImage::Format_RGBA8888);
    xf_.img = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    xf_.px = S.x + S.width * 0.5;
    xf_.py = S.y + S.height * 0.5;
    invalidateCanvas(S);
    emitTransformChanged();
    update();
    return true;
}

QTransform CanvasWidget::transformMatrixCanvas() const {
    QTransform m;
    m.translate(xf_.px + xf_.dx, xf_.py + xf_.dy);
    m.rotate(xf_.rot);
    m.scale(xf_.sx * (xf_.flipH ? -1.0 : 1.0), xf_.sy * (xf_.flipV ? -1.0 : 1.0));
    m.translate(-xf_.px, -xf_.py);
    return m;
}

int CanvasWidget::transformHitTest(const QPointF& logicalPos) const {
    const QTransform full = transformMatrixCanvas() * canvasToWidget();
    const QRectF box(xf_.S.x, xf_.S.y, xf_.S.width, xf_.S.height);
    const QPointF corners[8] = {box.topLeft(), QPointF(box.center().x(), box.top()), box.topRight(),
                                QPointF(box.right(), box.center().y()), box.bottomRight(),
                                QPointF(box.center().x(), box.bottom()), box.bottomLeft(),
                                QPointF(box.left(), box.center().y())};
    for (int i = 0; i < 8; ++i) {
        if (QLineF(full.map(corners[i]), logicalPos).length() <= 7.0) return 3 + i;
    }
    const QPolygonF poly = full.map(QPolygonF(box));
    if (poly.containsPoint(logicalPos, Qt::OddEvenFill)) return 1;
    // 상자 밖 가까운 곳은 회전.
    QRectF outer = poly.boundingRect().adjusted(-30, -30, 30, 30);
    if (outer.contains(logicalPos)) return 2;
    return 0;
}

void CanvasWidget::paintTransform(QPainter& p) {
    const QTransform full = transformMatrixCanvas() * canvasToWidget();
    p.save();
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.setTransform(full);
    p.drawImage(QPointF(xf_.S.x, xf_.S.y), xf_.img);
    p.restore();
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRectF box(xf_.S.x, xf_.S.y, xf_.S.width, xf_.S.height);
    const QPolygonF poly = full.map(QPolygonF(box));
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(0, 0, 0, 160), 3.0));
    p.drawPolygon(poly);
    p.setPen(QPen(QColor(0x3d, 0x7b, 0xd9), 1.5));
    p.drawPolygon(poly);
    const QPointF handles[8] = {box.topLeft(), QPointF(box.center().x(), box.top()), box.topRight(),
                                QPointF(box.right(), box.center().y()), box.bottomRight(),
                                QPointF(box.center().x(), box.bottom()), box.bottomLeft(),
                                QPointF(box.left(), box.center().y())};
    p.setBrush(Qt::white);
    for (const QPointF& h : handles) p.drawRect(QRectF(full.map(h) - QPointF(4, 4), QSizeF(8, 8)));
    // 피벗
    const QPointF pv = canvasToWidget().map(QPointF(xf_.px + xf_.dx, xf_.py + xf_.dy));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(pv, 5, 5);
    p.drawLine(pv - QPointF(8, 0), pv + QPointF(8, 0));
    p.drawLine(pv - QPointF(0, 8), pv + QPointF(0, 8));
}

void CanvasWidget::emitTransformChanged() {
    Q_EMIT transformChanged(xf_.active, xf_.dx, xf_.dy, xf_.sx, xf_.sy, xf_.rot);
}

void CanvasWidget::setTransformParams(f64 dx, f64 dy, f64 scaleX, f64 scaleY, f64 rotateDeg) {
    if (!xf_.active) return;
    xf_.dx = dx; xf_.dy = dy; xf_.sx = scaleX; xf_.sy = scaleY; xf_.rot = rotateDeg;
    update();
}

void CanvasWidget::transformFlip(bool horizontal) {
    if (!xf_.active) return;
    if (horizontal) xf_.flipH = !xf_.flipH; else xf_.flipV = !xf_.flipV;
    update();
}

void CanvasWidget::commitTransform() {
    if (!xf_.active || doc_ == nullptr) return;
    const LayerId lid = xf_.layer;
    app::TransformParams tp;
    tp.dx = xf_.dx; tp.dy = xf_.dy; tp.scaleX = xf_.sx; tp.scaleY = xf_.sy; tp.rotateDeg = xf_.rot;
    tp.flipH = xf_.flipH; tp.flipV = xf_.flipV;
    tp.usePivot = true; tp.pivotX = xf_.px; tp.pivotY = xf_.py;
    const Rect S = xf_.S;
    xf_ = Transforming{};
    undoPrepare(); // "변형 준비" 되돌리기 — 원본을 다시 놓고 진짜 변형을 한 번에 한다
    const Result<Rect> r = app::transformLayer(*doc_, StrokeSource::humanPen(), lid, tp);
    if (!r.ok()) Q_EMIT strokeRefused(QString::fromStdString(r.message()));
    invalidateCanvas();
    setCursor(Qt::ArrowCursor);
    emitTransformChanged();
    Q_EMIT regionFilled();
    update();
    (void)S;
}

void CanvasWidget::undoPrepare() {
    // 🔴 스택 맨 위가 정말 "변형 준비" 일 때만 되돌린다. 다른 항목이면(누가 그 사이 스택을 움직였다) 손대지 않는다 —
    //    잘라 낸 자리는 비어 있겠지만, 남의 붓질을 지우는 것보다 낫다.
    if (doc_->undoStack().undoText() == kTransformPrepareText) (void)doc_->undo();
}

void CanvasWidget::cancelTransform() {
    if (!xf_.active || doc_ == nullptr) return;
    xf_ = Transforming{};
    undoPrepare();
    invalidateCanvas();
    setCursor(Qt::ArrowCursor);
    emitTransformChanged();
    update();
}

void CanvasWidget::fillSelection(const QColor& color, bool eraser) {
    if (doc_ == nullptr || live_ != nullptr) return;
    const LayerId lid = doc_->layers().activeLayer();
    const Size cs = doc_->canvasSize();
    // fillWithMask 는 mask × 선택 을 곱하므로 mask 는 "전체" 로 주면 선택만 남는다.
    const SelectionMask& sel = doc_->selectionMask();
    const SelectionMask all = SelectionMask::all(cs);
    const Color8 c{static_cast<u8>(color.red()), static_cast<u8>(color.green()), static_cast<u8>(color.blue()),
                   static_cast<u8>(color.alpha())};
    const Result<u32> r = app::fillWithMask(*doc_, StrokeSource::humanPen(), lid, sel.isAll() ? all : sel, c, eraser);
    if (!r.ok()) {
        Q_EMIT strokeRefused(QString::fromStdString(r.message()));
        return;
    }
    scheduleCanvasRepaint(sel.isAll() ? Rect{0, 0, cs.width, cs.height} : sel.bounds(), 0);
    Q_EMIT regionFilled();
}

void CanvasWidget::selectAll() {
    if (doc_ == nullptr) return;
    doc_->setSelectionMask(SelectionMask::all(doc_->canvasSize()));
    rebuildSelectionOutline();
    Q_EMIT selectionChanged();
}

void CanvasWidget::deselect() {
    selectAll(); // Mari 규약: "선택 없음" = 전체 선택(제한 없음)
}

void CanvasWidget::invertSelection() {
    if (doc_ == nullptr) return;
    SelectionMask m = doc_->selectionMask();
    m.invert();
    doc_->setSelectionMask(std::move(m));
    rebuildSelectionOutline();
    Q_EMIT selectionChanged();
}

void CanvasWidget::selectionChangedExternally() {
    rebuildSelectionOutline();
    Q_EMIT selectionChanged();
}

// ── 마우스·키보드 (그리기가 아닌 조작만) ────────────────────────────────
//
// 🔴 그리기는 여기로 오지 않는다. 왼쪽 버튼·펜 접촉은 nativeEventFilter 가 먼저 먹는다 —
//    단, 뷰 드래그(Space·손 도구)와 스포이드일 때는 필터가 넘겨 주므로 여기로 온다.

void CanvasWidget::wheelEvent(QWheelEvent* e) {
    const f64 steps = e->angleDelta().y() / 120.0;
    if (steps != 0.0) {
        zoomBy(std::pow(1.25, steps), e->position());
    }
    e->accept();
}

void CanvasWidget::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::RightButton && live_ == nullptr) {
        Q_EMIT paletteRequested(e->globalPosition().toPoint());
        e->accept();
        return;
    }
    if (xf_.active && e->button() == Qt::LeftButton && !viewDragActive()) {
        const QPointF c = canvasToWidget().inverted().map(e->position());
        xf_.drag = transformHitTest(e->position());
        xf_.dragStart = c;
        xf_.startDx = xf_.dx; xf_.startDy = xf_.dy; xf_.startSx = xf_.sx; xf_.startSy = xf_.sy; xf_.startRot = xf_.rot;
        xf_.startAngle = std::atan2(c.y() - (xf_.py + xf_.dy), c.x() - (xf_.px + xf_.dx)) * 180.0 / 3.14159265358979;
        e->accept();
        return;
    }
    if (e->button() == Qt::LeftButton && !viewDragActive() && !altEyedropper_) {
        if (tool_ == Tool::Fill) {
            bucketFill(e->position());
            e->accept();
            return;
        }
        if (tool_ == Tool::SelectWand) {
            finishSelectionDrag(e->position(), e->modifiers());
            e->accept();
            return;
        }
        if (isShapeTool(tool_)) {
            shapeDrag_ = true;
            shapeStart_ = shapeCur_ = e->position();
            update();
            e->accept();
            return;
        }
        if (isSelectionTool(tool_)) {
            selDrag_ = true;
            selStart_ = selCur_ = e->position();
            lasso_.clear();
            lasso_.push_back(canvasToWidget().inverted().map(e->position()));
            update();
            e->accept();
            return;
        }
    }
    if (e->button() == Qt::LeftButton && shiftDown_ && !viewDragActive()) {
        sizeDrag_ = true;
        sizeDragStart_ = e->position();
        sizeDragStartDiameter_ = brushDiameter_;
        hoverPos_ = e->position();
        update();
        e->accept();
        return;
    }
    const bool leftDrag = e->button() == Qt::LeftButton && viewDragActive();
    if (e->button() == Qt::MiddleButton || leftDrag) {
        panning_ = true;
        panLast_ = e->position();
        dragStart_ = e->position();
        dragStartView_ = view_.state();
        if (viewMode_ == ViewMode::None) {
            setCursor(Qt::ClosedHandCursor);
        }
        e->accept();
        return;
    }
    if (e->button() == Qt::LeftButton && (tool_ == Tool::Eyedropper || altEyedropper_)) {
        const QColor c = pickColorAt(e->position());
        if (c.isValid()) {
            Q_EMIT colorPicked(c);
        }
        e->accept();
        return;
    }
    QWidget::mousePressEvent(e);
}

void CanvasWidget::mouseMoveEvent(QMouseEvent* e) {
    if (xf_.active) {
        const QPointF c = canvasToWidget().inverted().map(e->position());
        if (xf_.drag == 0) {
            const int h = transformHitTest(e->position());
            setCursor(h == 1 ? Qt::SizeAllCursor : h == 2 ? Qt::CrossCursor : h >= 3 ? Qt::SizeFDiagCursor : Qt::ArrowCursor);
            e->accept();
            return;
        }
        const f64 ddx = c.x() - xf_.dragStart.x(), ddy = c.y() - xf_.dragStart.y();
        if (xf_.drag == 1) {
            xf_.dx = xf_.startDx + ddx;
            xf_.dy = xf_.startDy + ddy;
        } else if (xf_.drag == 2) {
            const f64 ang = std::atan2(c.y() - (xf_.py + xf_.dy), c.x() - (xf_.px + xf_.dx)) * 180.0 / 3.14159265358979;
            f64 r = xf_.startRot + (ang - xf_.startAngle);
            if (e->modifiers() & Qt::ShiftModifier) r = std::round(r / 15.0) * 15.0;
            xf_.rot = r;
        } else {
            // 핸들: 피벗 기준 거리 비율로 확대. 0..7 = 좌상,상,우상,우,우하,하,좌하,좌 (원본 좌표계에서).
            const int h = xf_.drag - 3;
            // 마우스 위치를 원본 좌표계로 되돌린다(회전·이동 제거) → 피벗 기준 반지름 비율.
            const f64 th = -xf_.rot * 3.14159265358979 / 180.0;
            const auto local = [&](const QPointF& q, f64& lx, f64& ly) {
                const f64 rx = q.x() - (xf_.px + xf_.dx), ry = q.y() - (xf_.py + xf_.dy);
                lx = rx * std::cos(th) - ry * std::sin(th);
                ly = rx * std::sin(th) + ry * std::cos(th);
            };
            f64 lx0, ly0, lx1, ly1;
            local(xf_.dragStart, lx0, ly0);
            local(c, lx1, ly1);
            const bool horiz = h == 1 || h == 5; // 상·하 핸들은 세로만
            const bool vert = h == 3 || h == 7;  // 좌·우 핸들은 가로만
            f64 kx = (std::fabs(lx0) > 1e-3 && !horiz) ? lx1 / lx0 : 1.0;
            f64 ky = (std::fabs(ly0) > 1e-3 && !vert) ? ly1 / ly0 : 1.0;
            if (e->modifiers() & Qt::ShiftModifier) { const f64 k = (std::fabs(kx - 1) > std::fabs(ky - 1)) ? kx : ky; kx = ky = k; }
            if (horiz) kx = 1.0;
            if (vert) ky = 1.0;
            xf_.sx = std::clamp(xf_.startSx * kx, -64.0, 64.0);
            xf_.sy = std::clamp(xf_.startSy * ky, -64.0, 64.0);
            if (std::fabs(xf_.sx) < 0.01) xf_.sx = 0.01;
            if (std::fabs(xf_.sy) < 0.01) xf_.sy = 0.01;
        }
        emitTransformChanged();
        update();
        e->accept();
        return;
    }
    if (shapeDrag_) {
        shapeCur_ = e->position();
        update();
        e->accept();
        return;
    }
    if (selDrag_) {
        selCur_ = e->position();
        if (tool_ == Tool::SelectLasso) {
            lasso_.push_back(canvasToWidget().inverted().map(e->position()));
        }
        update();
        e->accept();
        return;
    }
    if (sizeDrag_) {
        // 오른쪽으로 끌면 커진다. 지수 스케일 — 작은 붓은 조금, 큰 붓은 많이.
        const f64 dx = e->position().x() - sizeDragStart_.x();
        const f64 newD = std::clamp(sizeDragStartDiameter_ * std::exp(dx / 120.0), 1.0, 500.0);
        Q_EMIT brushSizeGesture(newD);
        update();
        e->accept();
        return;
    }
    if (panning_) {
        if (viewMode_ == ViewMode::Zoom) {
            // 오른쪽으로 끌면 확대, 왼쪽이면 축소. 누른 점을 중심으로.
            const f64 dx = e->position().x() - dragStart_.x();
            mari::win::ViewState st = dragStartView_;
            view_.setState(st);
            zoomBy(std::pow(1.01, dx), dragStart_);
        } else if (viewMode_ == ViewMode::Rotate) {
            const QPointF c = QRectF(rect()).center();
            const f64 a0 = std::atan2(dragStart_.y() - c.y(), dragStart_.x() - c.x());
            const f64 a1 = std::atan2(e->position().y() - c.y(), e->position().x() - c.x());
            mari::win::ViewState st = dragStartView_;
            view_.setState(st);
            rotateBy((a1 - a0) * 180.0 / 3.14159265358979323846);
        } else {
            panBy(e->position() - panLast_);
        }
        panLast_ = e->position();
        e->accept();
        return;
    }
    if (e->buttons() & Qt::LeftButton) {
        if (tool_ == Tool::Eyedropper || altEyedropper_) {
            const QColor c = pickColorAt(e->position());
            if (c.isValid()) {
                Q_EMIT colorPicked(c);
            }
        }
    }
    // 호버 윤곽: 이전 자리와 새 자리만 갱신한다(신호 없음, 작은 rect 두 개).
    updateHoverRect();
    hoverPos_ = e->position();
    hoverVisible_ = true;
    updateHoverRect();
    QWidget::mouseMoveEvent(e);
}

void CanvasWidget::mouseReleaseEvent(QMouseEvent* e) {
    if (xf_.active && e->button() == Qt::LeftButton) {
        xf_.drag = 0;
        e->accept();
        return;
    }
    if (shapeDrag_ && e->button() == Qt::LeftButton) {
        shapeDrag_ = false;
        finishShapeDrag(e->position(), e->modifiers());
        update();
        e->accept();
        return;
    }
    if (selDrag_ && e->button() == Qt::LeftButton) {
        selDrag_ = false;
        finishSelectionDrag(e->position(), e->modifiers());
        update();
        e->accept();
        return;
    }
    if (sizeDrag_ && e->button() == Qt::LeftButton) {
        sizeDrag_ = false;
        update();
        e->accept();
        return;
    }
    if (panning_ && (e->button() == Qt::MiddleButton || e->button() == Qt::LeftButton)) {
        panning_ = false;
        setCursor(tool_ == Tool::Hand ? Qt::OpenHandCursor : Qt::CrossCursor);
        e->accept();
        return;
    }
    QWidget::mouseReleaseEvent(e);
}

void CanvasWidget::enterEvent(QEnterEvent* e) {
    hoverVisible_ = true;
    hoverPos_ = e->position();
    updateHoverRect();
    QWidget::enterEvent(e);
}

void CanvasWidget::leaveEvent(QEvent* e) {
    hoverVisible_ = false;
    updateHoverRect();
    QWidget::leaveEvent(e);
}

void CanvasWidget::keyPressEvent(QKeyEvent* e) {
    if (e->isAutoRepeat()) {
        e->accept();
        return;
    }
    if (xf_.active) {
        if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) { commitTransform(); e->accept(); return; }
        if (e->key() == Qt::Key_Escape) { cancelTransform(); e->accept(); return; }
        // 화살표: 1px(Shift: 10px) 이동.
        const f64 step = (e->modifiers() & Qt::ShiftModifier) ? 10.0 : 1.0;
        bool moved = true;
        if (e->key() == Qt::Key_Left) xf_.dx -= step;
        else if (e->key() == Qt::Key_Right) xf_.dx += step;
        else if (e->key() == Qt::Key_Up) xf_.dy -= step;
        else if (e->key() == Qt::Key_Down) xf_.dy += step;
        else moved = false;
        if (moved) { emitTransformChanged(); update(); e->accept(); return; }
    }
    // Space 계열은 "눌린 동안 모드" 다. 수식키는 누른 시점의 것을 본다.
    if (e->key() == Qt::Key_Space && live_ == nullptr) {
        spaceDown_ = true;
        const auto mods = e->modifiers();
        viewMode_ = (mods & Qt::ControlModifier) ? ViewMode::Zoom
                    : (mods & Qt::ShiftModifier) ? ViewMode::Rotate
                                                 : ViewMode::Pan;
        setCursor(viewMode_ == ViewMode::Pan ? Qt::OpenHandCursor : Qt::SizeAllCursor);
        update();
        e->accept();
        return;
    }
    if (e->key() == Qt::Key_Alt) {
        altEyedropper_ = true;
        setCursor(Qt::PointingHandCursor);
        e->accept();
        return;
    }
    if (e->key() == Qt::Key_Shift) {
        shiftDown_ = true;
    }
    QWidget::keyPressEvent(e);
}

void CanvasWidget::keyReleaseEvent(QKeyEvent* e) {
    if (e->isAutoRepeat()) {
        e->accept();
        return;
    }
    if (e->key() == Qt::Key_Space) {
        spaceDown_ = false;
        viewMode_ = ViewMode::None;
        panning_ = false;
        setCursor(tool_ == Tool::Hand ? Qt::OpenHandCursor : Qt::CrossCursor);
        update();
        e->accept();
        return;
    }
    if (e->key() == Qt::Key_Alt) {
        altEyedropper_ = false;
        setCursor(tool_ == Tool::Hand ? Qt::OpenHandCursor : Qt::CrossCursor);
        e->accept();
        return;
    }
    if (e->key() == Qt::Key_Shift) {
        shiftDown_ = false;
        sizeDrag_ = false;
        update();
    }
    QWidget::keyReleaseEvent(e);
}

} // namespace mari::ui
