// Mari Paint — EventHub 구현. 선언은 include/mari/app/events.hpp.
//
// 🔴 이 파일에서 제일 중요한 것은 **두 경로가 코드에서도 갈라져 있다**는 사실이다.
//    · `dispatch()` 앞부분 = 기록 경로. 루프 하나, 큐 없음, 조건 없음 → 드롭 불가.
//    · `dispatch()` 뒷부분 = 관찰 경로. `Subscriber::offer()` 하나만 부르고,
//      버릴지 말지는 **그 구독자 안에서** 결정된다.
//    버리는 코드가 기록 경로 쪽으로 옮겨 오면 docs/03 4.2 위반이다.
#include <mari/app/events.hpp>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <thread>
#include <utility>

namespace mari::app {

// ── 구독자 하나 ──────────────────────────────────────────────────────────

/// 자기 스레드 + 자기 유한 큐. **다른 구독자와도, 기록 경로와도 공유하지 않는다.**
/// 그래서 느려도 남을 막지 못하고, 넘쳐도 남의 이벤트를 버리지 못한다.
class EventHub::Subscriber {
public:
    Subscriber(SubscriptionId id, EventCallback cb, SubscribeOptions opt)
        : id_(id), cb_(std::move(cb)), cap_(opt.capacity == 0 ? 1 : opt.capacity),
          policy_(opt.onOverflow) {}

    ~Subscriber() { stop(); }

    Subscriber(const Subscriber&) = delete;
    Subscriber& operator=(const Subscriber&) = delete;

    void start() {
        worker_ = std::thread([this] { run(); });
    }

    /// 🔴 **절대 기다리지 않는다.** 이 함수가 블로킹하면 느린 구독자가
    ///    그리기 스레드를 막는다 — 역압 설계 전체가 무너지는 지점이다.
    void offer(const AppEvent& ev) {
        {
            std::lock_guard<std::mutex> g(m_);
            if (stopping_ || detached_) {
                return;
            }
            if (q_.size() >= cap_) {
                if (policy_ == OverflowPolicy::Detach) {
                    // 이 구독자만 끊는다. 남은 큐는 버리고, 끊겼다는 사실은 통계에 남는다.
                    detached_ = true;
                    q_.clear();
                    cv_.notify_all();
                    return;
                }
                // 요약: 오래된 것부터 버리고 **센다.** 조용히 넘어가지 않는다 —
                // 구독자는 이 숫자와 seq 구멍으로 자기가 뭘 못 봤는지 안다.
                q_.pop_front();
                ++summarized_;
            }
            q_.push_back(ev);
        }
        cv_.notify_one();
    }

    void stop() {
        {
            std::lock_guard<std::mutex> g(m_);
            stopping_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    [[nodiscard]] SubscriptionId id() const noexcept { return id_; }

    [[nodiscard]] SubscriberStats stats() const {
        std::lock_guard<std::mutex> g(m_);
        SubscriberStats s;
        s.delivered = delivered_;
        s.summarized = summarized_;
        s.detached = detached_;
        s.alive = true;
        return s;
    }

    /// 큐가 비고 콜백도 놀고 있으면 true.
    [[nodiscard]] bool idle() const {
        std::lock_guard<std::mutex> g(m_);
        return (q_.empty() && !inFlight_) || detached_;
    }

private:
    void run() {
        for (;;) {
            AppEvent ev;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [this] { return stopping_ || detached_ || !q_.empty(); });
                if (q_.empty()) {
                    return; // 멈추라고 했거나 끊겼다
                }
                if (detached_) {
                    return;
                }
                ev = std::move(q_.front());
                q_.pop_front();
                inFlight_ = true;
            }
            // 🔴 잠금 밖에서 부른다. 콜백이 얼마나 오래 걸리든 offer() 는 막히지 않는다.
            if (cb_) {
                cb_(ev);
            }
            {
                std::lock_guard<std::mutex> g(m_);
                inFlight_ = false;
                ++delivered_;
            }
            idleCv_.notify_all();
        }
    }

