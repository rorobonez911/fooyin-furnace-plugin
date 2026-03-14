#!/bin/bash
# Build Furnace engine as a PIC static library for plugin use
# Run this from the fooyin-furnace-plugin directory
set -e

FURNACE_DIR="${1:-./furnace}"

if [ ! -d "$FURNACE_DIR/src/engine" ]; then
    echo "Furnace source not found at $FURNACE_DIR"
    echo "Usage: $0 [path-to-furnace-source]"
    echo ""
    echo "To get Furnace source:"
    echo "  git clone --recursive https://github.com/tildearrow/furnace.git"
    exit 1
fi

echo "=== Patching Furnace for shared library compatibility ==="

# Fix missing #include <climits> in filePlayer.h (GCC 15+)
if ! grep -q '<climits>' "$FURNACE_DIR/src/engine/filePlayer.h"; then
    sed -i '/#include <thread>/i #include <climits>' "$FURNACE_DIR/src/engine/filePlayer.h"
    echo "  Patched filePlayer.h: added <climits>"
fi

# Remove thread_local (incompatible with -fPIC shared library linking)
count=$(grep -rl 'thread_local' "$FURNACE_DIR/src/engine/platform/"*.cpp 2>/dev/null | wc -l)
if [ "$count" -gt 0 ]; then
    find "$FURNACE_DIR/src/engine/platform" -name "*.cpp" \
        -exec grep -l "thread_local" {} \; | \
        while read f; do sed -i 's/thread_local //g' "$f"; done
    echo "  Patched $count platform files: removed thread_local"
fi

# Add static library target to CMakeLists.txt if not already present
if ! grep -q 'furnace_lib' "$FURNACE_DIR/CMakeLists.txt"; then
    sed -i '/^endif()$/,/^$/{
        /^$/i\
\
# Static library target for embedding in plugins\
set(LIB_SOURCES ${ENGINE_SOURCES} ${AUDIO_SOURCES})\
add_library(furnace_lib STATIC ${LIB_SOURCES})\
target_include_directories(furnace_lib SYSTEM PRIVATE ${DEPENDENCIES_INCLUDE_DIRS})\
target_compile_definitions(furnace_lib PRIVATE ${DEPENDENCIES_DEFINES})\
set_target_properties(furnace_lib PROPERTIES POSITION_INDEPENDENT_CODE ON)\
target_compile_options(furnace_lib PRIVATE -fPIC)
    }' "$FURNACE_DIR/CMakeLists.txt"
    echo "  Patched CMakeLists.txt: added furnace_lib target"
fi

echo ""
echo "=== Configuring Furnace build ==="
mkdir -p "$FURNACE_DIR/build-lib"
cd "$FURNACE_DIR/build-lib"

cmake .. \
    -DBUILD_GUI=OFF \
    -DUSE_SDL2=OFF \
    -DUSE_SNDFILE=ON \
    -DSYSTEM_LIBSNDFILE=ON \
    -DWITH_PORTAUDIO=OFF \
    -DUSE_RTMIDI=OFF \
    -DUSE_BACKWARD=OFF \
    -DWITH_JACK=OFF \
    -DWITH_LOCALE=OFF \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -G Ninja

echo ""
echo "=== Building furnace_lib ==="
ninja furnace_lib

echo ""
echo "=== Building dependencies (fmt, fftw, zlib) ==="
ninja furnace  # builds all deps as side effect

echo ""
echo "=== Done ==="
echo "Static library: $PWD/libfurnace_lib.a"
ls -lh libfurnace_lib.a
