# Host build for the Sircadian firmware tests and the deployment simulator.
#
# This is a HOST-ONLY convenience: it builds and runs the C++ tests and the
# sim on a desktop. The Arduino IDE and PlatformIO ignore this file entirely,
# so shipped firmware and library consumers are unaffected.
#
#   cmake -B build
#   cmake --build build
#   ctest --test-dir build --output-on-failure
#
# On Windows, configure with the g++ toolchain to match the project's usual
# build, e.g.  cmake -B build -G "MinGW Makefiles"  (or -G Ninja).
#
# The src/ and sim/ source lists below are the single source of truth: adding
# a file to either set means editing exactly one list here.

cmake_minimum_required(VERSION 3.16)
project(Sircadian CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to an optimized build (the sim was built at -O2); honour any explicit
# choice and multi-config generators.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

enable_testing()

# Warnings-as-errors, matching the g++ -Wall -Wextra -Werror build. Guarded to
# GCC/Clang so a Windows/MSVC configure still works (with its own defaults).
add_library(sircadian_warnings INTERFACE)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(sircadian_warnings INTERFACE -Wall -Wextra -Werror)
endif()

# --- core firmware (src/) : declared once ------------------------------------
add_library(sircadian STATIC
    src/SolarModel.cpp
    src/SolarFit.cpp
    src/DriftClock.cpp
    src/Discipline.cpp
    src/Sircadian.cpp)
target_include_directories(sircadian PUBLIC src)
target_link_libraries(sircadian PUBLIC sircadian_warnings)

# --- deployment simulator engine (sim/) : declared once ----------------------
# The world model; excludes sim/sim.cpp (the CLI main), which is its own target.
add_library(sircadian_sim STATIC
    sim/World.cpp
    sim/Deployment.cpp
    sim/Engine.cpp)
target_include_directories(sircadian_sim PUBLIC sim)
target_link_libraries(sircadian_sim PUBLIC sircadian)

# --- host tests --------------------------------------------------------------
# Firmware tests link the core library; the static archive means each test
# pulls only the objects it references (e.g. test_clock uses only DriftClock).
foreach(t test_fit test_clock test_facade)
    add_executable(${t} test/${t}.cpp)
    target_link_libraries(${t} PRIVATE sircadian)
    add_test(NAME ${t} COMMAND ${t})
endforeach()

# Engine sanity test links the sim engine.
add_executable(test_sim test/test_sim.cpp)
target_link_libraries(test_sim PRIVATE sircadian_sim)
add_test(NAME test_sim COMMAND test_sim)

# --- simulator CLI (not a test) ----------------------------------------------
add_executable(sim_run sim/sim.cpp)
target_link_libraries(sim_run PRIVATE sircadian_sim)
