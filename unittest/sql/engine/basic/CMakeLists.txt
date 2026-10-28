cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(file_sql_scan_tests LANGUAGES CXX)
  enable_testing()
endif()

get_filename_component(FILE_SQL_REPO_ROOT
  "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(FILE_SQL_GTEST_ROOT
  "${FILE_SQL_REPO_ROOT}/deps/3rd/usr/local/oceanbase/deps/devel"
  CACHE PATH "Existing GTest installation")
find_package(Threads REQUIRED)

add_library(file_sql_gtest STATIC IMPORTED GLOBAL)
set_target_properties(file_sql_gtest PROPERTIES
  IMPORTED_LOCATION "${FILE_SQL_GTEST_ROOT}/lib/libgtest.a"
  INTERFACE_INCLUDE_DIRECTORIES "${FILE_SQL_GTEST_ROOT}/include")
add_library(file_sql_gtest_main STATIC IMPORTED GLOBAL)
set_target_properties(file_sql_gtest_main PROPERTIES
  IMPORTED_LOCATION "${FILE_SQL_GTEST_ROOT}/lib/libgtest_main.a"
  INTERFACE_LINK_LIBRARIES "file_sql_gtest;Threads::Threads")

add_executable(test_file_scan_utils
  test_file_scan_utils.cpp
  test_file_sql_memory.cpp
  "${FILE_SQL_REPO_ROOT}/src/sql/engine/basic/file_sql_file_access.cpp"
  "${FILE_SQL_REPO_ROOT}/src/sql/engine/basic/ob_file_scan_utils.cpp"
  "${FILE_SQL_REPO_ROOT}/src/sql/engine/basic/ob_file_parquet_scan.cpp")
target_compile_features(test_file_scan_utils PRIVATE cxx_std_17)
target_include_directories(test_file_scan_utils PRIVATE "${FILE_SQL_REPO_ROOT}/src")
target_link_libraries(test_file_scan_utils PRIVATE file_sql_gtest_main)
add_test(NAME file_scan_utils COMMAND test_file_scan_utils)

if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  add_executable(test_file_table_op test_file_table_op.cpp)
  target_link_libraries(test_file_table_op PRIVATE oceanbase ob_base file_sql_gtest_main)
  set_property(TARGET test_file_table_op APPEND PROPERTY
    BUILD_RPATH "$<TARGET_FILE_DIR:oceanbase>")
  add_test(NAME file_table_op COMMAND test_file_table_op)
endif()
