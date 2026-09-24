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

add_executable(test_blue_rocks_env test_blue_rocks_env.cc)
target_include_directories(test_blue_rocks_env PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(test_blue_rocks_env
    PRIVATE
        bluestore
        bluefs
        blk
        kv
        RocksDB::RocksDB
        cxxlab_test_helpers
        GTest::gtest_main
)
add_test(NAME test_blue_rocks_env COMMAND test_blue_rocks_env)
