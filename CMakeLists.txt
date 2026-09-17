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
add_subdirectory(crypto)
add_subdirectory(stroke)
add_subdirectory(io/ora)
add_subdirectory(io/brush)
add_subdirectory(sigan)
# app — 플랫폼 중립 브리지 구현체. COM 레이어가 보는 인터페이스를 실제 타입에 물린다.
add_subdirectory(app)
# agent — 🔴 에이전트 능력 계층(docs/05). MCP·CLI·COM 은 이 위에 씌우는 얇은 어댑터다.
add_subdirectory(agent)
# mcp — MCP 서버 어댑터(docs/05 4절 · A3). agent-api 위에 씌우는 **얇은** 전송 계층이고
#       도구 목록은 agent 의 연산 표에서 생성된다. cli 보다 먼저여야 한다(cli 가 링크한다).
add_subdirectory(mcp)
# cli — 헤드리스 모드 + mari-paint 실행파일(docs/05 2.8). Qt 없이 빌드된다.
add_subdirectory(cli)

# Windows 전용 레이어. 각 파일 안에도 WIN32 가드가 있지만 여기서도 명시적으로 막는다.
# platform/win 이 hosts 보다 먼저여야 한다(hosts/CMakeLists.txt 가 FATAL_ERROR 로 막는다).
if(WIN32)
    add_subdirectory(platform/win)
    add_subdirectory(hosts)
endif()

if(MARI_BUILD_TESTS)
    add_subdirectory(tests)
endif()
