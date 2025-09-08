# audiotone/micropython.cmake
add_library(usermod_audiotone INTERFACE)

target_sources(usermod_audiotone INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/audiotone.c
)

target_include_directories(usermod_audiotone INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(usermod_audiotone INTERFACE
    hardware_pwm
    hardware_clocks
)

# Tell MicroPython to include this user module
target_link_libraries(usermod INTERFACE usermod_audiotone)