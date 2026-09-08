cmake_minimum_required(VERSION 3.16)
project(baichuan VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# URL shown in the command-line --help for further documentation.
# Change here (or pass -DHELP_URL=... at configure time) before compilation.
set(HELP_URL "https://github.com/HowardsPlayPen/bachuan"
    CACHE STRING "Project help/documentation URL shown in --help output")

# Find required packages
find_package(PkgConfig REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# Platform-specific networking libraries and defines.
# On Windows: Winsock2 (ws2_32); WIN32_LEAN_AND_MEAN avoids the winsock.h vs
# winsock2.h clash when GTK pulls in <windows.h>; _WIN32_WINNT=0x0A00 (Win10)
# is required for WSAPoll.
set(PLATFORM_NET_LIBS "")
if(WIN32)
    set(PLATFORM_NET_LIBS ws2_32)
    add_compile_definitions(WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
endif()

pkg_check_modules(LIBXML2 REQUIRED libxml-2.0)
pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
pkg_check_modules(FFMPEG REQUIRED libavcodec libavformat libavutil libswscale)
pkg_check_modules(LIBJPEG REQUIRED libjpeg)

# Source files
set(PROTOCOL_SOURCES
    src/protocol/bc_header.cpp
    src/protocol/bc_crypto.cpp
    src/protocol/bc_xml.cpp
    src/protocol/bc_media.cpp
)

set(CLIENT_SOURCES
    src/client/connection.cpp
    src/client/auth.cpp
    src/client/stream.cpp
)

set(VIDEO_SOURCES
    src/video/decoder.cpp
    src/video/display.cpp
    src/video/writer.cpp
    src/rtsp/rtsp_source.cpp
    src/mjpeg/mjpeg_source.cpp
)

set(DASHBOARD_VIDEO_SOURCES
    src/video/decoder.cpp
    src/video/dashboard_display.cpp
    src/rtsp/rtsp_source.cpp
    src/mjpeg/mjpeg_source.cpp
    src/control/command_server.cpp
)

set(UTILS_SOURCES
    src/utils/logger.cpp
    src/utils/md5.cpp
    src/utils/net_compat.cpp
    src/utils/onvif.cpp
)

# Common sources (shared between baichuan and dashboard)
set(COMMON_SOURCES
    ${PROTOCOL_SOURCES}
    ${CLIENT_SOURCES}
    ${UTILS_SOURCES}
)

# Bachuan (single camera viewer)
set(BACHUAN_SOURCES
    src/main.cpp
    ${COMMON_SOURCES}
    ${VIDEO_SOURCES}
)

# Dashboard (multi-camera viewer)
set(DASHBOARD_SOURCES
    src/dashboard_main.cpp
    ${COMMON_SOURCES}
    ${DASHBOARD_VIDEO_SOURCES}
)

# Main executable
add_executable(baichuan ${BACHUAN_SOURCES})

target_include_directories(baichuan PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
    ${LIBXML2_INCLUDE_DIRS}
    ${GTK3_INCLUDE_DIRS}
    ${FFMPEG_INCLUDE_DIRS}
    ${LIBJPEG_INCLUDE_DIRS}
)

target_link_libraries(baichuan PRIVATE
    ${OPENSSL_LIBRARIES}
    ${LIBXML2_LIBRARIES}
    ${GTK3_LIBRARIES}
    ${FFMPEG_LIBRARIES}
    ${LIBJPEG_LIBRARIES}
    Threads::Threads
    ${PLATFORM_NET_LIBS}
)

target_compile_options(baichuan PRIVATE
    ${GTK3_CFLAGS_OTHER}
    -Wall
    -Wextra
    -Wpedantic
)

# Dashboard executable
add_executable(dashboard ${DASHBOARD_SOURCES})

target_include_directories(dashboard PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
    ${LIBXML2_INCLUDE_DIRS}
    ${GTK3_INCLUDE_DIRS}
    ${FFMPEG_INCLUDE_DIRS}
    ${LIBJPEG_INCLUDE_DIRS}
)

target_link_libraries(dashboard PRIVATE
    ${OPENSSL_LIBRARIES}
    ${LIBXML2_LIBRARIES}
    ${GTK3_LIBRARIES}
    ${FFMPEG_LIBRARIES}
    ${LIBJPEG_LIBRARIES}
    Threads::Threads
    ${PLATFORM_NET_LIBS}
)

target_compile_definitions(dashboard PRIVATE HELP_URL="${HELP_URL}")

target_compile_options(dashboard PRIVATE
    ${GTK3_CFLAGS_OTHER}
    -Wall
    -Wextra
    -Wpedantic
)

# Install targets
install(TARGETS baichuan dashboard RUNTIME DESTINATION bin)
