# mari-stroke — 스트로크 파이프라인 + 자체 브러시 엔진. Qt 의존 없음.
# 이 파일은 stroke 모듈 담당이 소유한다.

set(MARI_STROKE_SOURCES
    src/normalize.cpp
    src/smoothing.cpp
    src/interpolate.cpp
    src/native_engine.cpp
    src/pipeline.cpp
    src/engine_factory.cpp
)

add_library(mari_stroke STATIC ${MARI_STROKE_SOURCES})

# core 에 대한 의존은 실제로 있다:
#   · StrokeContext::target 이 mari::TileMap 이고 엔진이 writable() 로 그린다
#   · engine_factory.cpp 가 mari/brush/engine.hpp 의 createEngine() 을 구현한다
# 헤더만 쓰는 지금도 PUBLIC 으로 적어 둔다 — 계약이 코드가 아니라 주석에만 있으면 썩는다.
target_link_libraries(mari_stroke PUBLIC mari::common mari::core)

add_library(mari::stroke ALIAS mari_stroke)
mari_register_module(mari_stroke)

# 테스트는 tests/stroke/test_*.cpp 에 있고 tests/CMakeLists.txt 의 자동 발견이 등록한다.
# (루트에 add_subdirectory(stroke) 가 들어오기 전의 임시 등록 블록은 걷어냈다.)
