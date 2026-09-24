# cmake/BuildRocksDB.cmake
# -------------------------------------------------------------------
# Build RocksDB (v7.10.2) as a shared library from submodule
# -------------------------------------------------------------------

include(ExternalProject)

set(ROCKSDB_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/rocksdb")
set(ROCKSDB_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/third_party/rocksdb")
set(ROCKSDB_LIBRARY "${ROCKSDB_BINARY_DIR}/librocksdb.so")

set(rocksdb_CMAKE_ARGS
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
    -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
    -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
    -DPORTABLE=ON
    -DFAIL_ON_WARNINGS=OFF
    -DUSE_RTTI=1
    -DWITH_GFLAGS=OFF
    -DWITH_JEMALLOC=OFF
    -DWITH_SNAPPY=OFF
    -DWITH_LZ4=OFF
    -DWITH_ZLIB=OFF
    -DWITH_ZSTD=OFF
    -DWITH_BZ2=OFF
    -DWITH_LIBURING=OFF
    -DWITH_NUMA=OFF
    -DWITH_TBB=OFF
    -DWITH_JNI=OFF
    -DWITH_BENCHMARK_TOOLS=OFF
    -DWITH_CORE_TOOLS=OFF
    -DWITH_TOOLS=OFF
    -DROCKSDB_BUILD_SHARED=ON
    -DROCKSDB_INSTALL_ON_WINDOWS=OFF
)

if(CMAKE_MAKE_PROGRAM MATCHES "make")
    set(make_cmd $(MAKE) rocksdb-shared)
else()
    set(make_cmd ${CMAKE_COMMAND} --build <BINARY_DIR> --target rocksdb-shared)
endif()

ExternalProject_Add(rocksdb_ext
    SOURCE_DIR "${ROCKSDB_SRC_DIR}"
    CMAKE_ARGS ${rocksdb_CMAKE_ARGS}
    BINARY_DIR "${ROCKSDB_BINARY_DIR}"
    BUILD_COMMAND "${make_cmd}"
    BUILD_BYPRODUCTS "${ROCKSDB_LIBRARY}"
    INSTALL_COMMAND ""
    LOG_CONFIGURE ON
    LOG_BUILD ON
)

add_library(RocksDB::RocksDB SHARED IMPORTED GLOBAL)
add_dependencies(RocksDB::RocksDB rocksdb_ext)

set_target_properties(RocksDB::RocksDB PROPERTIES
    IMPORTED_LOCATION "${ROCKSDB_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${ROCKSDB_SRC_DIR}/include"
    IMPORTED_LINK_INTERFACE_LANGUAGES "CXX"
)
