# Doom VR for Quest and Steam Frame (standalone): the libraries the engine
# needs that are not in this repository, built from their release sources with
# the NDK's CMake. The engine itself is configured from the repository's root
# (tools/build-quest.ps1 does both), because UZDoom's own CMake assumes it IS
# the root project.
#
#   EXT_SRC   folder holding SDL2/ and openal-soft-1.25.2/ (release sources)
#
# Outputs, in this build folder:
#   SDL2/libSDL2.so            SDL's Java side loads it by name
#   openal/libopenal.so        the engine dlopens it by name (DYN_OPENAL)
#   sdl2-include/SDL2/*.h      the headers as the engine includes them (<SDL2/SDL.h>)

cmake_minimum_required(VERSION 3.18)
project(doom_vr_android_deps C CXX)

get_filename_component(REPO "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(EXT_SRC "${REPO}/../../Android/src" CACHE PATH "Third-party release sources")
set(BUILD_SHARED_LIBS OFF)

# --- SDL2 (shared: SDL's Java side loads it by name) ---------------------------
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL2_DISABLE_UNINSTALL ON CACHE BOOL "" FORCE)
add_subdirectory("${EXT_SRC}/SDL2" SDL2)

# The engine includes <SDL2/SDL.h>; SDL's source tree has include/SDL.h. One
# folder with an SDL2/ inside it, filled from the source tree at configure
# time (its SDL_config.h picks SDL_config_android.h by itself on Android).
file(GLOB SDL2_HEADERS "${EXT_SRC}/SDL2/include/*.h")
file(COPY ${SDL2_HEADERS} DESTINATION "${CMAKE_BINARY_DIR}/sdl2-include/SDL2")

# --- OpenAL Soft (shared: libopenal.so) ---------------------------------------
set(LIBTYPE SHARED CACHE STRING "" FORCE)
set(ALSOFT_UTILS OFF CACHE BOOL "" FORCE)
set(ALSOFT_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ALSOFT_TESTS OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_CONFIG OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_HRTF_DATA OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_AMBDEC_PRESETS OFF CACHE BOOL "" FORCE)
add_subdirectory("${EXT_SRC}/openal-soft-1.25.2" openal)
