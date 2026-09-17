// Mari Paint — EventHub 구현. 리스너 목록 관리와 방송뿐이다.
#include <mari/app/events.hpp>

#include <algorithm>

namespace mari::app {

void EventHub::addListener(IAppEventListener* l) {
    if (l == nullptr) {
        return;
    }
    if (std::find(listeners_.begin(), listeners_.end(), l) != listeners_.end()) {
        return; // 두 번 걸어도 두 번 받지 않는다
    }
    listeners_.push_back(l);
}

bool EventHub::removeListener(IAppEventListener* l) {
    const auto it = std::find(listeners_.begin(), listeners_.end(), l);
    if (it == listeners_.end()) {
        return false;
    }
    listeners_.erase(it);
    return true;
}

void EventHub::fireDocumentOpened(const std::string& path, const std::string& fileHash, i32 w,
                                  i32 h) {
    for (IAppEventListener* l : listeners_) {
        l->onDocumentOpened(path, fileHash, w, h);
    }
}

void EventHub::fireDocumentSaved(const std::string& path, const std::string& fileHash,
                                 i64 sizeBytes) {
    for (IAppEventListener* l : listeners_) {
        l->onDocumentSaved(path, fileHash, sizeBytes);
    }
}

void EventHub::fireCanvasSnapshot(const std::string& canvasHash) {
    for (IAppEventListener* l : listeners_) {
        l->onCanvasSnapshot(canvasHash);
    }
}

void EventHub::fireViewChanged(f64 zoom, f64 rotationDeg) {
    for (IAppEventListener* l : listeners_) {
        l->onViewChanged(zoom, rotationDeg);
    }
}

void EventHub::firePaste(PasteSource source) {
    for (IAppEventListener* l : listeners_) {
        l->onPaste(source);
    }
}

void EventHub::fireUndo(i32 steps) {
    for (IAppEventListener* l : listeners_) {
        l->onUndo(steps);
    }
}

void EventHub::fireLayerChanged(LayerId layerId, const std::string& changeKind) {
    for (IAppEventListener* l : listeners_) {
        l->onLayerChanged(layerId, changeKind);
    }
}

void EventHub::fireStrokeCompleted(LayerId layerId, u64 firstSeq, u64 lastSeq, i32 pointCount) {
    for (IAppEventListener* l : listeners_) {
        l->onStrokeCompleted(layerId, firstSeq, lastSeq, pointCount);
    }
}

} // namespace mari::app
