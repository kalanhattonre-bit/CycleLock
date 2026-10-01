cmake_minimum_required(VERSION 3.22)

project(CycleLock VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CYCLELOCK_BUILD_PLUGIN "Build the CycleLock VST3 plugin" ON)
option(CYCLELOCK_BUILD_TESTS  "Build the offline CycleLock test runner" ON)

# Static MSVC runtime: the plugin then needs no Visual C++ redistributable.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# JUCE is pinned to one JUCE 8 release so every build uses identical framework code.
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.15
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE)
FetchContent_MakeAvailable(JUCE)

# Sources shared by the plugin and the test runner, so the tests exercise the shipped code.
set(CYCLELOCK_SOURCES
    Source/Parameters.cpp
    Source/Presets.cpp
    Source/dsp/PitchTracker.cpp
    Source/dsp/PitchMarks.cpp
    Source/dsp/ShiftVoice.cpp
    Source/dsp/CycleEngine.cpp
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/ui/CycleLookAndFeel.cpp
    Source/ui/Controls.cpp
    Source/ui/Display.cpp
    Source/ui/MainPanel.cpp)

set(CYCLELOCK_DEFINITIONS
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

if(CYCLELOCK_BUILD_PLUGIN)
    juce_add_plugin(CycleLock
        COMPANY_NAME                "Kalan Hatton"
        COMPANY_WEBSITE             "https://github.com/kalanhattonre-bit/CycleLock"
        BUNDLE_ID                   com.kalanhatton.cyclelock
        PLUGIN_MANUFACTURER_CODE    Khtn
        PLUGIN_CODE                 Cylk
        FORMATS                     VST3
        PRODUCT_NAME                "CycleLock"
        VERSION                     ${PROJECT_VERSION}
        IS_SYNTH                    FALSE
        NEEDS_MIDI_INPUT            TRUE
        NEEDS_MIDI_OUTPUT           FALSE
        IS_MIDI_EFFECT              FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS FALSE
        COPY_PLUGIN_AFTER_BUILD     FALSE
        VST3_CATEGORIES             Fx)

    target_sources(CycleLock PRIVATE ${CYCLELOCK_SOURCES})
    target_include_directories(CycleLock PRIVATE Source)
    target_compile_definitions(CycleLock PUBLIC ${CYCLELOCK_DEFINITIONS})
    target_link_libraries(CycleLock
        PRIVATE
            juce::juce_audio_processors
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)
endif()

if(CYCLELOCK_BUILD_TESTS)
    juce_add_console_app(CycleLockTests PRODUCT_NAME "CycleLockTests")

    target_sources(CycleLockTests PRIVATE Tests/TestMain.cpp ${CYCLELOCK_SOURCES})
    target_include_directories(CycleLockTests PRIVATE Source Tests)
    target_compile_definitions(CycleLockTests PRIVATE ${CYCLELOCK_DEFINITIONS} CYCLELOCK_TESTS=1)
    target_link_libraries(CycleLockTests
        PRIVATE
            juce::juce_audio_processors
            juce::juce_dsp
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)
endif()
