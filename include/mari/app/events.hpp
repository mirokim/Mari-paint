// Mari Paint — 앱 이벤트 허브 (docs/03 4절 ③ 이벤트 채널의 **플랫폼 중립 절반**)
//
// 🔴 왜 COM 의 EventBroadcaster 를 그냥 쓰지 않나
//    `mari/win/com/event_sink.hpp` 는 Windows 전용이다 — `IMariEventSink*` 라는 COM
//    포인터와 COM 마샬링 스레드를 안다. 앱 본체가 그걸 직접 알면 본체가 Windows 에
//    묶이고, Linux 에서는 한 줄도 테스트할 수 없게 된다(docs/04 2절이 그 상태다).
//    그래서 본체는 이 중립 허브에 대고 이벤트를 쏘고, COM 레이어는 **리스너 하나**로
//    여기에 붙는다. 이벤트 8종의 목록·인자는 docs/03 4절 표 그대로다.
//
// 스레드 규약: 리스너 호출은 **부른 스레드에서 그대로** 일어난다. 큐잉·스레드 분리는
// 리스너의 몫이다 — COM 리스너(EventBroadcaster)가 이미 그걸 한다(docs/03 5.2).
// 여기서 또 스레드를 만들면 큐가 두 겹이 되고 순서 보장이 어려워진다.
#ifndef MARI_APP_EVENTS_HPP
#define MARI_APP_EVENTS_HPP

#include <mari/core/types.hpp>

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

/// 이벤트를 받는 쪽. 전부 기본 구현이 비어 있어서 필요한 것만 덮어쓰면 된다.
/// COM 레이어는 이걸 구현해서 `IMariEventSink` 로 중계한다.
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
};

/// 붙어 있는 리스너 전부에게 이벤트를 뿌린다. 리스너를 **소유하지 않는다.**
class EventHub {
public:
    /// 같은 포인터를 두 번 걸어도 한 번만 들어간다.
    void addListener(IAppEventListener* l);
    /// 모르는 포인터면 false.
    bool removeListener(IAppEventListener* l);
    [[nodiscard]] usize listenerCount() const noexcept { return listeners_.size(); }
    void clear() noexcept { listeners_.clear(); }

    void fireDocumentOpened(const std::string& path, const std::string& fileHash, i32 w, i32 h);
    void fireDocumentSaved(const std::string& path, const std::string& fileHash, i64 sizeBytes);
    void fireCanvasSnapshot(const std::string& canvasHash);
    void fireViewChanged(f64 zoom, f64 rotationDeg);
    void firePaste(PasteSource source);
    void fireUndo(i32 steps);
    void fireLayerChanged(LayerId layerId, const std::string& changeKind);
    void fireStrokeCompleted(LayerId layerId, u64 firstSeq, u64 lastSeq, i32 pointCount);

private:
    std::vector<IAppEventListener*> listeners_;
};

} // namespace mari::app

#endif // MARI_APP_EVENTS_HPP
