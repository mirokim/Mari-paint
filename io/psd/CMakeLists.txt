# io/psd — Adobe Photoshop .psd 읽기/쓰기(교환 포맷). 픽셀 I/O 는 io/ora 의 image 도우미를 쓴다.
set(MARI_IO_PSD_SOURCES
    src/psd.cpp
)
add_library(mari_io_psd STATIC ${MARI_IO_PSD_SOURCES})
target_link_libraries(mari_io_psd PUBLIC mari::common mari::core mari::io_ora)
add_library(mari::io_psd ALIAS mari_io_psd)
mari_register_module(mari_io_psd)
