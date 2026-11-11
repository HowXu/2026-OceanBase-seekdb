#pragma once
#include "types.hpp"
#include <filesystem>
#include <iostream>
#include <unistd.h>
using namespace minivectordb;
inline void check(bool b) {
  if (!b)
    throw std::runtime_error("check failed");
}
template <class F> void rejects(F f) {
  bool rejected = false;
  try {
    f();
  } catch (const DbError &) {
    rejected = true;
  }
  check(rejected);
}
struct TestDir {
  std::filesystem::path path;
  TestDir() {
    path = std::filesystem::current_path() /
           ("task04-data-" + std::to_string(getpid()));
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
  }
  ~TestDir() { std::filesystem::remove_all(path); }
};
inline TableSchema schema() {
  return {"docs",
          {{"text", ColumnType::Varchar, 3, false},
           {"id", ColumnType::BigInt, {}, true},
           {"v", ColumnType::Vector, 2, false}},
          1};
}
