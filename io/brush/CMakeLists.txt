# io/brush — 브러시 임포터. .abr(Photoshop) / .sut(Clip Studio) → MariBrushPreset.
# .sut 는 시스템 sqlite3 를 쓴다. 번역 못 한 파라미터는 ImportReport 에 남긴다.
# 이 파일은 io/brush 담당 에이전트가 소유한다.

set(MARI_IO_BRUSH_SOURCES
    # src/abr_import.cpp
    # src/sut_import.cpp
)

if(MARI_IO_BRUSH_SOURCES)
    add_library(mari_io_brush STATIC ${MARI_IO_BRUSH_SOURCES})
    target_link_libraries(mari_io_brush PUBLIC mari::common mari::core)
else()
    add_library(mari_io_brush INTERFACE)
    target_link_libraries(mari_io_brush INTERFACE mari::common mari::core)
endif()

add_library(mari::io_brush ALIAS mari_io_brush)
mari_register_module(mari_io_brush)
