cmake_minimum_required (VERSION 3.5)
project(cxxlab VERSION 0.1.0 LANGUAGES CXX C)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "This project only supports Linux. Detected system: ${CMAKE_SYSTEM_NAME}")
endif()

# --- Global compiler settings ---
find_program(CCACHE ccache)
if(CCACHE)
    set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE})
    set(CMAKE_C_COMPILER_LAUNCHER ${CCACHE})
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

# --- Feature detection ---
include(DetectSIMD)
include(ConfigureChecks)

# --- Testing ---
enable_testing()

# --- Build options: -DTEST=ON/OFF controls test compilation ---
option(TEST "Build and run tests" ON)
option(EXAMPLE "Build and run examples" ON)

# --- System dependencies (Boost, fmt, ...) ---
include(FindDependencies)

# --- RPATH configuration ---
set(CMAKE_INSTALL_RPATH "$ORIGIN/../lib/cxxlab-thirdparty")
set(CMAKE_BUILD_WITH_INSTALL_RPATH FALSE)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

# --- Third-party libraries (Git Submodules) ---
include(BuildGoogleTest)
include(BuildSpdlog)
include(BuildISAL)
include(BuildRocksDB)

# --- Common library ---
add_subdirectory(common)

# --- Block device library ---
add_subdirectory(blk)

# --- KV storage library ---
add_subdirectory(kv)

# --- BlueFS user-space filesystem ---
add_subdirectory(bluefs)

# --- BlueStore library ---
add_subdirectory(bluestore)

# --- BTier tiered storage engine ---
add_subdirectory(btier)

# --- Examples ---
if(EXAMPLE)
    add_subdirectory(examples)
endif()

# --- Tests ---
if(TEST)
    add_subdirectory(tests)
endif()
