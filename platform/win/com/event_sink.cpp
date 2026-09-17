// Mari Paint — IMariEventSink 연결점 구현 (docs/03 4절 ③)
//
// 🔴 핵심 약속: **그리기 스레드를 절대 막지 않는다.**
//    fire*() 는 큐에 넣고 바로 돌아온다. 실제 COM 호출은 전용 스레드가 한다.
//    싱크(Sigan)가 느리거나 죽어 있어도 선은 안 끊긴다(docs/03 5.2).
#include <mari/win/com/event_sink.hpp>

#include <windows.h>

#include <condition_variable>
#include <deque>
#include <thread>

#include "mari.h"

namespace mari::win::com {

/// 큐에 담기는 이벤트 하나. 태그 유니온 대신 필드를 다 들고 간다 —
/// 분당 몇 번짜리라 크기보다 단순함이 낫다.
struct EventBroadcaster::Event {
    enum class Kind {
        DocumentOpened,
        DocumentSaved,
        CanvasSnapshot,
        ViewChanged,
        Paste,
        Undo,
        LayerChanged,
        StrokeCompleted,
    } kind{};
    std::string s1;
    std::string s2;
    i64 i1 = 0;
    i64 i2 = 0;
    i64 i3 = 0;
    f64 d1 = 0.0;
    f64 d2 = 0.0;
};

struct EventBroadcaster::Entry {
    long cookie = 0;
    /// 🔴 싱크 포인터를 스레드 사이로 그냥 넘기면 안 된다. 마샬링해서 넘긴다.
    ///    (아파트가 다르면 원시 포인터 호출은 미정의 동작이다.)
    IStream* marshalled = nullptr;
};

struct EventBroadcaster::Impl {
    mutable std::mutex mtx;
    std::vector<Entry> sinks;
    long nextCookie = 1;

    std::mutex qmtx;
    std::condition_variable qcv;
    std::deque<Event> queue;
    bool running = false;
    std::thread thread;

    /// 🔴 큐 상한. 싱크가 영영 안 받아도 메모리를 무한정 먹지 않는다.
    ///    이벤트는 **정본이 아니라 알림**이다(정본 스트로크는 파이프+저널이 책임진다,
    ///    docs/03 4.2). 그래서 여기서는 오래된 것부터 버려도 증거에 구멍이 안 난다.
    static constexpr size_t kMaxQueue = 4096;
    u64 dropped = 0;
};

EventBroadcaster::EventBroadcaster() : impl_(new Impl()) {}

EventBroadcaster::~EventBroadcaster() {
    stop();
    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        for (Entry& e : impl_->sinks) {
            if (e.marshalled != nullptr) {
                ::CoReleaseMarshalData(e.marshalled);
                e.marshalled->Release();
            }
        }
        impl_->sinks.clear();
    }
    delete impl_;
}

long EventBroadcaster::advise(IMariEventSink* sink) {
    if (sink == nullptr) {
        return 0;
    }
    // 다른 스레드에서 부를 수 있도록 마샬링해 둔다.
    IStream* stm = nullptr;
    const HRESULT hr = ::CoMarshalInterThreadInterfaceInStream(IID_IMariEventSink, sink, &stm);
    if (FAILED(hr) || stm == nullptr) {
        return 0;
    }
    std::lock_guard<std::mutex> lk(impl_->mtx);
    Entry e;
    e.cookie = impl_->nextCookie++;
    e.marshalled = stm;
    impl_->sinks.push_back(e);
    return e.cookie;
}

bool EventBroadcaster::unadvise(long cookie) {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    for (auto it = impl_->sinks.begin(); it != impl_->sinks.end(); ++it) {
        if (it->cookie == cookie) {
            if (it->marshalled != nullptr) {
                ::CoReleaseMarshalData(it->marshalled);
                it->marshalled->Release();
            }
            impl_->sinks.erase(it);
            return true;
        }
    }
    return false;
}

usize EventBroadcaster::sinkCount() const {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    return impl_->sinks.size();
}

void EventBroadcaster::start() {
    std::lock_guard<std::mutex> lk(impl_->qmtx);
    if (impl_->running) {
        return;
    }
    impl_->running = true;
    impl_->thread = std::thread([this] { threadMain(); });
}

void EventBroadcaster::stop() {
    {
        std::lock_guard<std::mutex> lk(impl_->qmtx);
        if (!impl_->running) {
            return;
        }
        impl_->running = false;
    }
    impl_->qcv.notify_all();
    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }
}

void EventBroadcaster::enqueue(Event&& e) {
    {
        std::lock_guard<std::mutex> lk(impl_->qmtx);
        if (!impl_->running) {
            return; // 싱크 스레드가 없으면 조용히 버린다(로컬 모드)
        }
        if (impl_->queue.size() >= Impl::kMaxQueue) {
            impl_->queue.pop_front();
            ++impl_->dropped;
        }
        impl_->queue.push_back(std::move(e));
    }
    impl_->qcv.notify_one();
}

