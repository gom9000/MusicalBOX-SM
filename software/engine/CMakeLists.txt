cmake_minimum_required(VERSION 3.16)
project(musicalbox_engine VERSION 0.11.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(MBX_BUILD_TESTS "Build the engine tests" ON)

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Third-party code (compiled once, warnings off)
# ---------------------------------------------------------------------------
add_library(mbx_thirdparty STATIC
    src/third_party_impl.cpp
    third_party/rtmidi/RtMidi.cpp
)
target_include_directories(mbx_thirdparty SYSTEM PUBLIC third_party)
target_link_libraries(mbx_thirdparty PUBLIC Threads::Threads ${CMAKE_DL_LIBS})

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(ALSA REQUIRED)
    target_compile_definitions(mbx_thirdparty PUBLIC __LINUX_ALSA__)
    target_link_libraries(mbx_thirdparty PUBLIC ALSA::ALSA m)
elseif(APPLE)
    target_compile_definitions(mbx_thirdparty PUBLIC __MACOSX_CORE__)
    target_link_libraries(mbx_thirdparty PUBLIC
        "-framework CoreMIDI" "-framework CoreAudio" "-framework CoreFoundation" "-framework AudioToolbox")
elseif(WIN32)
    target_compile_definitions(mbx_thirdparty PUBLIC __WINDOWS_MM__)
    target_link_libraries(mbx_thirdparty PUBLIC winmm)
endif()

# ---------------------------------------------------------------------------
# Engine core: everything that does not touch a device (testable on any PC)
# ---------------------------------------------------------------------------
add_library(mbx_core STATIC
    src/core/Log.cpp
    src/core/Engine.cpp
    src/core/Limiter.cpp
    src/core/Recorder.cpp
    src/sampler/Envelope.cpp
    src/sampler/Voice.cpp
    src/sampler/VoicePool.cpp
    src/sampler/Instrument.cpp
    src/sampler/InstrumentStore.cpp
    src/sampler/NoteName.cpp
    src/sampler/WavLoader.cpp
    src/sampler/Program.cpp
    src/midi/MidiParser.cpp
    src/midi/UartMidiInput.cpp
    src/app/BankManager.cpp
    src/app/ControlApi.cpp
    src/app/ControlServer.cpp
    src/app/Recording.cpp
    src/app/OutputRouter.cpp
    src/config/ConfigLoader.cpp
    src/config/JsonUtil.cpp
    src/config/ProgramConfig.cpp
    src/config/EngineConfig.cpp
    src/fx/Effect.cpp
    src/fx/FxChain.cpp
    src/fx/Tone.cpp
    src/fx/TapeEcho.cpp
    src/fx/Chorus.cpp
    src/fx/Phaser.cpp
    src/fx/Leslie.cpp
    src/fx/Tremolo.cpp
    src/fx/Fuzz.cpp
    src/fx/Width.cpp
    src/fx/Room.cpp
)
target_include_directories(mbx_core PUBLIC src)
target_link_libraries(mbx_core PUBLIC mbx_thirdparty)

if(MSVC)
    target_compile_options(mbx_core PRIVATE /W4)
else()
    target_compile_options(mbx_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# Device layer + executable
# ---------------------------------------------------------------------------
add_executable(mbx-engine
    src/main.cpp
    src/audio/AudioDevice.cpp
    src/midi/RtMidiInput.cpp
    src/app/OfflineRenderer.cpp
    src/app/Console.cpp
    src/gpio/GpioLines.cpp
)
target_link_libraries(mbx-engine PRIVATE mbx_core)
target_compile_definitions(mbx-engine PRIVATE MBX_VERSION="${PROJECT_VERSION}")
if(NOT MSVC)
    target_compile_options(mbx-engine PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS mbx-engine RUNTIME DESTINATION bin)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(MBX_BUILD_TESTS)
    enable_testing()
    foreach(t test_midi_parser test_note_name test_engine test_programs test_engine_config test_limiter test_fx test_uart_midi test_control test_output_router)
        add_executable(${t} tests/${t}.cpp)
        target_link_libraries(${t} PRIVATE mbx_core)
        add_test(NAME ${t} COMMAND ${t})
    endforeach()
endif()

# Offline effect render, for comparing effects on a WAV file
add_executable(fx_render tools/fx_render.cpp)
target_link_libraries(fx_render PRIVATE mbx_core)
