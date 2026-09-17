# mari-core — Qt 의존 없음. 순수 C++20.
# 이 파일은 core 모듈 담당 에이전트가 소유한다. 구현 파일을 SOURCES 에 더해라.
# 기반 계층이 만든 스텁이다: 소스가 없으면 인터페이스 라이브러리로 동작한다.

set(MARI_CORE_SOURCES
    # src/tile.cpp
    # src/layer.cpp
)

if(MARI_CORE_SOURCES)
    add_library(mari_core STATIC ${MARI_CORE_SOURCES})
    target_link_libraries(mari_core PUBLIC mari::common)
else()
    add_library(mari_core INTERFACE)
    target_link_libraries(mari_core INTERFACE mari::common)
endif()

add_library(mari::core ALIAS mari_core)
mari_register_module(mari_core)
