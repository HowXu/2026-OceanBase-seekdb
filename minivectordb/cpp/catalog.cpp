#include "catalog.hpp"
#include "storage_codec.hpp"
#include <algorithm>

namespace minivectordb {
void Catalog::add(TableSchema schema, const std::string &object) {
  validate_schema(schema);
  storage::validate_reference(object, "objects", ".tbl");
  auto name = schema.name;
  if (std::any_of(tables_.begin(), tables_.end(), [&](const auto& item) {
        return same_name(item.first, name);
      }))
    throw DbError("table_exists", "table already exists");
  tables_.emplace(std::move(name), TableEntry{std::move(schema), {object}});
}
const TableEntry &Catalog::find(const std::string &table) const {
  auto it = std::find_if(tables_.begin(), tables_.end(), [&](const auto& item) {
    return same_name(item.first, table);
  });
  if (it == tables_.end())
    throw DbError("unknown_table", "table does not exist");
  return it->second;
}
void Catalog::replace_object(const std::string &table,
                             const std::string &object) {
  replace_objects(table, {object});
}
void Catalog::replace_objects(const std::string &table,
                              std::vector<std::string> objects) {
  if (objects.empty()) throw DbError("type_error", "table requires a base object");
  for (const auto& object : objects)
    storage::validate_reference(object, "objects", ".tbl");
  auto it = std::find_if(tables_.begin(), tables_.end(), [&](const auto& item) {
    return same_name(item.first, table);
  });
  if (it == tables_.end())
    throw DbError("unknown_table", "table does not exist");
  it->second.objects = std::move(objects);
}
void Catalog::add_vector_index(VectorIndexDescriptor descriptor) {
  for (const auto* name : {&descriptor.name, &descriptor.table, &descriptor.column}) {
    if (name->empty() || name->find('\0') != std::string::npos)
      throw DbError("type_error", "invalid index reference name");
    utf8_codepoints(*name);
  }
  if (descriptor.backend != VectorIndexBackend::FlatScan)
    throw DbError("type_error", "unknown vector index backend");
  const auto& schema = find(descriptor.table).schema;
  const ColumnSchema* column = nullptr;
  for (const auto& item : schema.columns)
    if (same_name(item.name, descriptor.column)) column = &item;
  if (!column) throw DbError("unknown_column", "index column does not exist");
  if (column->type != ColumnType::Vector)
    throw DbError("type_error", "vector index requires a VECTOR column");
  if (std::any_of(vector_indexes_.begin(), vector_indexes_.end(), [&](const auto& item) {
        return same_name(item.first, descriptor.name);
      }))
    throw DbError("index_exists", "index name already exists");
  auto name = descriptor.name;
  vector_indexes_.emplace(std::move(name), std::move(descriptor));
}
} // namespace minivectordb
