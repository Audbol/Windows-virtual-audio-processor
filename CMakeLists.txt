cmake_minimum_required(VERSION 3.22)

# Static MSVC runtime: VocalBridge.exe / vbsetup.exe run without the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(VocalBridgeSuite VERSION 0.1.0 LANGUAGES C CXX)

enable_testing()

add_subdirectory(app)

if(WIN32)
    add_subdirectory(tools/vbsetup)
endif()
