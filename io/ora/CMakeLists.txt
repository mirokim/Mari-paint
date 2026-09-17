# io/ora — OpenRaster(.ora) 읽기/쓰기. 네이티브 포맷.
# 시스템 zlib 을 쓴다(ZIP + PNG + XML). 외부 의존성을 fetch 하지 않는다.
# 이 파일은 io/ora 담당 에이전트가 소유한다.

set(MARI_IO_ORA_SOURCES
    src/zip.cpp
    src/xml.cpp
    src/image.cpp
    src/composite_op.cpp
    src/ora.cpp
)

find_package(ZLIB REQUIRED)
find_package(PNG REQUIRED)

if(MARI_IO_ORA_SOURCES)
    add_library(mari_io_ora STATIC ${MARI_IO_ORA_SOURCES})
    target_link_libraries(mari_io_ora PUBLIC mari::common mari::core PRIVATE PNG::PNG ZLIB::ZLIB)
else()
    add_library(mari_io_ora INTERFACE)
    target_link_libraries(mari_io_ora INTERFACE mari::common mari::core)
endif()

add_library(mari::io_ora ALIAS mari_io_ora)
mari_register_module(mari_io_ora)
