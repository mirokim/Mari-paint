# Mari Paint — 테스트/모듈 등록 헬퍼.
# 루트 CMakeLists 에서 include 하므로 모든 모듈 디렉터리에서 쓸 수 있다.

# 모듈 라이브러리를 전역 목록에 등록한다.
# tests/ 의 자동 발견 테스트가 여기 등록된 타깃을 전부 링크한다.
#   mari_register_module(mari_core)
function(mari_register_module target)
    set_property(GLOBAL APPEND PROPERTY MARI_MODULE_LIBS "${target}")
endfunction()

# 테스트 실행파일 하나를 만들고 ctest 에 건다.
#   mari_add_test(core_tile SOURCES a.cpp b.cpp LIBS mari_core)
# 이름은 전역에서 유일해야 한다. 모듈 이름을 접두사로 쓰는 것을 권장한다.
function(mari_add_test name)
    cmake_parse_arguments(MT "" "" "SOURCES;LIBS" ${ARGN})
    if(NOT MARI_BUILD_TESTS)
        return()
    endif()
    if(NOT MT_SOURCES)
        message(FATAL_ERROR "mari_add_test(${name}): SOURCES 가 비어 있다")
    endif()
    set(_tgt "mari_test_${name}")
    add_executable(${_tgt} ${MT_SOURCES})
    target_link_libraries(${_tgt} PRIVATE mari::common ${MT_LIBS})
    set_target_properties(${_tgt} PROPERTIES OUTPUT_NAME "${name}")
    add_test(NAME ${name} COMMAND ${_tgt})
    # 🔴 테스트가 진짜 사용자 브러시 폴더(%LOCALAPPDATA%)에 쓰지 않게 격리한다.
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "MARI_BRUSH_DIR=${CMAKE_BINARY_DIR}/test-brushes/${name}")
endfunction()
