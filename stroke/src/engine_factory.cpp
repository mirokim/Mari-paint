// mari::brush::createEngine 구현 — 엔진 모듈이 자기를 등록하는 자리.
//
// 지금 등록된 것: "native" (자체 최소 엔진).
// libmypaint 어댑터가 들어오면 여기 한 줄을 더한다. 인터페이스는 그대로다.
#include <mari/brush/engine.hpp>
#include <mari/stroke/native_engine.hpp>

namespace mari::brush {

Result<BrushEnginePtr> createEngine(const std::string& name) {
    if (name.empty() || name == stroke::kNativeEngineName)
        return stroke::makeNativeEngine();
    return Err("모르는 브러시 엔진: '" + name + "'", ErrorCode::NotFound);
}

} // namespace mari::brush
