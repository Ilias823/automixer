cmake_minimum_required(VERSION 3.22)
# Must come before project(): universal binaries (Apple Silicon + Intel).
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "")
project(Automixer VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(juce
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.15
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(juce)

# Core DSP, no JUCE: shared by the plugin, the CLI and the tests.
add_library(am_core STATIC
    src/core/GroupBus.cpp
    src/core/AutomixChannel.cpp)
target_include_directories(am_core PUBLIC src/core)
set_target_properties(am_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

juce_add_plugin(Automixer
    COMPANY_NAME "Ilias823"
    BUNDLE_ID com.ilias823.automixer
    PLUGIN_MANUFACTURER_CODE Ilas
    PLUGIN_CODE Amx1
    FORMATS VST3
    PRODUCT_NAME "Automixer"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    VST3_CATEGORIES Fx Dynamics
    COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(Automixer PRIVATE
    src/plugin/PluginProcessor.cpp
    src/plugin/PluginEditor.cpp
    src/plugin/Updater.cpp)
target_compile_definitions(Automixer PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)
target_link_libraries(Automixer
    PRIVATE am_core juce::juce_audio_utils juce::juce_audio_processors
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

juce_add_console_app(automix_cli PRODUCT_NAME "automix_cli")
target_sources(automix_cli PRIVATE tools/automix_cli.cpp)
target_compile_definitions(automix_cli PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(automix_cli PRIVATE am_core juce::juce_audio_formats juce::juce_core
    juce::juce_recommended_config_flags)

add_executable(am_tests tests/test_core.cpp)
target_link_libraries(am_tests PRIVATE am_core)

# Loads the built VST3 like a host and checks linking, gain-sharing and state.
juce_add_console_app(host_check PRODUCT_NAME "host_check")
target_sources(host_check PRIVATE tools/host_check.cpp)
target_compile_definitions(host_check PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_PLUGINHOST_VST3=1 JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(host_check PRIVATE juce::juce_audio_processors juce::juce_events
    juce::juce_recommended_config_flags)

# Prints the live state of a group bus (debugging the link inside a host).
add_executable(bus_monitor tools/bus_monitor.cpp)
target_link_libraries(bus_monitor PRIVATE am_core)

# Measures how two lav recordings line up (installed with the plugin).
juce_add_console_app(sync_check PRODUCT_NAME "automixer-sync-check")
target_sources(sync_check PRIVATE tools/sync_check.cpp)
target_compile_definitions(sync_check PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(sync_check PRIVATE juce::juce_audio_formats juce::juce_dsp juce::juce_core
    juce::juce_recommended_config_flags)
