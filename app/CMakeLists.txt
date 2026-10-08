cmake_minimum_required(VERSION 3.22)

project(VocalBridge VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# JUCE: use a local checkout if given, otherwise download the pinned release.
# ---------------------------------------------------------------------------
set(VOCALBRIDGE_JUCE_DIR "" CACHE PATH "Path to a local JUCE checkout (leave empty to download JUCE 8.0.9)")

if(VOCALBRIDGE_JUCE_DIR)
    add_subdirectory(${VOCALBRIDGE_JUCE_DIR} ${CMAKE_BINARY_DIR}/JUCE EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        8.0.9
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

# ---------------------------------------------------------------------------
# Optional ASIO support (lowest-latency monitoring on most audio interfaces).
# Download the ASIO SDK from Steinberg and point this at the extracted folder
# (the one that contains common/iasiodrv.h).
# ---------------------------------------------------------------------------
set(VOCALBRIDGE_ASIO_SDK_DIR "" CACHE PATH "Path to the Steinberg ASIO SDK (enables ASIO)")

juce_add_gui_app(VocalBridge
    PRODUCT_NAME "VocalBridge"
    COMPANY_NAME "VocalBridge"
    BUNDLE_ID    "com.vocalbridge.app"
    VERSION      ${PROJECT_VERSION}
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "VocalBridge processes your microphone in real time.")

target_sources(VocalBridge PRIVATE
    Source/Main.cpp
    Source/AudioEngine.cpp
    Source/AudioEngine.h
    Source/MainComponent.cpp
    Source/MainComponent.h
    Source/PluginChain.cpp
    Source/PluginChain.h
    Source/StreamingResampler.h
    Source/VirtualMicOutput.cpp
    Source/VirtualMicOutput.h
    ../shared/VocalBridgeShared.h)

target_include_directories(VocalBridge PRIVATE ../shared)

target_compile_definitions(VocalBridge PRIVATE
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:VocalBridge,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:VocalBridge,JUCE_VERSION>"
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_LADSPA=0
    JUCE_WASAPI=1
    JUCE_DIRECTSOUND=0
    JUCE_JACK=0)

if(WIN32 AND VOCALBRIDGE_ASIO_SDK_DIR)
    if(NOT EXISTS "${VOCALBRIDGE_ASIO_SDK_DIR}/common/iasiodrv.h")
        message(FATAL_ERROR "VOCALBRIDGE_ASIO_SDK_DIR does not contain common/iasiodrv.h")
    endif()
    target_include_directories(VocalBridge PRIVATE "${VOCALBRIDGE_ASIO_SDK_DIR}/common")
    target_compile_definitions(VocalBridge PRIVATE JUCE_ASIO=1)
    message(STATUS "VocalBridge: ASIO support enabled")
else()
    target_compile_definitions(VocalBridge PRIVATE JUCE_ASIO=0)
endif()

target_link_libraries(VocalBridge
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_audio_devices
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------------------
# Offline tests for the virtual mic path (resampler + drift compensation).
# ---------------------------------------------------------------------------
option(VOCALBRIDGE_BUILD_TESTS "Build VocalBridge unit tests" ON)

if(VOCALBRIDGE_BUILD_TESTS)
    juce_add_console_app(VocalBridgeTests PRODUCT_NAME "VocalBridgeTests")

    target_sources(VocalBridgeTests PRIVATE
        Tests/VirtualMicOutputTests.cpp
        Source/VirtualMicOutput.cpp)

    target_include_directories(VocalBridgeTests PRIVATE ../shared)

    target_compile_definitions(VocalBridgeTests PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_ASIO=0
        JUCE_JACK=0)

    target_link_libraries(VocalBridgeTests
        PRIVATE
            juce::juce_audio_devices
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)

    enable_testing()
    add_test(NAME VocalBridgeTests COMMAND VocalBridgeTests)
endif()
