# mari-app — 브리지 구현체.
#
# 🔴 COM 레이어가 보던 `IDocumentBridge`/`IApplicationBridge` 는 순수 가상 선언만 있고
#    구현체가 리포 어디에도 없었다(docs/04 2절). 그 구멍을 메우는 모듈이다.
#    **Win32 비의존**이다 — Linux 에서 컴파일·테스트된다. COM 래퍼만 Windows 전용으로 남는다.
#
# 의존: core(레이어·타일·실행취소) · crypto(해시) · io/ora(저장·읽기).
# Qt 도, sigan 도 모른다 — Sigan 상태는 밖에서 setSiganStatus() 로 밀어 넣는다.

set(MARI_APP_SOURCES
    src/events.cpp
    src/document.cpp
    src/stroke_entry.cpp
    src/live_stroke.cpp
    src/application.cpp
)

add_library(mari_app STATIC ${MARI_APP_SOURCES})
# stroke: LiveStroke 가 파이프라인·네이티브 엔진을 굴린다(사람 펜 경로).
target_link_libraries(mari_app PUBLIC mari::common mari::core mari::crypto mari::io_ora mari::stroke)

add_library(mari::app ALIAS mari_app)
mari_register_module(mari_app)
