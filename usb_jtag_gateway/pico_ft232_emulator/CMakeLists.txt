cmake_minimum_required(VERSION 3.13)

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(pico_ft232_emulator C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(pico_ft232_emulator
    main.c
    usb_descriptors.c
)

target_include_directories(pico_ft232_emulator PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_compile_definitions(pico_ft232_emulator PRIVATE
    PICO_BOARD="pico"
)

target_link_libraries(pico_ft232_emulator
    pico_stdlib
    hardware_gpio
    tinyusb_device
    tinyusb_board   # provides board_init() — required for USB PLL setup
)

pico_enable_stdio_usb(pico_ft232_emulator 0)
pico_enable_stdio_uart(pico_ft232_emulator 0)
pico_add_extra_outputs(pico_ft232_emulator)