cmake_minimum_required(VERSION 3.20)
project(minivectordb LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(Python 3.12 EXACT REQUIRED COMPONENTS Interpreter Development)
find_package(pybind11 3.1.0 EXACT CONFIG REQUIRED)
add_library(minivectordb_core STATIC cpp/engine.cpp cpp/executor.cpp cpp/exact_search.cpp cpp/types.cpp
  cpp/table_file.cpp cpp/catalog.cpp cpp/root_store.cpp)
option(MINIVECTORDB_ENABLE_FAULT_INJECTION "Enable process-stop checkpoints for crash tests ONLY" OFF)
if(MINIVECTORDB_ENABLE_FAULT_INJECTION)
  target_compile_definitions(minivectordb_core PUBLIC MINIVECTORDB_TESTING)
endif()
set_target_properties(minivectordb_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(minivectordb_core PUBLIC cpp)
pybind11_add_module(_minivectordb cpp/bindings.cpp)
target_link_libraries(_minivectordb PRIVATE minivectordb_core)
set_target_properties(_minivectordb PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/python")
include(CTest)
if(BUILD_TESTING)
  foreach(name types table_file root_store)
    add_executable(test_${name} tests/cpp/test_${name}.cpp)
    target_link_libraries(test_${name} PRIVATE minivectordb_core)
  endforeach()
  add_executable(test_root_publication tests/cpp/test_root_store.cpp
    cpp/root_store.cpp cpp/catalog.cpp cpp/table_file.cpp cpp/types.cpp)
  target_include_directories(test_root_publication PRIVATE cpp)
  target_compile_definitions(test_root_publication PRIVATE MINIVECTORDB_TESTING)
  add_test(NAME task07_publication_recovery_gc COMMAND test_root_publication publication)
  add_test(NAME task07_persistence COMMAND "${Python_EXECUTABLE}" -m pytest
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_persistence.py" -q)
  set_tests_properties(task07_persistence PROPERTIES ENVIRONMENT
    "PYTHONPATH=${CMAKE_BINARY_DIR}/python:${CMAKE_CURRENT_SOURCE_DIR}/python")
  add_test(NAME task06_exact_search
    COMMAND "${Python_EXECUTABLE}" -m pytest
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_exact_search.py" -q)
  set_tests_properties(task06_exact_search PROPERTIES ENVIRONMENT
    "PYTHONPATH=${CMAKE_BINARY_DIR}/python:${CMAKE_CURRENT_SOURCE_DIR}/python")
  add_test(NAME task05_sql_semantics
    COMMAND "${Python_EXECUTABLE}" -m pytest
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_sql_semantics.py" -q)
  set_tests_properties(task05_sql_semantics PROPERTIES ENVIRONMENT
    "PYTHONPATH=${CMAKE_BINARY_DIR}/python:${CMAKE_CURRENT_SOURCE_DIR}/python")
  add_test(NAME task04_types COMMAND test_types)
  add_test(NAME task04_streaming_table COMMAND test_table_file)
  add_test(NAME task04_truncation COMMAND test_table_file truncation)
  add_test(NAME task04_catalog_paths COMMAND test_root_store)
  set_tests_properties(task04_types task04_streaming_table task04_truncation
    task04_catalog_paths PROPERTIES RUN_SERIAL TRUE)
  add_executable(test_binding_values tests/cpp/test_binding_values.cpp)
  target_link_libraries(test_binding_values PRIVATE minivectordb_core pybind11::embed)
  add_test(NAME task01_binding_values COMMAND test_binding_values)
  add_test(NAME task01_protocol_boundary
    COMMAND "${Python_EXECUTABLE}" -m pytest
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_protocol.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ir_boundary.py" -q)
  set_tests_properties(task01_protocol_boundary PROPERTIES ENVIRONMENT
    "PYTHONPATH=${CMAKE_BINARY_DIR}/python:${CMAKE_CURRENT_SOURCE_DIR}/python")
endif()
