# Mari Paint — .8bf 격리 호스트 (docs/02 6.2)
#
# 🔴 두 벌을 만든다: mari-8bf-host-x64.exe / mari-8bf-host-x86.exe
#    32비트 .8bf 는 32비트 프로세스에서만 돈다. 이건 우회할 방법이 없다.
#
# 🔴 x86 호스트는 **별도 빌드 트리**가 필요하다. 한 CMake 빌드가 두 아키텍처를
#    동시에 내지 못한다. 그래서 여기서는 현재 아키텍처의 호스트만 만들고,
#    다른 쪽은 아래 주석의 명령으로 따로 빌드해 나란히 놓는다.
#    CI(.github/workflows/ci.yml)가 실제로 그렇게 한다 — 두 번 빌드한다.
#
# 🔴 기반 계층에 요청: 루트 CMakeLists.txt 에 `add_subdirectory(hosts)` 한 줄.
#    이 파일은 WIN32 가드가 걸려 있어 Linux 빌드를 깨지 않는다.

if(NOT WIN32)
    message(STATUS "Mari: hosts/ 는 Windows 에서만 빌드한다 — 건너뛴다")
    return()
endif()

if(NOT TARGET mari_win_com)
    message(FATAL_ERROR
        "hosts/ 는 platform/win 이 먼저 add_subdirectory 되어야 한다. "
        "루트 CMakeLists.txt 에서 platform/win 을 hosts 보다 먼저 넣어라.")
endif()

# 아키텍처 접미사. 파일 이름에 그대로 들어간다.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(MARI_HOST_ARCH "x64")
else()
    set(MARI_HOST_ARCH "x86")
endif()

# ──────────────────────────────────────────────────────────────────────────────
# mari-8bf-host-{arch}.exe — 남의 코드가 도는 프로세스
#
# 🔴 이 실행파일이 죽는 것은 정상 동작의 일부다. 본체는 살아 있다.
# ──────────────────────────────────────────────────────────────────────────────
set(MARI_8BF_HOST_SOURCES
    host/main.cpp
    host/plugin_loader.cpp
    host/host_object.cpp
)

add_executable(mari_8bf_host ${MARI_8BF_HOST_SOURCES} "${MARI_TLB_RESOURCE}")
add_dependencies(mari_8bf_host mari_midl)
# tlb 가 바뀌면 리소스를 다시 컴파일한다(platform/win/CMakeLists.txt 의 MARI_TLB_FILE 주석).
set_source_files_properties("${MARI_TLB_RESOURCE}" PROPERTIES OBJECT_DEPENDS "${MARI_TLB_FILE}")
set_target_properties(mari_8bf_host PROPERTIES
    OUTPUT_NAME "mari-8bf-host-${MARI_HOST_ARCH}"
    WIN32_EXECUTABLE FALSE)   # 콘솔 서브시스템. /regserver 오류를 stderr 로 보여 준다

target_link_libraries(mari_8bf_host PRIVATE
    mari::common
    mari::win_com
    mari::win_iid
    ole32 oleaut32 advapi32 user32)

target_include_directories(mari_8bf_host PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/host")

if(MSVC)
    target_compile_definitions(mari_8bf_host PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    target_compile_options(mari_8bf_host PRIVATE /external:anglebrackets /external:W0)

    # 🔴 DEP·ASLR 을 명시적으로 켠다. 신뢰할 수 없는 네이티브 코드를 돌리는
    #    프로세스다 — 기본값에 기대지 않는다.
    target_link_options(mari_8bf_host PRIVATE /NXCOMPAT /DYNAMICBASE)

    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        # 32비트 호스트는 /LARGEADDRESSAWARE 로 3GB 까지 쓴다.
        # protocol.hpp 의 3GB 상한이 이 값과 짝이다.
        target_link_options(mari_8bf_host PRIVATE /LARGEADDRESSAWARE)
    endif()
endif()

# ──────────────────────────────────────────────────────────────────────────────
# mari_8bf_client — 본체가 링크하는 쪽
#
# 🔴 여기에는 LoadLibrary("*.8bf") 가 없다. 있으면 설계 원칙 3 위반이다.
#    플러그인을 여는 코드는 host/ 안에만 있고, 그건 별도 실행파일로만 빌드된다.
# ──────────────────────────────────────────────────────────────────────────────
set(MARI_8BF_CLIENT_SOURCES
    client/host_client.cpp
)

add_library(mari_8bf_client STATIC ${MARI_8BF_CLIENT_SOURCES})
add_dependencies(mari_8bf_client mari_midl)
target_link_libraries(mari_8bf_client PUBLIC
    mari::common
    mari::win_com
    mari::win_iid
    ole32 oleaut32)
target_include_directories(mari_8bf_client PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/client")
add_library(mari::8bf_client ALIAS mari_8bf_client)
mari_register_module(mari_8bf_client)

if(MSVC)
    target_compile_definitions(mari_8bf_client PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    target_compile_options(mari_8bf_client PRIVATE /external:anglebrackets /external:W0)
endif()

# ──────────────────────────────────────────────────────────────────────────────
# 반대편 아키텍처 호스트
#
# 지금 빌드가 x64 면 x86 호스트가, x86 이면 x64 호스트가 따로 필요하다.
# 한 트리에서 둘 다 못 만드니 명령만 남겨 둔다. CI 는 이대로 두 번 돈다.
# ──────────────────────────────────────────────────────────────────────────────
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_other "x86")
    set(_gen_a "-A Win32")
else()
    set(_other "x64")
    set(_gen_a "-A x64")
endif()

message(STATUS
    "Mari: 이 빌드는 mari-8bf-host-${MARI_HOST_ARCH}.exe 만 만든다.\n"
    "      ${_other} 호스트는 따로 빌드해서 나란히 놓아라:\n"
    "        cmake -S . -B build-${_other} ${_gen_a}\n"
    "        cmake --build build-${_other} --target mari_8bf_host\n"
    "      둘 다 /regserver 로 등록해야 한다 — 32/64비트 레지스트리 뷰가 다르다\n"
    "      (client/host_client.cpp 의 CLSCTX_ACTIVATE_*_BIT_SERVER 주석 참조).")
