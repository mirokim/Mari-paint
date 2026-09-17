// Mari Paint — 앱 이벤트 허브 (docs/03 4절 ③ 이벤트 채널의 **플랫폼 중립 절반**)
//
// 🔴 왜 COM 의 EventBroadcaster 를 그냥 쓰지 않나
//    `mari/win/com/event_sink.hpp` 는 Windows 전용이다 — `IMariEventSink*` 라는 COM
//    포인터와 COM 마샬링 스레드를 안다. 앱 본체가 그걸 직접 알면 본체가 Windows 에
//    묶이고, Linux 에서는 한 줄도 테스트할 수 없게 된다(docs/04 2절이 그 상태다).
//    그래서 본체는 이 중립 허브에 대고 이벤트를 쏘고, COM 레이어는 **리스너 하나**로
//    여기에 붙는다. 이벤트 8종의 목록·인자는 docs/03 4절 표 그대로다.
//
// 🔴 이 버스에는 **정책이 다른 두 경로**가 있다. 섞으면 둘 중 하나가 반드시 거짓말을 한다.
//
//    ① 기록 경로 — `addListener(IAppEventListener*)`
//       **동기 호출. 큐가 없다. 그래서 드롭이 구조적으로 불가능하다.**
//       Sigan 기록(docs/03 4.2 "네이티브 경로는 정본이다. 드롭 금지")과 COM 중계가 여기다.
//       느리면 그리는 쪽이 느려진다 — 그게 맞다. 증거를 버리느니 붓이 느린 게 낫다.
//       (발행기는 어차피 저널에 먼저 쓰고 파이프는 논블로킹이라 실제로는 안 느리다.)
//
//    ② 관찰 경로 — `subscribe(fn)`
//       **구독자마다 자기 스레드와 자기 유한 큐를 갖는다.** 넘치면 그 구독자만
//       요약(오래된 것부터 버리고 센다)하거나 끊긴다. 남의 구독자도, 기록 경로도,
//       그리기 스레드도 건드리지 않는다.
//       에이전트 스트리밍(docs/05 2.7)·`--serve` 푸시가 여기다.
//
//    🔴 두 경로를 한 API 로 합치지 마라. 합치는 순간 "드롭 금지"와 "드롭 허용"이
//       같은 함수에 들어가고, 어느 쪽이 적용됐는지 호출자가 알 수 없게 된다.
//
// 스레드 규약:
//   · ①의 리스너 호출은 **부른 스레드에서 그대로** 일어난다(예전 그대로).
//   · ②의 콜백은 **그 구독자의 전용 스레드에서** 일어난다. 세션·문서를 만지지 마라.
//   · `fire*` 는 여러 스레드에서 불러도 된다. seq 는 버스 전역으로 단조 증가한다.
#ifndef MARI_APP_EVENTS_HPP
#define MARI_APP_EVENTS_HPP

#include <mari/core/types.hpp>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mari::app {

/// 붙여넣기 출처. 값은 IDL 의 `MariPasteSource` 와 같다 — 바꾸면 COM 이 깨진다.
enum class PasteSource : i32 {
    Unknown = 0,
    Clipboard = 1,
    File = 2,
    DragDrop = 3,
    Script = 4,
    Internal = 5,
};

/// 안정 문자열. 로그·테스트용.
[[nodiscard]] constexpr const char* pasteSourceName(PasteSource s) noexcept {
    switch (s) {
    case PasteSource::Clipboard:
        return "clipboard";
    case PasteSource::File:
        return "file";
    case PasteSource::DragDrop:
        return "dragDrop";
    case PasteSource::Script:
        return "script";
    case PasteSource::Internal:
        return "internal";
    case PasteSource::Unknown:
        break;
    }
    return "unknown";
}

// ── 이벤트 값 표현 ───────────────────────────────────────────────────────

