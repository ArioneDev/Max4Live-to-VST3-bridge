cmake_minimum_required(VERSION 3.22)

project(M4LBridge VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Point this at a JUCE 8 checkout.  FetchContent also works, see README.
if(NOT DEFINED JUCE_SOURCE_DIR)
    set(JUCE_SOURCE_DIR "D:/AP Timbre Resynth/build/_deps/juce-src" CACHE PATH "JUCE 8 source tree")
endif()

add_subdirectory("${JUCE_SOURCE_DIR}" juce)

set(M4LB_COMMON_DEFS
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
)

# ---------------------------------------------------------------------------
# Max-side shims.  They are plain VST3s that only speak shared memory; they
# are loaded by Max's [vst~] through an absolute path.
# ---------------------------------------------------------------------------
juce_add_plugin(M4LBridgeOut
    PRODUCT_NAME "M4L Bridge Out"
    COMPANY_NAME "M4LBridge"
    PLUGIN_MANUFACTURER_CODE M4lb
    PLUGIN_CODE Out1
    FORMATS VST3
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT TRUE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES "Fx"
)

target_sources(M4LBridgeOut PRIVATE Source/ShimProcessor.cpp)
target_include_directories(M4LBridgeOut PRIVATE Source)
target_compile_definitions(M4LBridgeOut PRIVATE ${M4LB_COMMON_DEFS} M4LB_SHIM_MODE=0)
target_link_libraries(M4LBridgeOut PRIVATE
    juce::juce_audio_utils
    juce::juce_recommended_config_flags
)

juce_add_plugin(M4LBridgeIn
    PRODUCT_NAME "M4L Bridge In"
    COMPANY_NAME "M4LBridge"
    PLUGIN_MANUFACTURER_CODE M4lb
    PLUGIN_CODE In01
    FORMATS VST3
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES "Fx"
)

target_sources(M4LBridgeIn PRIVATE Source/ShimProcessor.cpp)
target_include_directories(M4LBridgeIn PRIVATE Source)
target_compile_definitions(M4LBridgeIn PRIVATE ${M4LB_COMMON_DEFS} M4LB_SHIM_MODE=1)
target_link_libraries(M4LBridgeIn PRIVATE
    juce::juce_audio_utils
    juce::juce_recommended_config_flags
)

# ---------------------------------------------------------------------------
# The host-side plugin.
# ---------------------------------------------------------------------------
juce_add_plugin(M4LBridge
    PRODUCT_NAME "M4L Bridge"
    COMPANY_NAME "M4LBridge"
    PLUGIN_MANUFACTURER_CODE M4lb
    PLUGIN_CODE Brg1
    FORMATS VST3 Standalone
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT TRUE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES "Fx"
)

target_sources(M4LBridge PRIVATE
    Source/BridgeProcessor.cpp
    Source/PatchGenerator.cpp
)

target_include_directories(M4LBridge PRIVATE Source)
target_compile_definitions(M4LBridge PRIVATE ${M4LB_COMMON_DEFS})
target_link_libraries(M4LBridge PRIVATE
    juce::juce_audio_utils
    juce::juce_gui_basics
    juce::juce_recommended_config_flags
)

# ---------------------------------------------------------------------------
# Instrument build (loads in FL's channel rack / generator slot).
# ---------------------------------------------------------------------------
juce_add_plugin(M4LBridgeInstrument
    PRODUCT_NAME "M4L Bridge Instrument"
    COMPANY_NAME "M4LBridge"
    PLUGIN_MANUFACTURER_CODE M4lb
    PLUGIN_CODE Ins1
    FORMATS VST3
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES "Instrument"
)

target_sources(M4LBridgeInstrument PRIVATE
    Source/BridgeProcessor.cpp
    Source/PatchGenerator.cpp
)

target_include_directories(M4LBridgeInstrument PRIVATE Source)
target_compile_definitions(M4LBridgeInstrument PRIVATE ${M4LB_COMMON_DEFS} M4LBRIDGE_INSTRUMENT=1)
target_link_libraries(M4LBridgeInstrument PRIVATE
    juce::juce_audio_utils
    juce::juce_gui_basics
    juce::juce_recommended_config_flags
)

# ---------------------------------------------------------------------------
# Offline transport test (no host, no audio device needed).
# ---------------------------------------------------------------------------
add_executable(M4LBTest tools/loopback_test/main.cpp)
target_include_directories(M4LBTest PRIVATE Source)
target_compile_definitions(M4LBTest PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

add_executable(M4LBPatchGen tools/patchgen/main.cpp Source/PatchGenerator.cpp)
target_include_directories(M4LBPatchGen PRIVATE Source)
target_compile_definitions(M4LBPatchGen PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(M4LBPatchGen PRIVATE juce::juce_core)

# Minimal VST3 host: loads M4L Bridge.vst3 the way a DAW would.
add_executable(M4LHostTest tools/host_test/main.cpp)
target_compile_definitions(M4LHostTest PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN
    JUCE_PLUGINHOST_VST3=1
    JUCE_MODAL_LOOPS_PERMITTED=1
)
target_link_libraries(M4LHostTest PRIVATE
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_gui_basics
    juce::juce_recommended_config_flags
)

