add_executable(test_bluestore test_bluestore.cc)
target_include_directories(test_bluestore PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(test_bluestore
    PRIVATE
        bluestore
        kv
        blk
        cxxlab_test_helpers
        GTest::gtest_main
)
add_test(NAME test_bluestore COMMAND test_bluestore)

add_executable(test_blob test_blob.cc)
target_include_directories(test_blob PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(test_blob
    PRIVATE
        bluestore
        cxxlab_test_helpers
        GTest::gtest_main
)
add_test(NAME test_blob COMMAND test_blob)

add_executable(test_bluestore_extent_map test_extent_map.cc)
target_include_directories(test_bluestore_extent_map PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(test_bluestore_extent_map
    PRIVATE
        bluestore
        cxxlab_test_helpers
        GTest::gtest_main
)
add_test(NAME test_bluestore_extent_map COMMAND test_bluestore_extent_map)

add_executable(test_onode_collection test_onode_collection.cc)
target_include_directories(test_onode_collection PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(test_onode_collection
    PRIVATE
        bluestore
        kv
        cxxlab_test_helpers
        GTest::gtest_main
)
add_test(NAME test_onode_collection COMMAND test_onode_collection)

add_executable(test_bluestore_types test_bluestore_types.cc)
target_include_directories(test_bluestore_types PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(test_bluestore_types
    PRIVATE
        bluestore
        cxxlab_test_helpers
        GTest::gtest_main
)
add_test(NAME test_bluestore_types COMMAND test_bluestore_types)

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

add_executable(test_trans_context test_trans_context.cc)
target_include_directories(test_trans_context PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(test_trans_context
    PRIVATE
        bluestore
        kv
        blk
        cxxlab_test_helpers
        GTest::gtest_main
)
add_test(NAME test_trans_context COMMAND test_trans_context)

add_executable(test_write test_write.cc)
target_include_directories(test_write PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(test_write
    PRIVATE
        bluestore
        kv
        blk
        cxxlab_test_helpers
        GTest::gtest_main
)
add_test(NAME test_write COMMAND test_write)

add_executable(test_read test_read.cc)
target_include_directories(test_read PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(test_read
    PRIVATE
        bluestore
        kv
        blk
        cxxlab_test_helpers
        GTest::gtest_main
)
add_test(NAME test_read COMMAND test_read)

add_executable(test_zero_remove_attrs test_zero_remove_attrs.cc)
target_include_directories(test_zero_remove_attrs PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(test_zero_remove_attrs
    PRIVATE
        bluestore
        kv
        blk
        cxxlab_test_helpers
        GTest::gtest_main
)
add_test(NAME test_zero_remove_attrs COMMAND test_zero_remove_attrs)

add_executable(test_collection_list test_collection_list.cc)
target_include_directories(test_collection_list PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(test_collection_list
    PRIVATE
        bluestore
        kv
        blk
        cxxlab_test_helpers
        GTest::gtest_main
)
add_test(NAME test_collection_list COMMAND test_collection_list)

add_executable(test_omap test_omap.cc)
target_include_directories(test_omap PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(test_omap
    PRIVATE
        bluestore
        kv
        blk
        cxxlab_test_helpers
        GTest::gtest_main
)
add_test(NAME test_omap COMMAND test_omap)

add_executable(test_fsck test_fsck.cc)
target_include_directories(test_fsck PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(test_fsck
    PRIVATE
        bluestore
        kv
        blk
        cxxlab_test_helpers
        GTest::gtest_main
)
add_test(NAME test_fsck COMMAND test_fsck)

add_executable(test_buffer_cache test_buffer_cache.cc)
target_include_directories(test_buffer_cache PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(test_buffer_cache
    PRIVATE
        bluestore
        kv
        blk
        cxxlab_test_helpers
        GTest::gtest_main
)
add_test(NAME test_buffer_cache COMMAND test_buffer_cache)