/// 이벤트 종류. **docs/03 4절 표의 8종 + 진행률.**
/// 값은 로그·와이어에 이름으로 나가므로 뒤에만 더해라.
enum class AppEventKind : u8 {
    DocumentOpened = 1,
    DocumentSaved = 2,
    CanvasSnapshot = 3,
    ViewChanged = 4,
    Paste = 5,
    Undo = 6,
    LayerChanged = 7,
    StrokeCompleted = 8,
    /// 긴 작업(큰 필터·8bf·대형 저장)의 진행률(docs/05 2.7).
    /// 🔴 **기록이 아니다.** 픽셀을 설명하지 않는 진척 보고일 뿐이라
    ///    Sigan 정본에 들어갈 사실이 아니다. 관찰 경로용으로 두는 이유가 그것이다.
    Progress = 9,
};

/// 안정 문자열. 에이전트 API 의 `kind` 가 이 이름 그대로다 — 바꾸면 클라이언트가 깨진다.
[[nodiscard]] constexpr const char* appEventKindName(AppEventKind k) noexcept {
    switch (k) {
    case AppEventKind::DocumentOpened:
        return "documentOpened";
    case AppEventKind::DocumentSaved:
        return "documentSaved";
    case AppEventKind::CanvasSnapshot:
        return "canvasSnapshot";
    case AppEventKind::ViewChanged:
        return "viewChanged";
    case AppEventKind::Paste:
        return "paste";
    case AppEventKind::Undo:
        return "undo";
    case AppEventKind::LayerChanged:
        return "layerChanged";
    case AppEventKind::StrokeCompleted:
        return "strokeCompleted";
    case AppEventKind::Progress:
        return "progress";
    }
    return "unknown";
}

/// 이벤트 하나의 **값**. 큐에 담으려면 값이어야 한다(포인터·참조를 담으면
/// 구독자 스레드가 읽을 때쯤 대상이 죽어 있을 수 있다).
///
/// 필드를 종류별로 나눠 두지 않고 한 구조체에 평평하게 둔 것은 의도다 —
/// 종류가 늘 때마다 큐·직렬화·전송을 함께 고쳐야 하는 구조를 만들지 않는다.
/// 어느 종류가 어느 칸을 쓰는지는 아래 주석이 정본이다.
struct AppEvent {
    AppEventKind kind = AppEventKind::Progress;
    /// 🔴 **버스 전역 단조 증가.** 구독자가 요약(드롭)당했을 때 seq 구멍으로
    ///    "몇 개를 못 봤는지"를 스스로 안다. docs/03 4.2 의 seq 사고방식 그대로다 —
    ///    빠진 것을 조용히 덮지 않는다.
    u64 seq = 0;

    std::string path;   ///< DocumentOpened · DocumentSaved
    /// DocumentOpened/Saved 면 **파일 해시**, CanvasSnapshot 이면 **캔버스 픽셀 해시**.
    std::string hash;
    /// LayerChanged 의 변경 종류("pixels" · "added" · ...), Progress 의 작업 이름.
    std::string change;

    i32 width = 0;     ///< DocumentOpened
    i32 height = 0;    ///< DocumentOpened
    i64 sizeBytes = 0; ///< DocumentSaved
    f64 zoom = 1.0;         ///< ViewChanged
    f64 rotationDeg = 0.0;  ///< ViewChanged
    PasteSource source = PasteSource::Unknown; ///< Paste
    i32 steps = 0;     ///< Undo (>0 undo, <0 redo)
    LayerId layerId = kInvalidLayerId; ///< LayerChanged · StrokeCompleted
    u64 firstSeq = 0;  ///< StrokeCompleted — 파이프 프레임 seq(docs/03 4.1)
    u64 lastSeq = 0;   ///< StrokeCompleted
    i32 pointCount = 0; ///< StrokeCompleted
    f64 fraction = 0.0; ///< Progress 0..1
    bool done = false;  ///< Progress — 끝났나
};

/// 이벤트를 받는 쪽. 전부 기본 구현이 비어 있어서 필요한 것만 덮어쓰면 된다.
/// COM 레이어는 이걸 구현해서 `IMariEventSink` 로 중계한다.
///
/// 🔴 **기록 경로다.** 동기 호출이고 큐가 없다 — 그래서 여기서 이벤트가 사라질 수 없다.
class IAppEventListener {
public:
    virtual ~IAppEventListener() = default;

