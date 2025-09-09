# mp3player/micropython.cmake
add_library(usermod_mp3player INTERFACE)

target_sources(usermod_mp3player INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/mp3player.c
    ${CMAKE_CURRENT_LIST_DIR}/ring_buffer.c
    ${CMAKE_CURRENT_LIST_DIR}/mp3_decode_minimp3.c
    ${CMAKE_CURRENT_LIST_DIR}/audio_out_pwm.c
    ${CMAKE_CURRENT_LIST_DIR}/vfs_bridge.c
)

target_include_directories(usermod_mp3player INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(usermod_mp3player INTERFACE
    hardware_pwm
    hardware_dma
    hardware_irq
    hardware_clocks
)

# Link into MicroPython's usermod umbrella
target_link_libraries(usermod INTERFACE usermod_mp3player)
