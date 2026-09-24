# GearsUE3's native engine. Everything here is independently authored C++ that
# operates on the title's own cooked content; nothing executes guest code.

add_library(gears_engine_package STATIC
    package/byte_reader.cpp
    package/compressed_record.cpp
    package/lzo1x.cpp
    package/object_tables.cpp
    package/package.cpp
    package/package_summary.cpp)
target_include_directories(gears_engine_package PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
x360port_enable_warnings(gears_engine_package)

add_executable(test_engine_package "${CMAKE_SOURCE_DIR}/tests/test_engine_package.cpp")
target_link_libraries(test_engine_package PRIVATE gears_engine_package)
x360port_enable_warnings(test_engine_package)
gears_add_binary_test(gears_engine_package test_engine_package)

# Loads every cooked package of an operator-supplied extracted disc. Not a
# CTest: restricted title bytes never become a hosted-test input.
add_executable(gears_package_census "${CMAKE_SOURCE_DIR}/tools/package_census/main.cpp")
target_link_libraries(gears_package_census PRIVATE gears_engine_package lucent::lucent)
x360port_enable_warnings(gears_package_census)
