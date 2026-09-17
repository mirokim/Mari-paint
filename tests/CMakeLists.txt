# Mari Paint — 테스트 등록
#
# 두 가지 방법이 있다. 아무 것도 이 파일을 고칠 필요가 없다.
#
#  1) 파일을 놓기만 한다 (권장)
#     tests/test_*.cpp 또는 tests/<모듈>/test_*.cpp 를 놓으면
#     파일 하나당 실행파일 하나가 자동으로 만들어지고 ctest 에 등록된다.
#     등록된 모든 모듈 라이브러리(mari_register_module)가 자동으로 링크된다.
#
#  2) 직접 만든다
#     자기 모듈 CMakeLists.txt 에서
#         mari_add_test(core_tile SOURCES ../tests/core/test_tile.cpp LIBS mari_core)
#     여러 소스를 묶거나 특별한 링크가 필요할 때 쓴다.
#
# 실행: cmake --build build && ctest --test-dir build --output-on-failure

get_property(_mari_module_libs GLOBAL PROPERTY MARI_MODULE_LIBS)

file(GLOB_RECURSE _mari_test_sources CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/*/test_*.cpp")

foreach(_src IN LISTS _mari_test_sources)
    get_filename_component(_stem "${_src}" NAME_WE)
    string(REGEX REPLACE "^test_" "" _name "${_stem}")
    file(RELATIVE_PATH _rel "${CMAKE_CURRENT_SOURCE_DIR}" "${_src}")
    get_filename_component(_dir "${_rel}" DIRECTORY)
    if(_dir)
        string(REPLACE "/" "_" _prefix "${_dir}")
        set(_name "${_prefix}_${_name}")
    endif()
    mari_add_test(${_name} SOURCES "${_src}" LIBS ${_mari_module_libs})
endforeach()

if(NOT _mari_test_sources)
    message(STATUS "Mari: tests/ 에 test_*.cpp 가 없다")
endif()
