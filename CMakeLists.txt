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

# 스트리밍 푸시 경로(docs/07)가 구독자마다 스레드를 하나씩 쓴다. 여기서 한 번만 건다 —
# 모듈마다 따로 걸면 언젠가 한 곳이 빠지고 그 빌드만 링크가 깨진다.
find_package(Threads REQUIRED)

# 모든 모듈이 링크하는 공통 인터페이스 타깃.
# include/ 경로와 경고 설정을 여기 한 곳에서 준다.
add_library(mari_common INTERFACE)
add_library(mari::common ALIAS mari_common)
target_include_directories(mari_common INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(mari_common INTERFACE cxx_std_20)
target_link_libraries(mari_common INTERFACE Threads::Threads)

if(MSVC)
    # /utf-8: 소스는 BOM 없는 UTF-8 이다. 안 주면 한글 주석·문자열이 코드페이지로 해석된다.
    target_compile_options(mari_common INTERFACE /W4 /permissive- /utf-8)
    # 테스트가 RSS 를 GetProcessMemoryInfo 로 잰다(include/mari/test/sys.hpp).
    target_link_libraries(mari_common INTERFACE psapi)
    # 🔴 ODR 가드. 이 매크로들은 헤더 안의 타입 크기·의미를 바꾼다. 일부 타깃(Qt 를 링크하는 ui)에만
    #    들어가면 같은 클래스를 두 레이아웃으로 컴파일해 힙이 깨진다(실측: MainWindow 생성자에서
    #    0xc0000374, Qt 가 _ENABLE_EXTENDED_ALIGNED_STORAGE 를 ui 에만 넣었다). 전부 여기서 한 번에.
    target_compile_definitions(mari_common INTERFACE
        _ENABLE_EXTENDED_ALIGNED_STORAGE
        NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
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
add_subdirectory(io/psd)
add_subdirectory(sigan)
# app — 플랫폼 중립 브리지 구현체. COM 레이어가 보는 인터페이스를 실제 타입에 물린다.
add_subdirectory(app)
# agent — 🔴 에이전트 능력 계층(docs/05). MCP·CLI·COM 은 이 위에 씌우는 얇은 어댑터다.
add_subdirectory(agent)
# record — 🔴 기록 배선(docs/06). app·agent 가 모르는 sigan 을 여기서만 안다.
#          `SiganPublisher::publish()` 호출자는 리포 전체에서 이 모듈 하나다.
#          cli 보다 먼저여야 한다(cli 가 --proof-out 때문에 링크한다).
add_subdirectory(record)
# mcp — MCP 서버 어댑터(docs/05 4절 · A3). agent-api 위에 씌우는 **얇은** 전송 계층이고
#       도구 목록은 agent 의 연산 표에서 생성된다. cli 보다 먼저여야 한다(cli 가 링크한다).
add_subdirectory(mcp)

# Windows 전용 레이어. 각 파일 안에도 WIN32 가드가 있지만 여기서도 명시적으로 막는다.
# platform/win 이 hosts 보다 먼저여야 한다(hosts/CMakeLists.txt 가 FATAL_ERROR 로 막는다).
if(WIN32)
    # MIDL 이 내는 mari_i.c 는 C, 타입 라이브러리를 박는 .rc 는 RC 다.
    enable_language(C RC)
    add_subdirectory(platform/win)
    add_subdirectory(hosts)
    # ui — Qt GUI. Qt 가 없으면 스스로 빠진다(MARI_HAS_GUI OFF).
    add_subdirectory(ui)
endif()

# cli — 헤드리스 모드 + mari-paint 실행파일(docs/05 2.8). Qt 없이도 빌드된다.
#       ui 뒤에 온다 — Qt 가 있으면 같은 바이너리에 GUI 를 싣는다(MARI_HAS_GUI).
add_subdirectory(cli)

if(MARI_BUILD_TESTS)
    add_subdirectory(tests)
endif()
