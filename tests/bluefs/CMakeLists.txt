add_executable(test_bluefs_types test_bluefs_types.cc)
target_include_directories(test_bluefs_types PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(test_bluefs_types
    PRIVATE
        bluefs
        cxxlab_test_helpers
        GTest::gtest_main
)
add_test(NAME test_bluefs_types COMMAND test_bluefs_types)

add_executable(test_bluefs_volume_selector test_bluefs_volume_selector.cc)
target_include_directories(test_bluefs_volume_selector PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(test_bluefs_volume_selector
    PRIVATE
        bluefs
        cxxlab_test_helpers
        GTest::gtest_main
)
add_test(NAME test_bluefs_volume_selector COMMAND test_bluefs_volume_selector)

add_executable(test_bluefs test_bluefs.cc)
target_include_directories(test_bluefs PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(test_bluefs
    PRIVATE
        bluefs
        blk
        cxxlab_test_helpers
        GTest::gtest_main
)
add_test(NAME test_bluefs COMMAND test_bluefs)
