// Mari Paint — IMariEventSink 연결점 (docs/03 4절 ③ 이벤트 채널)
//
// Sigan 이 싱크를 구현하고, Mari 가 호출한다. docs/03 4절 표의 이벤트 8종 전부.
//
// 🔴 이 채널로 스트로크를 보내지 마라. 초당 수백 개다 — COM 마샬링이 16ms 예산을 깬다.
//    스트로크는 명명 파이프 `sigan-native` 가 받는다(docs/03 4절 ②, sigan 모듈).
//    여기로 오는 건 저장·뷰변경·붙여넣기 같은 분당 몇 번짜리다.
//
// 🔴 그리기 스레드를 막지 않는다(docs/03 5.2).
//    싱크 호출은 전부 전용 이벤트 스레드에서 한다. 싱크가 느려도 선은 안 끊긴다.
//    싱크가 죽어 있으면(RPC_E_*) **조용히 떼어 낸다** — 재연결은 Sigan 이 한다.
//
// ⚠️ Windows 전용.
#ifndef MARI_WIN_COM_EVENT_SINK_HPP
#define MARI_WIN_COM_EVENT_SINK_HPP

#if !defined(_WIN32)
#error "mari/win/com/event_sink.hpp 는 Windows 전용이다"
#endif

#include <mari/core/types.hpp>
#include <mari/win/com/com_ptr.hpp>

// IConnectionPointContainer · IConnectionPoint · IEnumConnectionPoints 는 ocidl.h,
// CONNECT_E_* 는 olectl.h 에 있다. objbase.h 만으로는 안 나온다.
#include <ocidl.h>
#include <olectl.h>

#include <mutex>
#include <string>
#include <vector>

struct IMariEventSink; // MIDL 이 mari.h 에 낸다

namespace mari::win::com {

/// 붙여넣기 출처. IDL 의 MariPasteSource 와 값이 같다.
enum class PasteSource : long {
    Unknown = 0,
    Clipboard = 1,
    File = 2,
    DragDrop = 3,
    Script = 4,
    Internal = 5,
};

/// 붙어 있는 싱크 전부에게 이벤트를 뿌린다.
///
/// 스레드 규약: 어느 스레드에서 fire*() 를 불러도 된다. 내부에서 큐에 넣고
/// 전용 스레드가 실제 COM 호출을 한다 — **호출자는 절대 안 막힌다.**
class EventBroadcaster {
public:
    EventBroadcaster();
    ~EventBroadcaster();

    EventBroadcaster(const EventBroadcaster&) = delete;
    EventBroadcaster& operator=(const EventBroadcaster&) = delete;

    /// 싱크를 건다. 돌려주는 쿠키로 나중에 뗀다. 0 은 실패다.
    [[nodiscard]] long advise(IMariEventSink* sink);
    /// 싱크를 뗀다. 모르는 쿠키면 false.
    bool unadvise(long cookie);
    /// 붙어 있는 싱크 수.
    [[nodiscard]] usize sinkCount() const;

    /// 이벤트 스레드를 띄운다. COM 서버 시작 시 한 번.
    void start();
    /// 큐를 비우고 스레드를 접는다. 서버 종료 시.
    void stop();

    // ── docs/03 4절 표의 이벤트 8종 ─────────────────────────────────────
    void fireDocumentOpened(const std::string& path, const std::string& hash, i32 w, i32 h);
    void fireDocumentSaved(const std::string& path, const std::string& hash, i64 sizeBytes);
    void fireCanvasSnapshot(const std::string& hash);
    void fireViewChanged(f64 zoom, f64 rotation);
    void firePaste(PasteSource source);
    /// steps > 0 이면 undo, < 0 이면 redo.
    void fireUndo(i32 steps);
    void fireLayerChanged(i32 layerId, const std::string& changeKind);
    /// 🔴 firstSeq/lastSeq 는 `sigan-native` 파이프 프레임의 seq 다.
    ///    이걸로 COM 이벤트와 파이프 스트로크를 이어 붙인다(docs/03 4.1).
    void fireStrokeCompleted(i32 layerId, u64 firstSeq, u64 lastSeq, i32 pointCount);

private:
    struct Event;      ///< 큐에 담기는 이벤트 하나
    struct Entry;      ///< 싱크 + 쿠키
    struct Impl;       ///< 스레드·큐·뮤텍스
    void enqueue(Event&& e);
    void threadMain();

    Impl* impl_;
};

/// `IConnectionPointContainer` / `IConnectionPoint` 구현.
///
/// AdviseEvents() 만 쓰면 스크립트 언어에서는 충분하지만, C# 의 `event` 키워드와
/// VB6 의 `WithEvents` 는 표준 연결점을 찾는다. **둘 다 지원한다.**
class ConnectionPointContainer : public IConnectionPointContainer {
public:
    explicit ConnectionPointContainer(EventBroadcaster* bc) noexcept;
    virtual ~ConnectionPointContainer();

    // 🔴 IUnknown 은 **소유 객체에 위임한다.**
    //    이 컨테이너는 독립된 COM 객체가 아니라 MariApplication 의 한 면이다.
    //    자기 참조 카운트를 따로 세면 COM 의 "모든 인터페이스가 같은 IUnknown 을
    //    돌려준다" 규칙이 깨지고, 클라이언트가 같은 객체를 다른 객체로 오해한다.
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        return owner()->QueryInterface(riid, ppv);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return owner()->AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override { return owner()->Release(); }

    HRESULT STDMETHODCALLTYPE EnumConnectionPoints(IEnumConnectionPoints** ppEnum) override;
    HRESULT STDMETHODCALLTYPE FindConnectionPoint(REFIID riid, IConnectionPoint** ppCP) override;

protected:
    /// 컨테이너의 수명은 소유 객체가 관리한다. 여기서는 위임만 한다.
    virtual IUnknown* owner() noexcept = 0;

private:
    EventBroadcaster* bc_;
    IConnectionPoint* cp_; ///< IMariEventSink 용 하나만 있다
};

} // namespace mari::win::com

#endif // MARI_WIN_COM_EVENT_SINK_HPP
