add_executable(test_ai_column_dml test_ai_column_dml.cpp)
target_compile_features(test_ai_column_dml PRIVATE cxx_std_17)
target_link_libraries(test_ai_column_dml PRIVATE
  oceanbase ob_base file_sql_gtest_main)
set_property(TARGET test_ai_column_dml APPEND PROPERTY
  BUILD_RPATH "$<TARGET_FILE_DIR:oceanbase>")
add_test(NAME ai_column_dml COMMAND test_ai_column_dml)
