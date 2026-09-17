# Mari Paint — Windows 전용 레이어 (COM + Windows Ink 입력)
#
# 🔴 이 파일 전체가 if(WIN32) 로 감싸여 있다. Linux/macOS 빌드는 여기서 그냥 나간다.
#    개발은 컨테이너(Linux)에서도 돌아야 한다 — 그게 깨지면 아무도 안 고친다.
#
# 🔴 기반 계층에 요청: 루트 CMakeLists.txt 에 두 줄이 필요하다.
#        add_subdirectory(platform/win)
#        add_subdirectory(hosts)
#    루트는 기반 계층 소유라 여기서 고치지 않았다. 두 줄 다 WIN32 가드가
#    이 파일 안에 있으므로, 넣어도 Linux 빌드는 그대로 통과한다.
#
#    ⚠️ 그때까지 이 디렉터리는 **빌드되지 않는다.** 대신 Win32 를 안 쓰는 부분
#       (뷰 변환 · 펜 정규화 · 8bf 프로토콜 · PIPL 파서)은 전부 헤더온리로 빼 두었고,
#       tests/win/ 의 테스트 4개가 **Linux CI 에서 실제로 돈다.**
#       입력 경로에서 제일 틀리기 쉬운 부분은 Windows 없이도 검증된다.

if(NOT WIN32)
    message(STATUS "Mari: platform/win 은 Windows 에서만 빌드한다 — 건너뛴다")
    return()
endif()

# ──────────────────────────────────────────────────────────────────────────────
# MIDL — mari.idl → mari.h / mari_i.c / mari.tlb
# ──────────────────────────────────────────────────────────────────────────────
set(MARI_IDL "${CMAKE_CURRENT_SOURCE_DIR}/com/mari.idl")
set(MARI_MIDL_DIR "${CMAKE_CURRENT_BINARY_DIR}/midl")
file(MAKE_DIRECTORY "${MARI_MIDL_DIR}")

set(MARI_MIDL_OUTPUTS
    "${MARI_MIDL_DIR}/mari.h"
    "${MARI_MIDL_DIR}/mari_i.c"
    "${MARI_MIDL_DIR}/mari.tlb")

# x64 와 x86 에서 MIDL 의 /env 가 다르다. 안 맞추면 포인터 크기가 어긋난 헤더가 나온다.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(MARI_MIDL_ENV win64)
else()
    set(MARI_MIDL_ENV win32)
endif()

find_program(MARI_MIDL_EXE midl)
if(NOT MARI_MIDL_EXE)
    message(FATAL_ERROR
        "midl.exe 를 찾지 못했다. Windows SDK 가 필요하다.\n"
        "  Visual Studio 개발자 명령 프롬프트에서 cmake 를 돌리거나,\n"
        "  Windows SDK 의 bin 경로를 PATH 에 넣어라.")
endif()

add_custom_command(
    OUTPUT ${MARI_MIDL_OUTPUTS}
    COMMAND "${MARI_MIDL_EXE}"
            /nologo
            /env ${MARI_MIDL_ENV}
            /W1 /WX                       # 경고를 오류로. IDL 오타가 조용히 넘어가면 안 된다
            /out "${MARI_MIDL_DIR}"
            /h mari.h
            /iid mari_i.c
            /tlb mari.tlb
            /proxy mari_p.c
            /dlldata mari_dlldata.c
            "${MARI_IDL}"
    DEPENDS "${MARI_IDL}"
    COMMENT "MIDL: mari.idl → mari.h / mari_i.c / mari.tlb"
    VERBATIM)

add_custom_target(mari_midl DEPENDS ${MARI_MIDL_OUTPUTS})

# IID 정의(mari_i.c)는 이걸 쓰는 실행파일마다 정확히 한 번 링크되어야 한다.
add_library(mari_win_iid STATIC "${MARI_MIDL_DIR}/mari_i.c")
add_dependencies(mari_win_iid mari_midl)
target_include_directories(mari_win_iid PUBLIC "${MARI_MIDL_DIR}")
set_source_files_properties("${MARI_MIDL_DIR}/mari_i.c" PROPERTIES LANGUAGE C)
add_library(mari::win_iid ALIAS mari_win_iid)

# ──────────────────────────────────────────────────────────────────────────────
# mari_win_input — Windows Ink (WM_POINTER)
#
# 🔴 WinTab 은 없다. wintab32 를 로드하는 코드가 한 줄도 없고,
#    아래 링크 목록에도 wintab32.lib 가 없다(docs/03 3절).
#    CI 가 산출물의 임포트 테이블을 검사한다(scripts/ci/check-no-wintab.ps1).
# ──────────────────────────────────────────────────────────────────────────────
set(MARI_WIN_INPUT_SOURCES
    input/pointer_input.cpp
)

add_library(mari_win_input STATIC ${MARI_WIN_INPUT_SOURCES})
target_link_libraries(mari_win_input PUBLIC mari::common)
# user32: WM_POINTER API 전부가 여기 있다. 그 외에 펜 관련 라이브러리는 없다.
target_link_libraries(mari_win_input PUBLIC user32)
add_library(mari::win_input ALIAS mari_win_input)
mari_register_module(mari_win_input)

# ──────────────────────────────────────────────────────────────────────────────
# mari_win_com — COM 서버 (LocalServer32)
# ──────────────────────────────────────────────────────────────────────────────
set(MARI_WIN_COM_SOURCES
    com/com_ptr.cpp
    com/dual_base.cpp
    com/event_sink.cpp
    com/server.cpp
    com/registration.cpp
    com/application.cpp
)

add_library(mari_win_com STATIC ${MARI_WIN_COM_SOURCES})
add_dependencies(mari_win_com mari_midl)
target_include_directories(mari_win_com PUBLIC "${MARI_MIDL_DIR}")
target_link_libraries(mari_win_com PUBLIC
    mari::common
    mari::win_iid
    mari::win_input   # ViewTransform 을 쓴다
    ole32 oleaut32 advapi32 user32)
add_library(mari::win_com ALIAS mari_win_com)
mari_register_module(mari_win_com)

# 타입 라이브러리를 실행파일에 박기 위한 .rc.
# 🔴 tlb 가 실행파일 안에 있어야 등록 전에도 IDispatch 가 돈다(dual_base.cpp 참조).
set(MARI_TLB_RC "${CMAKE_CURRENT_BINARY_DIR}/mari_tlb.rc")
file(WRITE "${MARI_TLB_RC}"
"// 자동 생성. mari.tlb 를 실행파일 리소스로 박는다.\n"
"1 TYPELIB \"${MARI_MIDL_DIR}/mari.tlb\"\n")
set(MARI_TLB_RESOURCE "${MARI_TLB_RC}" CACHE INTERNAL "타입 라이브러리 리소스")

# ──────────────────────────────────────────────────────────────────────────────
# 경고 설정
# COM 헤더(oaidl.h 등)는 /W4 에서 시끄럽다. 우리 코드의 경고는 그대로 두되
# 시스템 헤더는 external 로 돌린다.
# ──────────────────────────────────────────────────────────────────────────────
if(MSVC)
    foreach(_t mari_win_input mari_win_com)
        target_compile_options(${_t} PRIVATE /external:anglebrackets /external:W0)
        target_compile_definitions(${_t} PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            UNICODE
            _UNICODE)
    endforeach()
endif()
