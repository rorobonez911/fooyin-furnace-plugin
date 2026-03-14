# fooyin Furnace Input Plugin

A plugin for [fooyin](https://github.com/fooyin/fooyin) music player that adds playback support for [Furnace tracker](https://github.com/tildearrow/furnace) modules (`.fur` files).

Uses Furnace's DivEngine for chip-accurate audio rendering with 90+ sound chip emulators.

## Features

- Full playback of `.fur` module files with real chip emulation
- Metadata extraction: title, author, system/chip info, duration
- Seeking support
- Stereo 44.1kHz float output
- 2-loop playback before stopping

## Supported Systems

All systems supported by Furnace, including:

- Yamaha FM chips (YM2612, YM2151, YM2203, YM2608, YM2610, YMF262/OPL3, etc.)
- SN76489 (SMS/Genesis PSG)
- NES (2A03), Game Boy, SNES
- Amiga (Paula), C64 (SID 6581/8580)
- PC Engine, PC Speaker
- AY-3-8910, SAA1099
- QSound, X1-010, ES5506
- ESFM, SegaPCM, MultiPCM
- ZX Spectrum Beeper
- Neo Geo (YM2610)
- Atari TIA, Lynx
- Nintendo DS, GBA
- And more

## Pre-built Plugin

If you just want to use the plugin, grab `fyplugin_furnaceinput.fyl` from the releases page and install it via fooyin's Settings > Plugins > Install Plugin.

Note: the pre-built plugin is linked against specific library versions and may only work with a source-built fooyin on the same system.

## Building from Source

### Prerequisites

- [fooyin](https://github.com/fooyin/fooyin) built from source (for SDK headers and libraries)
- CMake 3.18+, Ninja
- GCC or Clang with C++23 support
- Qt6
- zlib, libsndfile (dev packages)

### 1. Build Furnace Engine Library

Clone Furnace into this directory and use the included build script:

```bash
cd fooyin-furnace-plugin
git clone --recursive https://github.com/tildearrow/furnace.git
./build-furnace-lib.sh ./furnace
```

The script handles everything automatically:
- Patches `filePlayer.h` for GCC 15+ compatibility (`<climits>`)
- Removes `thread_local` from platform sources (required for shared library linking)
- Adds a PIC static library target (`furnace_lib`) to Furnace's CMakeLists.txt
- Configures and builds the headless engine (no GUI, no SDL, no PortAudio)

### 2. Build the Plugin

```bash
mkdir build && cd build
cmake .. -G Ninja -DCMAKE_PREFIX_PATH=/path/to/fooyin/build/run
ninja
```

If your Furnace source is somewhere other than `./furnace`, pass the paths:

```bash
cmake .. -G Ninja \
  -DCMAKE_PREFIX_PATH=/path/to/fooyin/build/run \
  -DFURNACE_SRC_DIR=/path/to/furnace \
  -DFURNACE_BUILD_DIR=/path/to/furnace/build-lib
ninja
```

### 3. Install

**Option A** - Via fooyin's GUI:

Rename the built plugin and install through Settings > Plugins > Install Plugin:

```bash
cp build/fyplugin_furnaceinput.so fyplugin_furnaceinput.fyl
```

Then select the `.fyl` file in the dialog.

**Option B** - Manual:

```bash
cp build/fyplugin_furnaceinput.so ~/.local/lib/fooyin/plugins/
```

Restart fooyin and `.fur` files will be playable.

## License

GPLv3+. This plugin links against Furnace (GPLv2+) and fooyin (GPLv3+).