    /// hash 는 **연 파일의 SHA-256**(파일 바이트 그대로. crypto/sha256.hpp 규약).
    virtual void onDocumentOpened(const std::string& /*path*/, const std::string& /*fileHash*/,
                                  i32 /*w*/, i32 /*h*/) {}
    virtual void onDocumentSaved(const std::string& /*path*/, const std::string& /*fileHash*/,
                                 i64 /*sizeBytes*/) {}
    /// hash 는 **캔버스 픽셀 해시**(crypto/canvas_hash.hpp 규약). 파일 해시가 아니다.
    virtual void onCanvasSnapshot(const std::string& /*canvasHash*/) {}
    virtual void onViewChanged(f64 /*zoom*/, f64 /*rotationDeg*/) {}
    /// 🔴 붓으로 그리지 않은 픽셀이 들어왔다. 숨기지 않는다(docs/03 2절 정신).
    virtual void onPaste(PasteSource /*source*/) {}
    /// steps > 0 이면 undo, < 0 이면 redo.
    virtual void onUndo(i32 /*steps*/) {}
    virtual void onLayerChanged(LayerId /*layerId*/, const std::string& /*changeKind*/) {}
    /// firstSeq/lastSeq 는 `sigan-native` 파이프 프레임의 seq 다(docs/03 4.1).
    virtual void onStrokeCompleted(LayerId /*layerId*/, u64 /*firstSeq*/, u64 /*lastSeq*/,
                                   i32 /*pointCount*/) {}
    /// 긴 작업의 진행률(docs/05 2.7). COM 표에는 없다 — 중계하지 않아도 된다.
    virtual void onProgress(const std::string& /*task*/, f64 /*fraction*/, bool /*done*/) {}

    /// 🔴 위 타입별 콜백과 **같은 사건의 다른 표현**이다. 허브는 타입별을 먼저 부르고
    ///    이어서 이걸 부른다 — **둘 다 덮어쓰면 같은 사건을 두 번 받는다.** 하나만 골라라.
    ///    (COM 중계는 타입별을, 값이 필요한 쪽은 이쪽을 쓴다.)
    virtual void onEvent(const AppEvent& /*ev*/) {}
};

// ── 관찰 경로(구독) ──────────────────────────────────────────────────────

/// 구독자 식별자. 0 은 "없음"이다.
using SubscriptionId = u64;

/// 구독자 콜백. **그 구독자의 전용 스레드에서** 불린다.
using EventCallback = std::function<void(const AppEvent&)>;

/// 🔴 큐가 넘쳤을 때 **그 구독자에게만** 적용되는 정책(docs/05 2.7 역압).
///    기록 경로에는 이런 것이 없다 — 거긴 큐가 아예 없어서 넘칠 일이 없다.
enum class OverflowPolicy : u8 {
    /// 오래된 것부터 버리고 **버린 수를 센다.** 구독자는 seq 구멍으로도 안다.
    /// 조용히 넘어가지 않는다는 점이 핵심이다(docs/03 4.2 의 정신).
    Summarize = 0,
    /// 그 구독자만 끊는다. 다시 받고 싶으면 다시 구독해야 한다.
    Detach = 1,
};

struct SubscribeOptions {
    /// 이 구독자의 큐 길이. 넘으면 onOverflow 가 적용된다.
    usize capacity = 256;
    OverflowPolicy onOverflow = OverflowPolicy::Summarize;
};

/// 구독자 하나의 정직한 숫자. **숨기는 칸이 없다.**
struct SubscriberStats {
    u64 delivered = 0;   ///< 콜백이 실제로 받은 건수
    u64 summarized = 0;  ///< 🔴 이 구독자에게만 버려진 건수. 0 이 아니면 그렇게 보인다
    bool detached = false; ///< Detach 정책으로 끊겼나
    bool alive = false;    ///< 아직 구독 목록에 있나
};

