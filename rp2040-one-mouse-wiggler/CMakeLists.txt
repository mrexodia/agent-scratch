cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD waveshare_rp2040_one CACHE STRING "Board type")

include(pico_sdk_import.cmake)

project(mouse_wiggler C CXX ASM)
set(CMAKE_C_STANDARD 11)

pico_sdk_init()

add_subdirectory(lib/wiggle)

add_executable(mouse_wiggler
    src/main.c
    src/usb_descriptors.c
)

pico_generate_pio_header(mouse_wiggler ${CMAKE_CURRENT_LIST_DIR}/src/ws2812.pio)

target_include_directories(mouse_wiggler PRIVATE src)
target_link_libraries(mouse_wiggler PRIVATE
    pico_stdlib
    pico_rand
    hardware_pio
    hardware_flash
    hardware_sync
    tinyusb_device
    wiggle
)

# No stdio: the only USB interface is the mouse.
pico_enable_stdio_usb(mouse_wiggler 0)
pico_enable_stdio_uart(mouse_wiggler 0)

pico_add_extra_outputs(mouse_wiggler)
