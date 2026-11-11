#include "catalog.hpp"
#include "storage_codec.hpp"

namespace minivectordb {
void Catalog::add(TableSchema schema, const std::string &object) {
  validate_schema(schema);
  storage::validate_reference(object, "objects", ".tbl");
  auto name = schema.name;
  if (tables_.contains(name))
    throw DbError("table_exists", "table already exists");
  tables_.emplace(std::move(name), TableEntry{std::move(schema), object});
}
const TableEntry &Catalog::find(const std::string &table) const {
  auto it = tables_.find(table);
  if (it == tables_.end())
    throw DbError("unknown_table", "table does not exist");
  return it->second;
}
void Catalog::replace_object(const std::string &table,
                             const std::string &object) {
  storage::validate_reference(object, "objects", ".tbl");
  auto it = tables_.find(table);
  if (it == tables_.end())
    throw DbError("unknown_table", "table does not exist");
  it->second.object = object;
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
    if (item.name == descriptor.column) column = &item;
  if (!column) throw DbError("unknown_column", "index column does not exist");
  if (column->type != ColumnType::Vector)
    throw DbError("type_error", "vector index requires a VECTOR column");
  if (vector_indexes_.contains(descriptor.name))
    throw DbError("index_exists", "index name already exists");
  auto name = descriptor.name;
  vector_indexes_.emplace(std::move(name), std::move(descriptor));
}
} // namespace minivectordb