/// 이벤트 버스 하나. **Sigan 도 에이전트도 이 하나를 쓴다**(docs/05 2.7 — 두 번 만들지 않는다).
/// 리스너도 구독자 콜백도 **소유하지 않는다**(콜백은 값으로 복사해 둔다).
class EventHub {
public:
    EventHub();
    /// 구독자 스레드를 전부 세우고 합류시킨 뒤에 돌아온다.
    ~EventHub();

    EventHub(const EventHub&) = delete;
    EventHub& operator=(const EventHub&) = delete;

    // ── ① 기록 경로 — 동기 호출, 큐 없음, 드롭 불가 ──────────────────────

    /// 같은 포인터를 두 번 걸어도 한 번만 들어간다.
    void addListener(IAppEventListener* l);
    /// 모르는 포인터면 false.
    bool removeListener(IAppEventListener* l);
    [[nodiscard]] usize listenerCount() const noexcept;
    /// 리스너와 구독자를 **전부** 뗀다.
    void clear();

    // ── ② 관찰 경로 — 구독자별 스레드·유한 큐·역압 ───────────────────────

    /// 콜백을 건다. 돌아온 id 로 떼거나 통계를 본다. 콜백이 비었으면 0 을 돌려준다.
    ///
    /// 🔴 콜백은 **다른 스레드에서** 불린다. 세션·문서 같은 단일 스레드 객체를
    ///    만지지 마라. 느려도 된다 — 느린 만큼 자기 큐만 찬다.
    [[nodiscard]] SubscriptionId subscribe(EventCallback cb, SubscribeOptions opt = {});
    /// 그 구독자의 스레드를 세우고 합류시킨다. 모르는 id 면 false.
    bool unsubscribe(SubscriptionId id);
    [[nodiscard]] usize subscriberCount() const noexcept;
    /// 모르는 id 면 `alive == false` 인 빈 통계.
    [[nodiscard]] SubscriberStats subscriberStats(SubscriptionId id) const;

    /// 모든 구독자의 큐가 빌 때까지 기다린다(진단·테스트용).
    /// 시간 안에 다 비면 true. **그리기 경로에서 부르지 마라** — 느린 구독자를
    /// 기다리는 함수라 부르는 순간 역압 정책이 무의미해진다.
    [[nodiscard]] bool waitForSubscribers(u32 timeoutMs);

    // ── 발사 ─────────────────────────────────────────────────────────────

    void fireDocumentOpened(const std::string& path, const std::string& fileHash, i32 w, i32 h);
    void fireDocumentSaved(const std::string& path, const std::string& fileHash, i64 sizeBytes);
    void fireCanvasSnapshot(const std::string& canvasHash);
    void fireViewChanged(f64 zoom, f64 rotationDeg);
    void firePaste(PasteSource source);
    void fireUndo(i32 steps);
    void fireLayerChanged(LayerId layerId, const std::string& changeKind);
    void fireStrokeCompleted(LayerId layerId, u64 firstSeq, u64 lastSeq, i32 pointCount);
    /// 긴 작업의 진행률(docs/05 2.7). fraction 은 0..1 로 잘린다.
    void fireProgress(const std::string& task, f64 fraction, bool done);

    /// 지금까지 이 버스를 지나간 이벤트 수(= 마지막 seq).
    [[nodiscard]] u64 firedCount() const noexcept;

private:
    class Subscriber; ///< 구현은 .cpp — 스레드·큐는 헤더가 알 필요가 없다

    /// 값 하나를 두 경로에 뿌린다. seq 는 여기서 붙는다.
    void dispatch(AppEvent& ev);

    mutable std::mutex mu_;
    std::vector<IAppEventListener*> listeners_;
    std::vector<std::shared_ptr<Subscriber>> subs_;
    SubscriptionId nextId_ = 1;
    u64 seq_ = 0;
};

} // namespace mari::app

#endif // MARI_APP_EVENTS_HPP
