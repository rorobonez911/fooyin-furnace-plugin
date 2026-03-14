cmake_minimum_required(VERSION 3.18)
project(FooyinFurnacePlugin VERSION 0.1.0 DESCRIPTION "Furnace tracker module plugin for fooyin")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Find fooyin SDK
find_package(Fooyin REQUIRED)

# Find zlib for .fur decompression
find_package(ZLIB REQUIRED)

# Find libsndfile (required by Furnace engine)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SNDFILE REQUIRED sndfile)

# Furnace engine paths - adjust these if your furnace source is elsewhere
set(FURNACE_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/furnace" CACHE PATH "Path to Furnace source tree")
set(FURNACE_BUILD_DIR "${FURNACE_SRC_DIR}/build-lib" CACHE PATH "Path to Furnace build-lib directory")

create_fooyin_plugin(
    FurnaceInput
    SOURCES
        src/furnaceinput.h
        src/furnaceinput.cpp
        src/furnaceplugin.h
        src/furnaceplugin.cpp
        src/furnaceparser.h
        src/furnaceparser.cpp
    JSON_IN
        src/furnaceinput.json.in
    DEPENDS
        Fooyin::Core
        ZLIB::ZLIB
)

# Furnace engine include directories
target_include_directories(FurnaceInput SYSTEM PRIVATE
    ${FURNACE_SRC_DIR}
    ${FURNACE_SRC_DIR}/src
    ${FURNACE_SRC_DIR}/extern/blip_buf
    ${FURNACE_SRC_DIR}/extern/fmt/include
    ${FURNACE_SRC_DIR}/extern/fftw/api
    ${FURNACE_SRC_DIR}/extern/fftw/.
    ${FURNACE_SRC_DIR}/extern/vgsound_emu-modified
    ${FURNACE_SRC_DIR}/extern/zlib
    ${FURNACE_SRC_DIR}/extern/IconFontCppHeaders
    ${FURNACE_SRC_DIR}/src/icon
    ${FURNACE_BUILD_DIR}/extern/zlib
    ${FURNACE_BUILD_DIR}/extern/fftw
)

# Furnace engine compile definitions
target_compile_definitions(FurnaceInput PRIVATE
    HAVE_SNDFILE
    HAVE_DIRENT_TYPE
)

# Link Furnace static library and its dependencies
target_link_libraries(FurnaceInput PRIVATE
    ${FURNACE_BUILD_DIR}/libfurnace_lib.a
    ${FURNACE_BUILD_DIR}/extern/fmt/libfmt.a
    ${FURNACE_BUILD_DIR}/extern/fftw/libfftw3.a
    ${FURNACE_BUILD_DIR}/extern/zlib/libz.a
    ${SNDFILE_LIBRARIES}
    pthread
    dl
    m
)
