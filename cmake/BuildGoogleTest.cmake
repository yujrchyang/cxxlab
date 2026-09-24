# GoogleTest - shared library, only when TEST is ON
if(TEST)
    set(BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

    set(GTEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/googletest")
    if(EXISTS "${GTEST_DIR}/CMakeLists.txt")
        add_subdirectory(third_party/googletest)
    else()
        message(FATAL_ERROR "GTest submodule not found at ${GTEST_DIR}!
        Please run: git submodule update --init --recursive")
    endif()
endif()