    mutable std::mutex m_;
    std::condition_variable cv_;
    std::condition_variable idleCv_;
    std::deque<AppEvent> q_;
    std::thread worker_;
    SubscriptionId id_ = 0;
    EventCallback cb_;
    usize cap_ = 256;
    OverflowPolicy policy_ = OverflowPolicy::Summarize;
    u64 delivered_ = 0;
    u64 summarized_ = 0;
    bool stopping_ = false;
    bool detached_ = false;
    bool inFlight_ = false;
};

// ── 허브 ─────────────────────────────────────────────────────────────────

EventHub::EventHub() = default;

EventHub::~EventHub() {
    clear();
}

void EventHub::addListener(IAppEventListener* l) {
    if (l == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> g(mu_);
    if (std::find(listeners_.begin(), listeners_.end(), l) != listeners_.end()) {
        return; // 두 번 걸어도 두 번 받지 않는다
    }
    listeners_.push_back(l);
}

bool EventHub::removeListener(IAppEventListener* l) {
    std::lock_guard<std::mutex> g(mu_);
    const auto it = std::find(listeners_.begin(), listeners_.end(), l);
    if (it == listeners_.end()) {
        return false;
    }
    listeners_.erase(it);
    return true;
}

usize EventHub::listenerCount() const noexcept {
    std::lock_guard<std::mutex> g(mu_);
    return listeners_.size();
}

void EventHub::clear() {
    std::vector<std::shared_ptr<Subscriber>> doomed;
    {
        std::lock_guard<std::mutex> g(mu_);
        listeners_.clear();
        doomed.swap(subs_);
    }
    // 🔴 스레드 합류는 잠금 밖에서 한다. 콜백이 허브를 다시 부를 수 있기 때문이다.
    for (const std::shared_ptr<Subscriber>& s : doomed) {
        s->stop();
    }
}

SubscriptionId EventHub::subscribe(EventCallback cb, SubscribeOptions opt) {
    if (!cb) {
        return 0; // 빈 콜백은 구독이 아니다
    }
    std::shared_ptr<Subscriber> sub;
    SubscriptionId id = 0;
    {
        std::lock_guard<std::mutex> g(mu_);
        id = nextId_++;
        sub = std::make_shared<Subscriber>(id, std::move(cb), opt);
        subs_.push_back(sub);
    }
    sub->start();
    return id;
}

bool EventHub::unsubscribe(SubscriptionId id) {
    std::shared_ptr<Subscriber> doomed;
    {
        std::lock_guard<std::mutex> g(mu_);
        const auto it = std::find_if(subs_.begin(), subs_.end(),
                                     [id](const std::shared_ptr<Subscriber>& s) {
                                         return s->id() == id;
                                     });
        if (it == subs_.end()) {
            return false;
        }
        doomed = *it;
        subs_.erase(it);
    }
    doomed->stop(); // 잠금 밖
    return true;
}

usize EventHub::subscriberCount() const noexcept {
    std::lock_guard<std::mutex> g(mu_);
    return subs_.size();
}

SubscriberStats EventHub::subscriberStats(SubscriptionId id) const {
    std::shared_ptr<Subscriber> found;
    {
        std::lock_guard<std::mutex> g(mu_);
        for (const std::shared_ptr<Subscriber>& s : subs_) {
            if (s->id() == id) {
                found = s;
                break;
            }
        }
    }
    if (!found) {
        return SubscriberStats{}; // alive == false
    }
    return found->stats();
}

bool EventHub::waitForSubscribers(u32 timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        std::vector<std::shared_ptr<Subscriber>> snapshot;
        {
            std::lock_guard<std::mutex> g(mu_);
            snapshot = subs_;
        }
        bool allIdle = true;
        for (const std::shared_ptr<Subscriber>& s : snapshot) {
            allIdle = allIdle && s->idle();
        }
        if (allIdle) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

u64 EventHub::firedCount() const noexcept {
    std::lock_guard<std::mutex> g(mu_);
    return seq_;
}

// ── 발사 ─────────────────────────────────────────────────────────────────

namespace {

/// 값 하나를 타입별 콜백으로 되돌려 준다. COM 중계가 보던 모양 그대로다.
void callTyped(IAppEventListener* l, const AppEvent& ev) {
    switch (ev.kind) {
    case AppEventKind::DocumentOpened:
        l->onDocumentOpened(ev.path, ev.hash, ev.width, ev.height);
        return;
    case AppEventKind::DocumentSaved:
        l->onDocumentSaved(ev.path, ev.hash, ev.sizeBytes);
        return;
    case AppEventKind::CanvasSnapshot:
        l->onCanvasSnapshot(ev.hash);
        return;
    case AppEventKind::ViewChanged:
        l->onViewChanged(ev.zoom, ev.rotationDeg);
        return;
    case AppEventKind::Paste:
        l->onPaste(ev.source);
        return;
    case AppEventKind::Undo:
        l->onUndo(ev.steps);
        return;
    case AppEventKind::LayerChanged:
        l->onLayerChanged(ev.layerId, ev.change);
        return;
    case AppEventKind::StrokeCompleted:
        l->onStrokeCompleted(ev.layerId, ev.firstSeq, ev.lastSeq, ev.pointCount);
        return;
    case AppEventKind::Progress:
        l->onProgress(ev.change, ev.fraction, ev.done);
        return;
    }
}

} // namespace

void EventHub::dispatch(AppEvent& ev) {
    std::vector<IAppEventListener*> listeners;
    std::vector<std::shared_ptr<Subscriber>> subs;
    {
        std::lock_guard<std::mutex> g(mu_);
        ev.seq = ++seq_;
        listeners = listeners_;
        subs = subs_;
    }

    // ── ① 기록 경로. 조건이 없다. 큐도 없다. **여기에 드롭을 넣지 마라**(docs/03 4.2).
    for (IAppEventListener* l : listeners) {
        callTyped(l, ev);
        l->onEvent(ev);
    }

    // ── ② 관찰 경로. 넘칠지 말지는 구독자 안에서 정해지고, 그 구독자에게만 적용된다.
    for (const std::shared_ptr<Subscriber>& s : subs) {
        s->offer(ev);
    }
}

void EventHub::fireDocumentOpened(const std::string& path, const std::string& fileHash, i32 w,
                                  i32 h) {
    AppEvent ev;
    ev.kind = AppEventKind::DocumentOpened;
    ev.path = path;
    ev.hash = fileHash;
    ev.width = w;
    ev.height = h;
    dispatch(ev);
}

void EventHub::fireDocumentSaved(const std::string& path, const std::string& fileHash,
                                 i64 sizeBytes) {
    AppEvent ev;
    ev.kind = AppEventKind::DocumentSaved;
    ev.path = path;
    ev.hash = fileHash;
    ev.sizeBytes = sizeBytes;
    dispatch(ev);
}

void EventHub::fireCanvasSnapshot(const std::string& canvasHash) {
    AppEvent ev;
    ev.kind = AppEventKind::CanvasSnapshot;
    ev.hash = canvasHash;
    dispatch(ev);
}

void EventHub::fireViewChanged(f64 zoom, f64 rotationDeg) {
    AppEvent ev;
    ev.kind = AppEventKind::ViewChanged;
    ev.zoom = zoom;
    ev.rotationDeg = rotationDeg;
    dispatch(ev);
}

void EventHub::firePaste(PasteSource source) {
    AppEvent ev;
    ev.kind = AppEventKind::Paste;
    ev.source = source;
    dispatch(ev);
}

void EventHub::fireUndo(i32 steps) {
    AppEvent ev;
    ev.kind = AppEventKind::Undo;
    ev.steps = steps;
    dispatch(ev);
}

void EventHub::fireLayerChanged(LayerId layerId, const std::string& changeKind) {
    AppEvent ev;
    ev.kind = AppEventKind::LayerChanged;
    ev.layerId = layerId;
    ev.change = changeKind;
    dispatch(ev);
}

void EventHub::fireStrokeCompleted(LayerId layerId, u64 firstSeq, u64 lastSeq, i32 pointCount) {
    AppEvent ev;
    ev.kind = AppEventKind::StrokeCompleted;
    ev.layerId = layerId;
    ev.firstSeq = firstSeq;
    ev.lastSeq = lastSeq;
    ev.pointCount = pointCount;
    dispatch(ev);
}

void EventHub::fireProgress(const std::string& task, f64 fraction, bool done) {
    AppEvent ev;
    ev.kind = AppEventKind::Progress;
    ev.change = task;
    // 범위를 벗어난 진행률은 거짓말이다. 자른다.
    ev.fraction = fraction < 0.0 ? 0.0 : (fraction > 1.0 ? 1.0 : fraction);
    ev.done = done;
    dispatch(ev);
}

} // namespace mari::app
