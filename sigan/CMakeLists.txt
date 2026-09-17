# sigan — Sigan(VASE9) 연동. 스트로크 프레임 · 로컬 저널 · 버전 협상.
# Qt 의존 없음. Mari 는 증명하지 않는다 — 증명 가능한 형태로 기록할 뿐이다(docs/03 2절).
# 이 파일은 sigan 담당 에이전트가 소유한다.

set(MARI_SIGAN_SOURCES
    # src/frame.cpp
    # src/journal.cpp
)

if(MARI_SIGAN_SOURCES)
    add_library(mari_sigan STATIC ${MARI_SIGAN_SOURCES})
    target_link_libraries(mari_sigan PUBLIC mari::common mari::core)
else()
    add_library(mari_sigan INTERFACE)
    target_link_libraries(mari_sigan INTERFACE mari::common mari::core)
endif()

add_library(mari::sigan ALIAS mari_sigan)
mari_register_module(mari_sigan)
