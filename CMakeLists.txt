# Mari Paint — 루트 빌드 설정
# 규칙: 각 모듈 디렉터리는 자기 CMakeLists.txt 를 소유한다. 이 파일은 기반 계층이 소유한다.
cmake_minimum_required(VERSION 3.20)
project(MariPaint VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "빌드 타입" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# 모든 모듈이 링크하는 공통 인터페이스 타깃.
# include/ 경로와 경고 설정을 여기 한 곳에서 준다.
add_library(mari_common INTERFACE)
add_library(mari::common ALIAS mari_common)
target_include_directories(mari_common INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(mari_common INTERFACE cxx_std_20)

if(MSVC)
    target_compile_options(mari_common INTERFACE /W4 /permissive-)
else()
    target_compile_options(mari_common INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/MariTest.cmake")

option(MARI_BUILD_TESTS "테스트를 빌드한다" ON)
if(MARI_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(core)
add_subdirectory(io/ora)
add_subdirectory(io/brush)
add_subdirectory(sigan)

if(MARI_BUILD_TESTS)
    add_subdirectory(tests)
endif()
