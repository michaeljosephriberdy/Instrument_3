cmake_minimum_required(VERSION 3.16)

project(MicrotonalInstrument VERSION 0.3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)

pkg_check_modules(ALSA REQUIRED alsa)
pkg_check_modules(HIDAPI REQUIRED hidapi-hidraw)

find_package(Threads REQUIRED)

include(FetchContent)
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(json)


include_directories(
    include
    ${ALSA_INCLUDE_DIRS}
    ${HIDAPI_INCLUDE_DIRS}
)

link_directories(
    ${ALSA_LIBRARY_DIRS}
    ${HIDAPI_LIBRARY_DIRS}
)

# Phase 0–3: core + config/layout JSON + action surfaces.
# Phase 3: ConfigManager + LayoutManager (JSON) are built.
# Action/KeyActionMap layer is driven from layout JSON.
# RGBManager was retired (Phase 2): it duplicated VialController's HID
# device handling, so StartupManager talks to VialController directly.

add_executable(microtonal_instrument

    src/main.cpp
    src/engine.cpp
    src/logger.cpp
    src/instrument_state.cpp
    src/keyboard_manager.cpp
    src/keyboard_discovery.cpp
    src/keyboard_assignment.cpp
    src/breath_controller.cpp
    src/midi_engine.cpp
    src/vial_controller.cpp
    src/startup_manager.cpp
    src/usb_topology.cpp
    src/config_manager.cpp
    src/layout_manager.cpp
    src/audio_graph_manager.cpp

)

target_link_libraries(microtonal_instrument

    ${ALSA_LIBRARIES}
    ${HIDAPI_LIBRARIES}
    Threads::Threads
    nlohmann_json::nlohmann_json

)

target_compile_options(microtonal_instrument PRIVATE

    -Wall
    -Wextra
    -Wpedantic

)

# --- Pi headless audio sink overrides (added by build.sh --headless-boot) ---
if(DEFINED PI_AUDIO_LEFT_SINK)
  target_compile_definitions(microtonal_instrument PRIVATE PI_AUDIO_LEFT_SINK="${PI_AUDIO_LEFT_SINK}")
endif()
if(DEFINED PI_AUDIO_RIGHT_SINK)
  target_compile_definitions(microtonal_instrument PRIVATE PI_AUDIO_RIGHT_SINK="${PI_AUDIO_RIGHT_SINK}")
endif()

# [zynvol] MIDI-controlled volume helper for Mode 1 breath volume
pkg_check_modules(JACK QUIET jack)
if(JACK_FOUND)
  add_executable(zynvol tools/zynvol/zynvol.cpp)
  target_include_directories(zynvol PRIVATE ${JACK_INCLUDE_DIRS} ${ALSA_INCLUDE_DIRS})
  target_link_directories(zynvol PRIVATE ${JACK_LIBRARY_DIRS} ${ALSA_LIBRARY_DIRS})
  target_link_libraries(zynvol PRIVATE ${JACK_LIBRARIES} ${ALSA_LIBRARIES})
  target_compile_options(zynvol PRIVATE -Wall -Wextra)
else()
  message(WARNING "[zynvol] jack.pc not found (apt install libjack-jackd2-dev): Mode 1 will have no breath volume")
endif()
