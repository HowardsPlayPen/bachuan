cmake_minimum_required(VERSION 3.16)
project(bachuan VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Find required packages
find_package(PkgConfig REQUIRED)
find_package(OpenSSL REQUIRED)

pkg_check_modules(LIBXML2 REQUIRED libxml-2.0)
pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
pkg_check_modules(FFMPEG REQUIRED libavcodec libavformat libavutil libswscale)

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
)

set(UTILS_SOURCES
    src/utils/logger.cpp
    src/utils/md5.cpp
)

set(ALL_SOURCES
    src/main.cpp
    ${PROTOCOL_SOURCES}
    ${CLIENT_SOURCES}
    ${VIDEO_SOURCES}
    ${UTILS_SOURCES}
)

# Main executable
add_executable(bachuan ${ALL_SOURCES})

target_include_directories(bachuan PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
    ${LIBXML2_INCLUDE_DIRS}
    ${GTK3_INCLUDE_DIRS}
    ${FFMPEG_INCLUDE_DIRS}
)

target_link_libraries(bachuan PRIVATE
    ${OPENSSL_LIBRARIES}
    ${LIBXML2_LIBRARIES}
    ${GTK3_LIBRARIES}
    ${FFMPEG_LIBRARIES}
    pthread
)

target_compile_options(bachuan PRIVATE
    ${GTK3_CFLAGS_OTHER}
    -Wall
    -Wextra
    -Wpedantic
)

# Install target
install(TARGETS bachuan RUNTIME DESTINATION bin)
