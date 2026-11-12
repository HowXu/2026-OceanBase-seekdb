#pragma once
#include "types.hpp"
#include <map>

namespace minivectordb {
struct TableEntry {
  TableSchema schema;
  // Ordered immutable objects: base segment first, then oldest-to-newest deltas.
  std::vector<std::string> objects;
};
enum class VectorIndexBackend { FlatScan = 1 };
struct VectorIndexDescriptor {
  std::string name;
  std::string table;
  std::string column;
  VectorIndexBackend backend = VectorIndexBackend::FlatScan;
};
// Only schema, descriptors and object references are resident; never table contents.
class Catalog {
public:
  void add(TableSchema schema, const std::string &object);
  const TableEntry &find(const std::string &table) const;
  void replace_object(const std::string &table, const std::string &object);
  void replace_objects(const std::string &table, std::vector<std::string> objects);
  void add_vector_index(VectorIndexDescriptor descriptor);
  const std::map<std::string, VectorIndexDescriptor> &vector_indexes() const { return vector_indexes_; }
  const std::map<std::string, TableEntry> &tables() const { return tables_; }

private:
  std::map<std::string, TableEntry> tables_;
  std::map<std::string, VectorIndexDescriptor> vector_indexes_;
};
} // namespace minivectordb
