# io/brush — 브러시 임포터. .abr(Photoshop) / .sut(Clip Studio) → MariBrushPreset.
# .sut 는 시스템 sqlite3 를 쓴다. 텍스처 PNG 디코드는 시스템 libpng 을 쓴다.
# 번역 못 한 파라미터는 ImportReport 에 남긴다.
# 이 파일은 io/brush 담당 에이전트가 소유한다.

set(MARI_IO_BRUSH_SOURCES
    src/byte_reader.cpp
    src/tar.cpp
    src/png_gray.cpp
    src/abr_descriptor.cpp
    src/abr_import.cpp
    src/sut_import.cpp
)

find_package(SQLite3 REQUIRED)
find_package(PNG REQUIRED)

if(MARI_IO_BRUSH_SOURCES)
    add_library(mari_io_brush STATIC ${MARI_IO_BRUSH_SOURCES})
    # 테스트 픽스처가 직접 sqlite3/libpng 로 .sut/.png 를 만들어야 해서 PUBLIC 으로 둔다.
    target_link_libraries(mari_io_brush PUBLIC mari::common mari::core SQLite::SQLite3 PNG::PNG)
else()
    add_library(mari_io_brush INTERFACE)
    target_link_libraries(mari_io_brush INTERFACE mari::common mari::core)
endif()

add_library(mari::io_brush ALIAS mari_io_brush)
mari_register_module(mari_io_brush)
