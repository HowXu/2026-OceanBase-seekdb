add_executable(test_ai_column_schema
  test_ai_column_schema.cpp test_ai_column_schema_integration.cpp
  test_ai_column_inner_table.cpp)
target_compile_features(test_ai_column_schema PRIVATE cxx_std_17)
target_link_libraries(test_ai_column_schema PRIVATE
  oceanbase ob_base file_sql_gtest_main)
set_property(TARGET test_ai_column_schema APPEND PROPERTY
  BUILD_RPATH "$<TARGET_FILE_DIR:oceanbase>")
add_test(NAME ai_column_schema COMMAND test_ai_column_schema)
