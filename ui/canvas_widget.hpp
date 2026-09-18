// Mari Paint — 캔버스 위젯 (docs/08 3.1 · 3.2 · 3.3)
//
// 이 위젯이 하는 일 세 가지, 그리고 하지 않는 일 하나:
//   1. **더티 영역만** 그린다. `core` 의 합성기가 더티 사각형을 RGBA8 로 평탄화해 주고,
//      여기는 그걸 QImage 백킹에 덮어 쓴 뒤 그 영역만 화면에 올린다. 전체 재합성 코드는 없다.
//   2. 뷰 변환(줌·회전·팬)을 **소유**한다. 캔버스는 화면을 모른다(docs/08 3.1).
//   3. WM_POINTER 를 앱 전역 네이티브 이벤트 필터에서 직접 받아 `platform/win` 의 PointerInput 에
//      넘긴다. 🔴 QTabletEvent 는 쓰지 않는다 — wintab32 를 요구한다(docs/08 3.2).
//      ⚠️ 캔버스에 자기 HWND 를 주지 않는다(WA_NativeWindow 금지). 자식 HWND 는 부모 HWND 에
//      그려지는 형제(도크·상태 표시줄)를 덮어 버린다(실측). 그래서 메시지는 최상위 창 HWND 로
//      오고, 필터가 캔버스 영역 안의 것만 골라 낸다. PointerInput 의 ViewTransform 은 최상위 창
//      물리 클라이언트 좌표 기준이라, 위젯 좌표 기준 view_ 에 캔버스 오프셋을 더한 사본을 준다.
//   ✗ 발행 코드가 없다. 획은 `app::LiveStroke` → `app::StrokeEntry` 로 흐른다.
//      이 파일에서 새로 쓴 기록 코드는 **0줄**이다(docs/08 3.3).
//
// 좌표계 세 개:
//   · 캔버스 px      — 문서 픽셀. 파이프라인 아래로는 이것만 흐른다.
//   · 물리 클라이언트 px — WM_POINTER 가 주는 것. ViewTransform 은 캔버스↔이것을 잇는다.
//   · Qt 논리 px      — QWidget/QPainter 좌표. 물리 px ÷ devicePixelRatio.
//   Qt 6 은 Per-Monitor-V2 라 물리 px 와 논리 px 의 비가 창마다 정해진다. 그 비를 여기서만 곱한다.
//
// 스레드: 전부 UI 스레드다. docs/02 4절은 [1]~[4] 를 UI 밖에서 돌리라 하지만, 먼저 16ms 를
// 재고 나서 옮긴다 — 재지 않고 옮기면 뭘 고쳤는지 모른다(docs/08 4절).
#ifndef MARI_UI_CANVAS_WIDGET_HPP
#define MARI_UI_CANVAS_WIDGET_HPP

#include <mari/app/document.hpp>
#include <mari/app/live_stroke.hpp>
#include <mari/win/input/pointer_input.hpp>
#include <mari/win/input/view_transform.hpp>

#include <QAbstractNativeEventFilter>
#include <QPainterPath>
#include <QColor>
#include <QImage>
#include <QRegion>
#include <QWidget>

#include <array>
#include <functional>
#include <memory>
#include <vector>

class QTimer;

