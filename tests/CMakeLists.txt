# Generate shared test helper header
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/cxxlab_test.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/include/cxxlab_test.h
)

# Interface library carrying the generated include path
add_library(cxxlab_test_helpers INTERFACE)
target_include_directories(cxxlab_test_helpers INTERFACE
    ${CMAKE_CURRENT_BINARY_DIR}/include
)

add_subdirectory(common)
add_subdirectory(blk)
add_subdirectory(kv)
add_subdirectory(bluefs)
add_subdirectory(bluestore)
add_subdirectory(btier)
