cmake_minimum_required(VERSION 3.20)
project(archipelago_bridge C)
if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Build this plugin for Windows x64.")
endif()
add_library(archipelago_bridge SHARED bridge.c)
target_link_libraries(archipelago_bridge PRIVATE ws2_32)
set_target_properties(archipelago_bridge PROPERTIES PREFIX "" C_STANDARD 11
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(MSVC)
    target_compile_options(archipelago_bridge PRIVATE /W4 /WX /utf-8)
endif()
add_custom_command(TARGET archipelago_bridge POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/mod.ini" "$<TARGET_FILE_DIR:archipelago_bridge>/mod.ini"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/bridge.ini" "$<TARGET_FILE_DIR:archipelago_bridge>/bridge.ini"
    COMMAND ${CMAKE_COMMAND} -E echo "Plugin folder: $<TARGET_FILE_DIR:archipelago_bridge>")
