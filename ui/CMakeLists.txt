# Mari Paint — GUI (Qt Widgets)
#
# 🔴 LGPL 모듈만 쓴다(docs/01 5절 · docs/08 2절): QtCore · QtGui · QtWidgets (· QtSvg).
#    QtCharts 같은 GPL 전용 애드온을 하나라도 링크하면 Mari 전체가 GPL 이 된다.
#    아래 mari_check_qt_license() 가 링크 목록을 검사해 허용 밖 모듈이 있으면 빌드를 깬다.
#
# Qt 가 없으면 이 디렉터리는 조용히 빠진다. 헤드리스 CLI·테스트는 Qt 없이 그대로 돈다
# (Linux CI 가 그렇다). mari-paint 는 MARI_HAS_GUI 없이 빌드되고, 인자 없이 뜨면 사용법을 낸다.

find_package(Qt6 6.5 QUIET COMPONENTS Core Gui Widgets Svg)
if(NOT Qt6_FOUND)
    message(STATUS "Mari: Qt6 를 찾지 못했다 — GUI 없이 빌드한다 (CMAKE_PREFIX_PATH 에 Qt 경로를 주면 켜진다)")
    set(MARI_HAS_GUI OFF CACHE INTERNAL "GUI 포함 여부")
    return()
endif()
set(MARI_HAS_GUI ON CACHE INTERNAL "GUI 포함 여부")
message(STATUS "Mari: Qt ${Qt6_VERSION} — GUI 를 빌드한다 (${Qt6_DIR})")

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(MARI_UI_SOURCES
    canvas_widget.cpp
    canvas_widget.hpp
    color_wheel.cpp
    color_wheel.hpp
    color_panel.cpp
    color_panel.hpp
    layer_panel.cpp
    layer_panel.hpp
    layer_row_delegate.cpp
    layer_row_delegate.hpp
    icons.cpp
    icons.hpp
    icons.qrc
    main_window.cpp
    main_window.hpp
    gui.cpp
    gui.hpp
)

add_library(mari_ui STATIC ${MARI_UI_SOURCES})
target_include_directories(mari_ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(mari_ui
    PUBLIC mari::common mari::app mari::record mari::win_input
           Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg)
target_compile_definitions(mari_ui PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
if(MSVC)
    # Qt 헤더는 /W4 에서 시끄럽다. 우리 코드의 경고는 그대로 둔다.
    target_compile_options(mari_ui PRIVATE /external:anglebrackets /external:W0)
endif()
add_library(mari::ui ALIAS mari_ui)

# ── 라이선스 게이트 ──────────────────────────────────────────────────────
# 링크된 Qt6:: 타깃이 허용 목록 밖이면 FATAL_ERROR. docs/02 10절 위험 표의 대응이다.
set(MARI_QT_ALLOWED Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg Qt6::SvgWidgets)
function(mari_check_qt_license target)
    get_target_property(_libs ${target} LINK_LIBRARIES)
    get_target_property(_ilibs ${target} INTERFACE_LINK_LIBRARIES)
    foreach(_l IN LISTS _libs _ilibs)
        if(_l MATCHES "^Qt6::" AND NOT _l IN_LIST MARI_QT_ALLOWED)
            message(FATAL_ERROR
                "Mari: ${target} 이 허용되지 않은 Qt 모듈을 링크한다: ${_l}\n"
                "  LGPL 범위(QtCore·QtGui·QtWidgets·QtSvg) 밖의 모듈은 Mari 전체를 GPL 로 만든다(docs/01 5절).")
        endif()
    endforeach()
endfunction()
mari_check_qt_license(mari_ui)
