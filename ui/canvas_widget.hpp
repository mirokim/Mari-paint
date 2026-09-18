// Mari Paint — 캔버스 위젯 (docs/08 3.1 · 3.2 · 3.3)
//
// 이 위젯이 하는 일 세 가지, 그리고 하지 않는 일 하나:
//   1. **더티 영역만** 그린다. `core` 의 합성기가 더티 사각형을 RGBA8 로 평탄화해 주고,
//      여기는 그걸 QImage 백킹에 덮어 쓴 뒤 그 영역만 화면에 올린다. 전체 재합성 코드는 없다.
//   2. 뷰 변환(줌·회전·팬)을 **소유**한다. 캔버스는 화면을 모른다(docs/08 3.1).
//   3. WM_POINTER 를 `nativeEvent()` 에서 직접 받아 `platform/win` 의 PointerInput 에 넘긴다.
//      🔴 QTabletEvent 는 쓰지 않는다 — wintab32 를 요구한다(docs/08 3.2).
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

#include <QImage>
#include <QWidget>

#include <functional>
#include <memory>

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

class CanvasWidget final : public QWidget, private mari::win::IPointerTarget {
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

    // ── 뷰 ───────────────────────────────────────────────────────────────
    [[nodiscard]] const mari::win::ViewState& viewState() const noexcept { return view_.state(); }
    /// 화면 점(논리 px)을 중심으로 배율을 곱한다.
    void zoomBy(f64 factor, QPointF logicalCenter);
    void rotateBy(f64 degrees);
    void panBy(QPointF logicalDelta);
    void resetView();
    /// 캔버스 전체가 보이게 맞춘다.
    void fitToView();

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

protected:
    void paintEvent(QPaintEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void showEvent(QShowEvent* e) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
    void wheelEvent(QWheelEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;

private:
    // IPointerTarget — 메시지 펌프 스레드(= UI 스레드)에서 불린다.
    void onPointerDown(const mari::win::PointerSample& s) override;
    void onPointerMove(const mari::win::PointerSample& s) override;
    void onPointerUp(const mari::win::PointerSample& s) override;
    void onPointerCancel(u32 pointerId) override;

    void finishStroke(const stroke::RawInputEvent* last);
    /// 캔버스 더티 → 백킹 합성 예약 + 화면 갱신 요청.
    void scheduleCanvasRepaint(const Rect& canvasRect, u64 inputNs);
    /// 예약된 캔버스 영역을 백킹 이미지에 합성한다. paintEvent 가 부른다.
    void compositePending();
    void applyView();
    [[nodiscard]] f64 dpr() const;
    [[nodiscard]] QRect canvasToWidgetRect(const Rect& r) const;
    [[nodiscard]] QTransform canvasToWidget() const;

    app::Document* doc_ = nullptr;
    std::function<app::LiveStrokeConfig()> cfgProvider_;
    std::unique_ptr<app::LiveStroke> live_;
    mari::win::PointerInput pointer_;
    mari::win::ViewTransform view_;
    QImage backing_;           ///< 캔버스 크기, RGBA8888(straight alpha). 합성 결과
    Rect pendingComposite_{};  ///< 아직 백킹에 합성하지 않은 캔버스 영역
    u64 pendingInputNs_ = 0;   ///< 화면에 아직 안 오른 가장 이른 입력 시각. 0 이면 없음
    LatencyStats latency_{};
    bool attached_ = false;
    // 마우스 가운데 버튼 팬
    bool panning_ = false;
    QPointF panLast_{};
    u64 strokeSeed_ = 0;
};

} // namespace mari::ui

#endif // MARI_UI_CANVAS_WIDGET_HPP