void EventBroadcaster::threadMain() {
    // 이벤트 스레드는 자기 아파트를 연다. MTA 로 열어야 싱크 호출이 직렬화되지 않는다.
    const HRESULT hrInit = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool needUninit = SUCCEEDED(hrInit);

    for (;;) {
        Event ev;
        {
            std::unique_lock<std::mutex> lk(impl_->qmtx);
            impl_->qcv.wait(lk, [this] { return !impl_->queue.empty() || !impl_->running; });
            if (impl_->queue.empty()) {
                if (!impl_->running) {
                    break;
                }
                continue;
            }
            ev = std::move(impl_->queue.front());
            impl_->queue.pop_front();
        }

        // 싱크 목록 스냅샷을 뜬다. 호출 중에 목록이 바뀌어도 안전하다.
        std::vector<Entry> snapshot;
        {
            std::lock_guard<std::mutex> lk(impl_->mtx);
            snapshot = impl_->sinks;
        }

        std::vector<long> dead;
        for (const Entry& entry : snapshot) {
            if (entry.marshalled == nullptr) {
                continue;
            }
            // 스트림은 한 번 읽으면 소모된다. 되감아서 다시 읽는다.
            LARGE_INTEGER zero{};
            entry.marshalled->Seek(zero, STREAM_SEEK_SET, nullptr);

            IMariEventSink* sink = nullptr;
            if (FAILED(::CoUnmarshalInterface(entry.marshalled, IID_IMariEventSink,
                                              reinterpret_cast<void**>(&sink))) ||
                sink == nullptr) {
                continue;
            }

            HRESULT hr = S_OK;
            switch (ev.kind) {
            case Event::Kind::DocumentOpened: {
                const Bstr p = bstrFromUtf8(ev.s1);
                const Bstr h = bstrFromUtf8(ev.s2);
                hr = sink->OnDocumentOpened(p.get(), h.get(), static_cast<LONG>(ev.i1),
                                            static_cast<LONG>(ev.i2));
                break;
            }
            case Event::Kind::DocumentSaved: {
                const Bstr p = bstrFromUtf8(ev.s1);
                const Bstr h = bstrFromUtf8(ev.s2);
                hr = sink->OnDocumentSaved(p.get(), h.get(), ev.i1);
                break;
            }
            case Event::Kind::CanvasSnapshot: {
                const Bstr h = bstrFromUtf8(ev.s1);
                hr = sink->OnCanvasSnapshot(h.get());
                break;
            }
            case Event::Kind::ViewChanged:
                hr = sink->OnViewChanged(ev.d1, ev.d2);
                break;
            case Event::Kind::Paste:
                hr = sink->OnPaste(static_cast<MariPasteSource>(ev.i1));
                break;
            case Event::Kind::Undo:
                hr = sink->OnUndo(static_cast<LONG>(ev.i1));
                break;
            case Event::Kind::LayerChanged: {
                const Bstr k = bstrFromUtf8(ev.s1);
                hr = sink->OnLayerChanged(static_cast<LONG>(ev.i1), k.get());
                break;
            }
            case Event::Kind::StrokeCompleted:
                hr = sink->OnStrokeCompleted(static_cast<LONG>(ev.i1), ev.i2, ev.i3,
                                             static_cast<LONG>(ev.d1));
                break;
            }
            sink->Release();

            // 🔴 싱크가 죽었다(Sigan 이 Velopack 업데이트로 재시작 중일 수 있다).
            //    조용히 떼어 낸다. 재연결은 Sigan 이 한다(docs/03 5.3).
            //    **여기서 에러를 띄우거나 그리기를 멈추지 않는다.**
            if (hr == RPC_E_DISCONNECTED || hr == RPC_S_SERVER_UNAVAILABLE ||
                hr == HRESULT_FROM_WIN32(RPC_S_CALL_FAILED) || hr == CO_E_OBJNOTCONNECTED) {
                dead.push_back(entry.cookie);
            }
        }
        for (long c : dead) {
            unadvise(c);
        }
    }

    if (needUninit) {
        ::CoUninitialize();
    }
}

// ── 이벤트 8종 (docs/03 4절 표) ─────────────────────────────────────────────

void EventBroadcaster::fireDocumentOpened(const std::string& path, const std::string& hash, i32 w,
                                          i32 h) {
    Event e;
    e.kind = Event::Kind::DocumentOpened;
    e.s1 = path;
    e.s2 = hash;
    e.i1 = w;
    e.i2 = h;
    enqueue(std::move(e));
}

void EventBroadcaster::fireDocumentSaved(const std::string& path, const std::string& hash,
                                         i64 sizeBytes) {
    Event e;
    e.kind = Event::Kind::DocumentSaved;
    e.s1 = path;
    e.s2 = hash;
    e.i1 = sizeBytes;
    enqueue(std::move(e));
}

void EventBroadcaster::fireCanvasSnapshot(const std::string& hash) {
    Event e;
    e.kind = Event::Kind::CanvasSnapshot;
    e.s1 = hash;
    enqueue(std::move(e));
}

void EventBroadcaster::fireViewChanged(f64 zoom, f64 rotation) {
    Event e;
    e.kind = Event::Kind::ViewChanged;
    e.d1 = zoom;
    e.d2 = rotation;
    enqueue(std::move(e));
}

