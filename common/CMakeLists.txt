add_library(common SHARED
    armor.cc
    buffer.cc
    cassert.cc
    cpu.cc
    crc32.cc
    error.cc
    formatter.cc
    logger.cc
    safe_io.cc
    spinlock.cc
    throttle.cc
)
target_include_directories(common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    $<TARGET_PROPERTY:spdlog::spdlog,INTERFACE_INCLUDE_DIRECTORIES>
)
target_link_libraries(common PUBLIC
    Boost::boost
    isal
)
target_link_libraries(common PRIVATE
    spdlog::spdlog
    fmt::fmt
    pthread
)
target_compile_definitions(common PUBLIC HAVE_ISA_L=1)
