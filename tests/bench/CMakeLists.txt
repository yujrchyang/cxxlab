add_executable(bench_bluestore bench_bluestore.cc)
target_include_directories(bench_bluestore PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(bench_bluestore
    PRIVATE
        bluestore
        kv
        blk
        cxxlab_test_helpers
        pthread
)