void EventBroadcaster::firePaste(PasteSource source) {
    Event e;
    e.kind = Event::Kind::Paste;
    e.i1 = static_cast<i64>(source);
    enqueue(std::move(e));
}

void EventBroadcaster::fireUndo(i32 steps) {
    Event e;
    e.kind = Event::Kind::Undo;
    e.i1 = steps;
    enqueue(std::move(e));
}

void EventBroadcaster::fireLayerChanged(i32 layerId, const std::string& changeKind) {
    Event e;
    e.kind = Event::Kind::LayerChanged;
    e.i1 = layerId;
    e.s1 = changeKind;
    enqueue(std::move(e));
}

void EventBroadcaster::fireStrokeCompleted(i32 layerId, u64 firstSeq, u64 lastSeq, i32 pointCount) {
    Event e;
    e.kind = Event::Kind::StrokeCompleted;
    e.i1 = layerId;
    e.i2 = static_cast<i64>(firstSeq);
    e.i3 = static_cast<i64>(lastSeq);
    e.d1 = static_cast<f64>(pointCount);
    enqueue(std::move(e));
}

// ── IConnectionPoint ────────────────────────────────────────────────────────
namespace {

/// IMariEventSink 하나짜리 연결점. C# 의 `event`, VB6 의 `WithEvents` 가 이걸 찾는다.
class SinkConnectionPoint final : public IConnectionPoint {
public:
    SinkConnectionPoint(EventBroadcaster* bc, IUnknown* owner) noexcept : bc_(bc), owner_(owner) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (ppv == nullptr) {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == IID_IConnectionPoint) {
            *ppv = static_cast<IConnectionPoint*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(::InterlockedIncrement(&refs_));
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG n = ::InterlockedDecrement(&refs_);
        if (n == 0) {
            delete this;
            return 0;
        }
        return static_cast<ULONG>(n);
    }

    HRESULT STDMETHODCALLTYPE GetConnectionInterface(IID* pIID) override {
        if (pIID == nullptr) {
            return E_POINTER;
        }
        *pIID = IID_IMariEventSink;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE
    GetConnectionPointContainer(IConnectionPointContainer** ppCPC) override {
        if (ppCPC == nullptr) {
            return E_POINTER;
        }
        return owner_->QueryInterface(IID_IConnectionPointContainer,
                                      reinterpret_cast<void**>(ppCPC));
    }
    HRESULT STDMETHODCALLTYPE Advise(IUnknown* pUnk, DWORD* pdwCookie) override {
        if (pUnk == nullptr || pdwCookie == nullptr) {
            return E_POINTER;
        }
        *pdwCookie = 0;
        IMariEventSink* sink = nullptr;
        const HRESULT hr =
            pUnk->QueryInterface(IID_IMariEventSink, reinterpret_cast<void**>(&sink));
        if (FAILED(hr)) {
            return CONNECT_E_CANNOTCONNECT;
        }
        const long cookie = bc_->advise(sink);
        sink->Release();
        if (cookie == 0) {
            return CONNECT_E_CANNOTCONNECT;
        }
        *pdwCookie = static_cast<DWORD>(cookie);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Unadvise(DWORD dwCookie) override {
        return bc_->unadvise(static_cast<long>(dwCookie)) ? S_OK : CONNECT_E_NOCONNECTION;
    }
    HRESULT STDMETHODCALLTYPE EnumConnections(IEnumConnections**) override {
        // 열거는 아무도 안 쓴다. 정직하게 미구현이라고 말한다.
        return E_NOTIMPL;
    }

private:
    ~SinkConnectionPoint() = default;
    LONG refs_ = 1;
    EventBroadcaster* bc_;
    IUnknown* owner_; ///< 소유자가 더 오래 산다. 참조를 잡으면 순환한다
};

} // namespace

ConnectionPointContainer::ConnectionPointContainer(EventBroadcaster* bc) noexcept
    : bc_(bc), cp_(nullptr) {}

ConnectionPointContainer::~ConnectionPointContainer() {
    if (cp_ != nullptr) {
        cp_->Release();
        cp_ = nullptr;
    }
}

HRESULT ConnectionPointContainer::EnumConnectionPoints(IEnumConnectionPoints** ppEnum) {
    if (ppEnum == nullptr) {
        return E_POINTER;
    }
    *ppEnum = nullptr;
    return E_NOTIMPL; // 연결점이 하나뿐이라 FindConnectionPoint 로 충분하다
}

HRESULT ConnectionPointContainer::FindConnectionPoint(REFIID riid, IConnectionPoint** ppCP) {
    if (ppCP == nullptr) {
        return E_POINTER;
    }
    *ppCP = nullptr;
    if (riid != IID_IMariEventSink) {
        return CONNECT_E_NOCONNECTION;
    }
    if (cp_ == nullptr) {
        cp_ = new (std::nothrow) SinkConnectionPoint(bc_, owner());
        if (cp_ == nullptr) {
            return E_OUTOFMEMORY;
        }
    }
    cp_->AddRef();
    *ppCP = cp_;
    return S_OK;
}

} // namespace mari::win::com
