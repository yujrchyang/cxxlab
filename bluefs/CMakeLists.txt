add_library(bluefs SHARED
    bluefs.cc
    bluefs_config.cc
    bluefs_types.cc
    bluefs_volume_selector.cc
)
target_include_directories(bluefs PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}
)
target_link_libraries(bluefs PUBLIC
    common
    blk
)
