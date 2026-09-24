add_executable(test_bitmap_freelist_manager test_bitmap_freelist_manager.cc)
target_include_directories(test_bitmap_freelist_manager PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(test_bitmap_freelist_manager
    PRIVATE
        bluestore
        kv
        cxxlab_test_helpers
        GTest::gtest_main
)
add_test(NAME test_bitmap_freelist_manager COMMAND test_bitmap_freelist_manager)

add_executable(test_bluefs_types test_bluefs_types.cc)
target_include_directories(test_bluefs_types PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(test_bluefs_types
    PRIVATE
        bluestore
        cxxlab_test_helpers
        GTest::gtest_main
)
add_test(NAME test_bluefs_types COMMAND test_bluefs_types)

add_executable(test_bluefs_volume_selector test_bluefs_volume_selector.cc)
target_include_directories(test_bluefs_volume_selector PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(test_bluefs_volume_selector
    PRIVATE
        bluestore
        cxxlab_test_helpers
        GTest::gtest_main
)
add_test(NAME test_bluefs_volume_selector COMMAND test_bluefs_volume_selector)

add_executable(test_bluefs test_bluefs.cc)
target_include_directories(test_bluefs PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(test_bluefs
    PRIVATE
        bluestore
        blk
        cxxlab_test_helpers
        GTest::gtest_main
)
add_test(NAME test_bluefs COMMAND test_bluefs)

add_executable(test_blue_rocks_env test_blue_rocks_env.cc)
target_include_directories(test_blue_rocks_env PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(test_blue_rocks_env
    PRIVATE
        bluestore
        blk
        kv
        RocksDB::RocksDB
        cxxlab_test_helpers
        GTest::gtest_main
)
add_test(NAME test_blue_rocks_env COMMAND test_blue_rocks_env)