namespace mari::ui {

/// 펜→화면 지연 통계(docs/08 4절 최우선 측정 항목).
/// WM_POINTER 수신 시각(QPC) 부터 그 입력을 반영한 paintEvent 가 끝난 시각까지.
struct LatencyStats {
    f64 lastMs = 0.0;
    f64 avgMs = 0.0;
    f64 maxMs = 0.0;
    u64 samples = 0;
    /// 16ms 를 넘긴 프레임 수. 목표는 0 이다.
    u64 over16 = 0;
};

/// 캔버스 도구. 지우개는 토글이 아니라 도구다(페인팅 앱 관례). 펜 뒤집기는 여전히 우선한다.
enum class Tool { Brush, Eraser, Eyedropper, Hand, Fill, SelectRect, SelectEllipse, SelectLasso, SelectWand };
[[nodiscard]] inline bool isSelectionTool(Tool t) noexcept {
    return t == Tool::SelectRect || t == Tool::SelectEllipse || t == Tool::SelectLasso || t == Tool::SelectWand;
}

/// 태블릿 테스터용 마지막 펜 샘플.
struct PenReadout {
    bool valid = false;
    bool pen = false;          ///< PT_PEN 인가(마우스/터치면 false)
    f32 rawPressure = 0.0f;    ///< 곡선 전 0..1
    f32 curvedPressure = 0.0f; ///< 곡선 후 0..1
    f32 tiltX = 0.0f, tiltY = 0.0f, rotation = 0.0f;
};

class CanvasWidget final : public QWidget,
                           private mari::win::IPointerTarget,
                           private QAbstractNativeEventFilter {
    Q_OBJECT
public:
    explicit CanvasWidget(QWidget* parent = nullptr);
    ~CanvasWidget() override;

    /// 문서를 붙인다(소유하지 않는다). nullptr 이면 빈 화면. 백킹 이미지를 새로 잡고 전부 합성한다.
    void setDocument(app::Document* doc);
    [[nodiscard]] app::Document* document() const noexcept { return doc_; }

    /// 다음 획부터 쓸 설정을 주는 함수. 획 **시작 시점**에 한 번 부른다.
    void setStrokeConfigProvider(std::function<app::LiveStrokeConfig()> f) { cfgProvider_ = std::move(f); }

    /// 캔버스 영역이 바깥에서 바뀌었다(실행취소·레이어 조작). 비면 전부.
    void invalidateCanvas(const Rect& canvasRect = Rect{});

    // ── 도구 ─────────────────────────────────────────────────────────────
    void setTool(Tool t);
    [[nodiscard]] Tool tool() const noexcept { return tool_; }
    /// 호버 윤곽에 쓸 붓 지름(캔버스 px). 툴바가 바뀔 때마다 넣는다.
    void setBrushDiameter(f64 px);
    /// 합성 결과에서 색을 읽는다(스포이드). 캔버스 밖이면 무효 색.
    [[nodiscard]] QColor pickColorAt(const QPointF& logicalPos) const;
    /// 전역 압력 곡선(LUT 256). 붓 프리셋의 압력 다이내믹 **앞**에 적용된다.
    void setPressureCurve(const std::array<f32, 256>& lut) { pressureLut_ = lut; hasPressureLut_ = true; }
    [[nodiscard]] const PenReadout& lastPen() const noexcept { return lastPen_; }
    /// 합성 결과(premultiplied). 내비게이터·썸네일이 읽는다.
    [[nodiscard]] const QImage& backingImage() const noexcept { return backing_; }
    /// 지금 보이는 캔버스 영역(캔버스 px).
    [[nodiscard]] QRectF visibleCanvasRect() const;
    /// 캔버스 점을 위젯 중앙에 놓는다(내비게이터).
    void centerOn(const QPointF& canvasPt);

    // ── 선택 · 채우기 ────────────────────────────────────────────────────
    /// 마술봉·페인트통 허용 오차(0..255)와 틈 닫기(px). 툴바가 넣는다.
    void setFloodOptions(int tolerance, int gapClose) { floodTolerance_ = tolerance; floodGap_ = gapClose; }
    void selectAll();
    void deselect();
    void invertSelection();
    /// 문서의 선택 마스크가 바깥에서 바뀌었다 — 점선을 다시 만든다.
    void selectionChangedExternally();

    // ── 뷰 ───────────────────────────────────────────────────────────────
    [[nodiscard]] const mari::win::ViewState& viewState() const noexcept { return view_.state(); }
    /// 화면 점(논리 px)을 중심으로 배율을 곱한다.
    void zoomBy(f64 factor, QPointF logicalCenter);
    void rotateBy(f64 degrees);
    void panBy(QPointF logicalDelta);
    void resetView();
    /// 캔버스 전체가 보이게 맞춘다.
    void fitToView();
    void toggleMirror();
    void resetRotation();

    [[nodiscard]] const LatencyStats& latency() const noexcept { return latency_; }
    [[nodiscard]] const mari::win::PointerStats& pointerStats() const noexcept { return pointer_.stats(); }
    [[nodiscard]] bool strokeActive() const noexcept { return live_ != nullptr; }

signals:
    /// 획 하나가 끝났다. 결과는 숨기지 않는다(기록 여부·롤백 포함).
    void strokeFinished(const mari::app::LiveStrokeOutcome& outcome);
    /// 획을 시작하지 못했다(잠긴 레이어·기록 고장 등). 사용자에게 보여 줄 문장.
    void strokeRefused(const QString& why);
    void viewChanged();
    void latencyUpdated();
    /// 스포이드로 색을 집었다.
    void colorPicked(const QColor& c);
    void toolChanged(mari::ui::Tool t);
    /// 우클릭·펜 배럴 — 팝업 팔레트를 띄우라(전역 좌표).
    void paletteRequested(const QPoint& globalPos);
    /// Shift+드래그 붓 크기 제스처. 새 지름(캔버스 px).
    void brushSizeGesture(f64 diameter);
    /// 선택이 바뀌었다(메뉴 활성/상태줄).
    void selectionChanged();
    /// 페인트통이 채웠다(썸네일·내비게이터 갱신).
    void regionFilled();

protected:
    void paintEvent(QPaintEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void showEvent(QShowEvent* e) override;
    void moveEvent(QMoveEvent* e) override;
    /// QAbstractNativeEventFilter — 최상위 창 HWND 의 WM_POINTER 를 Qt 보다 먼저 본다.
    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override;
    void wheelEvent(QWheelEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void keyReleaseEvent(QKeyEvent* e) override;
    void leaveEvent(QEvent* e) override;
    void enterEvent(QEnterEvent* e) override;

private:
    // IPointerTarget — 메시지 펌프 스레드(= UI 스레드)에서 불린다.
    void onPointerDown(const mari::win::PointerSample& s) override;
    void onPointerMove(const mari::win::PointerSample& s) override;
    void onPointerUp(const mari::win::PointerSample& s) override;
    void onPointerCancel(u32 pointerId) override;

    void finishStroke(const stroke::RawInputEvent* last);
    /// 압력 곡선을 적용하고 테스터 리드아웃을 갱신한 이벤트를 돌려준다.
    [[nodiscard]] stroke::RawInputEvent shapedEvent(const mari::win::PointerSample& s);
    /// 캔버스 더티 → 백킹 합성 예약 + 화면 갱신 요청.
    void scheduleCanvasRepaint(const Rect& canvasRect, u64 inputNs);
    /// 예약된 캔버스 영역을 백킹 이미지에 합성한다. paintEvent 가 부른다.
    void compositePending();
    void applyView();
    /// PointerInput 에 뷰를 넘긴다(캔버스 오프셋을 더한 최상위 창 좌표 기준).
    void pushViewToPointer();
    /// 최상위 창 클라이언트 물리 px 기준으로 이 위젯의 원점.
    [[nodiscard]] QPointF physOffset() const;
    /// 최상위 창 클라이언트 물리 px 기준으로 이 위젯의 사각형.
    [[nodiscard]] QRectF physRect() const;
    [[nodiscard]] f64 dpr() const;
    [[nodiscard]] QRect canvasToWidgetRect(const Rect& r) const;
    [[nodiscard]] QTransform canvasToWidget() const;
    /// Space 계열 뷰 모드. 눌린 동안만 산다.
    enum class ViewMode { None, Pan, Zoom, Rotate };
    [[nodiscard]] bool viewDragActive() const noexcept { return viewMode_ != ViewMode::None || tool_ == Tool::Hand; }
    void updateHoverRect();
    /// viewDirty_ 를 viewCache_ 에 반영한다. paintEvent 가 부른다.
    void renderViewCache();
    /// 선택 마스크 → 점선 경로(캔버스 좌표). 선택이 바뀔 때만.
    void rebuildSelectionOutline();
    void applySelection(SelectionMask mask, Qt::KeyboardModifiers mods);
    void finishSelectionDrag(const QPointF& logicalPos, Qt::KeyboardModifiers mods);
    void bucketFill(const QPointF& logicalPos);

public:
    /// 현재 선택 영역(없으면 캔버스 전체)을 color 로 채우거나(eraser=false) 지운다(eraser=true).
    /// 포토샵 Alt+Backspace / Ctrl+Backspace / Delete.
    void fillSelection(const QColor& color, bool eraser);

private:
    void invalidateView();  ///< 뷰가 바뀌었다 — 캐시 전체를 버린다

    app::Document* doc_ = nullptr;
    std::function<app::LiveStrokeConfig()> cfgProvider_;
    std::unique_ptr<app::LiveStroke> live_;
    mari::win::PointerInput pointer_;
    mari::win::ViewTransform view_;
    QImage backing_;           ///< 캔버스 크기, ARGB32 premultiplied. 합성 결과
    /// 뷰 캐시: 위젯 크기(물리 px)의, 변환·체커까지 끝난 화면 이미지. paintEvent 는 이걸 블릿만 한다.
    /// 🔴 레이아웃·포커스 변화로 캔버스 전체가 다시 그려질 때 1920×1080 을 매번 축소 샘플링하면
    ///    40ms 가 든다(실측: 획마다 >16ms 한 번). 캐시가 있으면 그 경우는 memcpy 다.
    QImage viewCache_;
    QRegion viewDirty_;        ///< viewCache_ 에서 다시 만들어야 할 영역(논리 px)
    std::vector<u8> straight_; ///< 합성기 출력(straight RGBA) 임시 버퍼. 더티 영역 크기만큼
    Rect pendingComposite_{};  ///< 아직 백킹에 합성하지 않은 캔버스 영역
    u64 pendingInputNs_ = 0;   ///< 화면에 아직 안 오른 가장 이른 입력 시각. 0 이면 없음
    LatencyStats latency_{};
    bool attached_ = false;
    HWND topHwnd_ = nullptr;
    bool needFit_ = true;      ///< 레이아웃이 실제 크기를 주면 한 번 fitToView()
    u32 bypassId_ = 0;         ///< Qt 에 넘긴(그리기 아닌) 포인터 id
    // 마우스 가운데 버튼·Space·손 도구 드래그
    bool panning_ = false;
    QPointF panLast_{};
    QPointF dragStart_{};
    mari::win::ViewState dragStartView_{};
    ViewMode viewMode_ = ViewMode::None;
    bool spaceDown_ = false;
    Tool tool_ = Tool::Brush;
    bool altEyedropper_ = false;   ///< Alt 를 누른 동안 임시 스포이드
    f64 brushDiameter_ = 10.0;
    QPointF hoverPos_{};
    bool hoverVisible_ = false;
    // Shift+드래그 크기 제스처
    bool sizeDrag_ = false;
    QPointF sizeDragStart_{};
    f64 sizeDragStartDiameter_ = 0.0;
    bool shiftDown_ = false;
    // 선택 도구
    bool selDrag_ = false;
    QPointF selStart_{};       ///< 논리 px
    QPointF selCur_{};
    std::vector<QPointF> lasso_; ///< 캔버스 px
    QPainterPath selOutline_;  ///< 캔버스 좌표
    bool hasSelection_ = false;
    int antsPhase_ = 0;
    QTimer* antsTimer_ = nullptr;
    int floodTolerance_ = 32;
    int floodGap_ = 0;
    // 압력 곡선 · 테스터
    std::array<f32, 256> pressureLut_{};
    bool hasPressureLut_ = false;
    PenReadout lastPen_{};
    u64 strokeSeed_ = 0;
};

} // namespace mari::ui

#endif // MARI_UI_CANVAS_WIDGET_HPP
