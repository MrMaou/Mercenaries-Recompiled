cmake_minimum_required(VERSION 3.24)
project(archipelago_plugin C CXX)
if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Build this plugin for Windows x64.")
endif()

option(ARCHIPELAGO_SSL "Support wss:// servers (needs OpenSSL, e.g. vcpkg openssl:x64-windows-static)" ON)

# Header-only network stack, pinned. SOURCE_SUBDIR points at a missing folder so
# FetchContent only downloads; nothing of theirs is configured.
include(FetchContent)
FetchContent_Declare(apclientpp GIT_REPOSITORY https://github.com/black-sliver/apclientpp.git
    GIT_TAG 6b28cc1781006deae0e28c61752d11a83819585a SOURCE_SUBDIR _none)
FetchContent_Declare(wswrap GIT_REPOSITORY https://github.com/black-sliver/wswrap.git
    GIT_TAG aeba7ac428028723fb26ce92488f260660f786b1 SOURCE_SUBDIR _none)
# websocketpp 0.8.2 needs an asio that still has io_service: 1.28 is the newest.
FetchContent_Declare(websocketpp GIT_REPOSITORY https://github.com/zaphoyd/websocketpp.git
    GIT_TAG 0.8.2 GIT_SHALLOW TRUE SOURCE_SUBDIR _none)
FetchContent_Declare(asio GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG asio-1-28-2 GIT_SHALLOW TRUE SOURCE_SUBDIR _none)
FetchContent_Declare(nlohmann_json GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.12.0 GIT_SHALLOW TRUE SOURCE_SUBDIR _none)
FetchContent_Declare(zlib GIT_REPOSITORY https://github.com/madler/zlib.git
    GIT_TAG v1.3.1 GIT_SHALLOW TRUE SOURCE_SUBDIR _none)
FetchContent_MakeAvailable(apclientpp wswrap websocketpp asio nlohmann_json zlib)

# zlib for permessage-deflate (servers warn about clients without it).
set(ZLIB_SOURCES adler32.c compress.c crc32.c deflate.c gzclose.c gzlib.c gzread.c gzwrite.c infback.c
    inffast.c inflate.c inftrees.c trees.c uncompr.c zutil.c)
list(TRANSFORM ZLIB_SOURCES PREPEND "${zlib_SOURCE_DIR}/")
add_library(archipelago_zlib STATIC ${ZLIB_SOURCES})
target_include_directories(archipelago_zlib PUBLIC "${zlib_SOURCE_DIR}")
target_compile_definitions(archipelago_zlib PRIVATE _CRT_SECURE_NO_DEPRECATE _CRT_NONSTDC_NO_DEPRECATE)
set_target_properties(archipelago_zlib PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(archipelago SHARED
    src/plugin_main.cpp src/ap_session.cpp src/game_logic.cpp src/panel.cpp src/toasts.cpp src/socket_bridge.c)
set_target_properties(archipelago PROPERTIES PREFIX "" CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON C_STANDARD 11
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_include_directories(archipelago PRIVATE
    "${apclientpp_SOURCE_DIR}" "${wswrap_SOURCE_DIR}/include" "${websocketpp_SOURCE_DIR}"
    "${asio_SOURCE_DIR}/asio/include" "${nlohmann_json_SOURCE_DIR}/include")
target_compile_definitions(archipelago PRIVATE ASIO_STANDALONE _WEBSOCKETPP_CPP11_STL_ _WEBSOCKETPP_CPP11_THREAD_ AP_NO_SCHEMA
    _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(archipelago PRIVATE archipelago_zlib ws2_32 mswsock crypt32 shell32 comctl32 user32 gdi32
    advapi32)
if(MSVC)
    target_compile_options(archipelago PRIVATE /W3 /utf-8 /bigobj /EHsc)
endif()

set(ARCHIPELAGO_EXTRA_FILES "${CMAKE_CURRENT_SOURCE_DIR}/mod.ini" "${CMAKE_CURRENT_SOURCE_DIR}/archipelago.ini")
if(ARCHIPELAGO_SSL)
    find_package(OpenSSL REQUIRED)
    target_link_libraries(archipelago PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    # CA bundle for certificate checks (curl's extract of Mozilla's store).
    set(CACERT "${CMAKE_BINARY_DIR}/cacert.pem")
    if(NOT EXISTS "${CACERT}")
        file(DOWNLOAD https://curl.se/ca/cacert.pem "${CACERT}" TLS_VERIFY ON STATUS cacert_status)
        list(GET cacert_status 0 cacert_code)
        if(NOT cacert_code EQUAL 0)
            message(FATAL_ERROR "Could not download https://curl.se/ca/cacert.pem; place it at ${CACERT}.")
        endif()
    endif()
    list(APPEND ARCHIPELAGO_EXTRA_FILES "${CACERT}")
else()
    target_compile_definitions(archipelago PRIVATE WSWRAP_NO_SSL)
    message(WARNING "Building without SSL: wss:// servers (archipelago.gg) will not work.")
endif()

add_custom_command(TARGET archipelago POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ARCHIPELAGO_EXTRA_FILES} "$<TARGET_FILE_DIR:archipelago>"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:archipelago>/licenses"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${apclientpp_SOURCE_DIR}/LICENSE"
        "$<TARGET_FILE_DIR:archipelago>/licenses/apclientpp.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${wswrap_SOURCE_DIR}/LICENSE"
        "$<TARGET_FILE_DIR:archipelago>/licenses/wswrap.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${websocketpp_SOURCE_DIR}/COPYING"
        "$<TARGET_FILE_DIR:archipelago>/licenses/websocketpp.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${asio_SOURCE_DIR}/asio/LICENSE_1_0.txt"
        "$<TARGET_FILE_DIR:archipelago>/licenses/asio.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${nlohmann_json_SOURCE_DIR}/LICENSE.MIT"
        "$<TARGET_FILE_DIR:archipelago>/licenses/nlohmann-json.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${zlib_SOURCE_DIR}/LICENSE"
        "$<TARGET_FILE_DIR:archipelago>/licenses/zlib.txt"
    COMMAND ${CMAKE_COMMAND} -E echo "Plugin folder: $<TARGET_FILE_DIR:archipelago>")
