add_executable(test_file_sql_parser test_file_sql_parser.cpp)
target_compile_features(test_file_sql_parser PRIVATE cxx_std_17)
target_link_libraries(test_file_sql_parser PRIVATE
  oceanbase ob_base file_sql_gtest_main)
set_property(TARGET test_file_sql_parser APPEND PROPERTY
  BUILD_RPATH "$<TARGET_FILE_DIR:oceanbase>")
add_test(NAME file_sql_parser COMMAND test_file_sql_parser)
