cmake_minimum_required(VERSION 3.16)
project(pcvrxr CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W4 /permissive-)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()

# Which C runtime to link against.
#
# It has to match the program this is compiled into, exactly: MSVC refuses to
# link a static-CRT object with a dynamic-CRT one, and the error names
# 'RuntimeLibrary' rather than saying so. Xash3D builds with the static CRT, so
# a port into it needs -DPCVRXR_STATIC_CRT=ON.
option(PCVRXR_STATIC_CRT "Link the static C runtime (/MT), to match a host that does" OFF)
if(MSVC AND PCVRXR_STATIC_CRT)
    cmake_policy(SET CMP0091 NEW)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    message(STATUS "pcvr-xr: static CRT (/MT)")
endif()

# The OpenXR loader, vendored so a port never has to go and find one - and
# vendored for BOTH architectures, because a layer that is only 64-bit is no use
# to the ports that need it most. GoldSrc, Xash3D and every engine that loads
# x86 game DLLs is a 32-bit process, and so is anything built on them.
set(PCVRXR_OPENXR "${CMAKE_CURRENT_SOURCE_DIR}/extern/openxr")
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(PCVRXR_ARCH x64)
else()
    set(PCVRXR_ARCH x86)
endif()
message(STATUS "pcvr-xr: building ${PCVRXR_ARCH}")

add_library(pcvrxr STATIC
    src/pcvrxr.cpp
    src/pcvrxr_input.cpp
    src/pcvrxr_panel.cpp
    src/pcvrxr_spectator.cpp)
target_include_directories(pcvrxr
    PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${PCVRXR_OPENXR}/include")
target_link_libraries(pcvrxr PUBLIC
    "${PCVRXR_OPENXR}/lib/${PCVRXR_ARCH}/openxr_loader.lib"
    opengl32 d3d11 dxgi)

# The Unity bridge: a DLL with a flat C ABI, for C# mods of Unity games
# (Direct3D 11). Built with the static CRT so a player needs no runtime installed.
option(PCVRXR_UNITY "Build the Unity bridge DLL (pcvrxr_unity.dll)" OFF)
if(PCVRXR_UNITY)
    add_library(pcvrxr_unity SHARED unity/pcvrxr_unity.cpp)
    target_link_libraries(pcvrxr_unity PRIVATE pcvrxr)
    add_custom_command(TARGET pcvrxr_unity POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PCVRXR_OPENXR}/bin/${PCVRXR_ARCH}/openxr_loader.dll"
            "$<TARGET_FILE_DIR:pcvrxr_unity>")
endif()

# The probe: no engine, no game data, no headset required to build it.
add_executable(probe probe/probe.cpp)
target_link_libraries(probe PRIVATE pcvrxr)

# The spectator view's maths, checked with no headset (tools/spectator_test.cpp).
add_executable(spectator_test tools/spectator_test.cpp)
target_link_libraries(spectator_test PRIVATE pcvrxr)

# openxr_loader.dll has to sit beside whatever links the loader.
add_custom_command(TARGET probe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PCVRXR_OPENXR}/bin/${PCVRXR_ARCH}/openxr_loader.dll"
        "$<TARGET_FILE_DIR:probe>")
