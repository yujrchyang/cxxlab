add_library(bluestore SHARED
    blob.cc
    buffer_cache.cc
    bluestore.cc
    bluestore_types.cc
    collection.cc
    error_injector.cc
    extent_map.cc
    onode.cc
    freelist_manager.cc
    bitmap_freelist_manager.cc
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
    bluefs
)
target_link_libraries(bluestore PRIVATE
    RocksDB::RocksDB
)
