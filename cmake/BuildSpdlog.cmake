# spdlog - shared library, no tests/examples/benchmarks
set(SPDLOG_BUILD_SHARED ON CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(SPDLOG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/spdlog")
if(EXISTS "${SPDLOG_DIR}/CMakeLists.txt")
    add_subdirectory(third_party/spdlog)
else()
    message(FATAL_ERROR "spdlog submodule not found at ${SPDLOG_DIR}!
    Please run: git submodule update --init --recursive")
endif()
