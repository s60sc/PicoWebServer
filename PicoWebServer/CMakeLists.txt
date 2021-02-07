
add_executable(PicoWebServer PicoWSexample.cpp PicoWebServer.cpp) 
pico_generate_pio_header(PicoWebServer ${CMAKE_CURRENT_LIST_DIR}/blinkLed.pio)
target_link_libraries(PicoWebServer pico_stdlib hardware_rtc hardware_pio pico_multicore hardware_adc)
#target_link_libraries(PicoWebServer pico_stdlib hardware_rtc hardware_pio pico_multicore hardware_adc double_tap_usb_boot)
pico_enable_stdio_usb(PicoWebServer 1)
pico_enable_stdio_uart(PicoWebServer 0)
pico_add_extra_outputs(PicoWebServer)
pico_set_program_url(PicoWebServer "https://github.com/raspberrypi/pico-examples/tree/HEAD")