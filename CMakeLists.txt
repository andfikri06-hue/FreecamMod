cmake_minimum_required(VERSION 3.21)

project(FreecamMod VERSION 1.0.0 LANGUAGES CXX)

# ── C++ standard ──────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD          23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

# ── Output: shared library (.so on Linux) ─────────────────────────────────────
add_library(FreecamMod SHARED
    src/FreecamMod.cpp
)

target_include_directories(FreecamMod
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# ── LeviLamina / BDS SDK ──────────────────────────────────────────────────────
#
#  With xmake+CPM or the official LeviLamina CMake package this block is
#  replaced by:
#      find_package(LeviLamina REQUIRED)
#      target_link_libraries(FreecamMod PRIVATE levilamina::levilamina)
#
#  The variables below mirror the conventional layout produced by the LL
#  CMake integration scripts.  Adjust LEVILAMINA_SDK_DIR to point at your
#  extracted BDS + LL header/lib bundle.

if(NOT DEFINED LEVILAMINA_SDK_DIR)
    set(LEVILAMINA_SDK_DIR "$ENV{LEVILAMINA_SDK_DIR}"
        CACHE PATH "Root of the LeviLamina SDK (headers + import libs)")
endif()

if(NOT LEVILAMINA_SDK_DIR)
    message(FATAL_ERROR
        "LEVILAMINA_SDK_DIR is not set.  "
        "Pass -DLEVILAMINA_SDK_DIR=/path/to/sdk to cmake or export the env var.")
endif()

target_include_directories(FreecamMod
    SYSTEM PRIVATE
        "${LEVILAMINA_SDK_DIR}/include"
)

target_link_directories(FreecamMod PRIVATE
    "${LEVILAMINA_SDK_DIR}/lib"
)

# LeviLamina exports its symbols from bedrock_server (or a stub import lib).
# The exact library name depends on your SDK build; adjust if needed.
target_link_libraries(FreecamMod PRIVATE
    LeviLamina          # ll:: namespace, event bus, command registrar …
    bedrock_server_api  # Bedrock server symbols (Player, Level, SimulatedPlayer …)
)

# ── Compiler flags ────────────────────────────────────────────────────────────
target_compile_options(FreecamMod PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -fvisibility=hidden     # hide all symbols by default; LL macros export what's needed
    -fno-rtti               # BDS is built without RTTI; match it
)

# Position-independent code is required for shared libraries on Linux
set_target_properties(FreecamMod PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    PREFIX ""                    # output: FreecamMod.so  (not libFreecamMod.so)
)

# ── Install ───────────────────────────────────────────────────────────────────
install(TARGETS FreecamMod
    LIBRARY DESTINATION plugins/FreecamMod
)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/manifest.json"
    DESTINATION plugins/FreecamMod
)
