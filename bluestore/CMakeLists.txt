add_library(bluestore SHARED
    freelist_manager.cc
    bitmap_freelist_manager.cc
    bluefs.cc
    bluefs_config.cc
    bluefs_types.cc
    bluefs_volume_selector.cc
    blue_rocks_env.cc
)
target_include_directories(bluestore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}
)
target_link_libraries(bluestore PUBLIC
    common
    kv
    blk
)
target_link_libraries(bluestore PRIVATE
    RocksDB::RocksDB
)
