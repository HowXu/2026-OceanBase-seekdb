#include "executor.hpp"
#include "exact_search.hpp"
#include <cmath>
#include "storage_codec.hpp"
#include "table_file.hpp"
#include "table_segments.hpp"
#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <queue>
#include <set>

namespace minivectordb {
namespace {
[[noreturn]] void semantic(const char* code, const char* message) {
  throw DbError(code, message);
}
std::size_t column_index(const TableSchema& schema, const std::string& name) {
  for (std::size_t i = 0; i < schema.columns.size(); ++i)
    if (same_name(schema.columns[i].name, name)) return i;
  semantic("unknown_column", "column does not exist");
}
std::int64_t key(const Row& row, const TableSchema& schema) {
  return std::get<std::int64_t>(row[schema.primary_key_index]);
}
std::vector<std::filesystem::path> table_paths(const TableEntry& entry, RootStore& store) {
  std::vector<std::filesystem::path> paths;
  for (const auto& object : entry.objects) paths.push_back(store.object_path(object));
  return paths;
}
std::shared_ptr<std::unordered_set<std::int64_t>> primary_keys(const TableEntry& entry,
                                                                RootStore& store) {
  if (!entry.primary_keys) {
    auto keys = std::make_shared<std::unordered_set<std::int64_t>>();
    LogicalTableReader reader(table_paths(entry, store), entry.schema);
    Row row;
    while (reader.next(row)) keys->insert(key(row, entry.schema));
    entry.primary_keys = std::move(keys);
  }
  return entry.primary_keys;
}
struct BoundExpression {
  std::optional<std::size_t> column;
  Cell literal = std::int64_t{0};
  ColumnType type;
  std::optional<std::vector<float>> query;
  bool operator==(const BoundExpression&) const = default;
  ResultCell evaluate(const Row& row, bool squared = false) const {
    if (query) {
      const auto distance = l2_distance_squared(std::get<std::vector<float>>(row[*column]), *query);
      return squared ? distance : std::sqrt(distance);
    }
    return std::visit([](const auto& value) -> ResultCell { return value; },
                      column ? row[*column] : literal);
  }
};
BoundExpression bind_expression(const Expression& expression, const TableSchema& schema) {
  if (auto c = std::get_if<ColumnRef>(&expression)) {
    auto i = column_index(schema, c->name);
    return {i, std::int64_t{0}, schema.columns[i].type, std::nullopt};
  }
  if (auto literal = std::get_if<Literal>(&expression)) {
    return std::visit([](const auto& value) -> BoundExpression {
      Cell cell = value;
      return {std::nullopt, cell, std::holds_alternative<std::int64_t>(cell)
          ? ColumnType::BigInt : ColumnType::Varchar, std::nullopt};
    }, *literal);
  }
  if (auto distance = std::get_if<L2Distance>(&expression)) {
    auto i = column_index(schema, distance->column);
    if (schema.columns[i].type != ColumnType::Vector)
      semantic("type_error", "L2_DISTANCE requires a vector column");
    return {i, std::int64_t{0}, ColumnType::Vector,
            parse_vector(distance->vector, *schema.columns[i].length)};
  }
  semantic("unsupported_sql", "expression is not supported");
}
int compare(const ResultCell& a, const ResultCell& b) {
  if (a.index() != b.index() || std::holds_alternative<std::vector<float>>(a))
    semantic("type_error", "comparison requires matching scalar types");
  if (auto n = std::get_if<std::int64_t>(&a)) {
    auto other = std::get<std::int64_t>(b);
    return *n < other ? -1 : *n > other ? 1 : 0;
  }
  if (auto n = std::get_if<double>(&a)) {
    auto other = std::get<double>(b);
    return *n < other ? -1 : *n > other ? 1 : 0;
  }
  const auto& x = std::get<std::string>(a);
  const auto& y = std::get<std::string>(b);
  // Valid UTF-8 byte order agrees with Unicode scalar order; no normalization.
  return x < y ? -1 : x > y ? 1 : 0;
}
struct BoundComparison { BoundExpression left, right; CompareOperator op; };
using Predicate = std::vector<BoundComparison>;
Predicate bind_where(const std::vector<Comparison>& where, const TableSchema& schema) {
  Predicate result;
  for (const auto& c : where) {
    auto left = bind_expression(c.left, schema), right = bind_expression(c.right, schema);
    if (left.type != right.type || left.type == ColumnType::Vector)
      semantic("type_error", "comparison requires matching scalar types");
    result.push_back({std::move(left), std::move(right), c.op});
  }
  return result;
}
bool matches(const Row& row, const Predicate& predicate) {
  for (const auto& c : predicate) {
    int n = compare(c.left.evaluate(row), c.right.evaluate(row));
    bool pass = false;
    switch (c.op) {
      case CompareOperator::Equal: pass = n == 0; break;
      case CompareOperator::Less: pass = n < 0; break;
      case CompareOperator::LessEqual: pass = n <= 0; break;
      case CompareOperator::Greater: pass = n > 0; break;
      case CompareOperator::GreaterEqual: pass = n >= 0; break;
    }
    if (!pass) return false;
  }
  return true;
}
// Reserved immutable objects are removed on failure; released only after a complete
// candidate has been produced. Destructors never mask the original SQL/I/O error.
class OwnedObjects {
 public:
  explicit OwnedObjects(RootStore& store) : store_(store) {}
  ~OwnedObjects() {
    for (const auto& ref : refs_) {
      std::error_code ec;
      std::filesystem::remove(store_.data_dir() / ref, ec);
    }
  }
  std::string create() {
    auto ref = store_.create_object();
    try { refs_.insert(ref); }
    catch (...) {
      std::error_code ec;
      std::filesystem::remove(store_.data_dir() / ref, ec);
      throw;
    }
    return ref;
  }
  void release(const std::string& ref) { refs_.erase(ref); }
  void remove(const std::string& ref) {
    std::error_code ec;
    std::filesystem::remove(store_.object_path(ref), ec);
    if (ec) semantic("storage_error", "cannot remove temporary run");
    refs_.erase(ref);
  }
 private:
  RootStore& store_;
  std::set<std::string> refs_;
};

struct SortItem { BoundExpression expression; bool descending; };
struct RowLess {
  const std::vector<SortItem>* order;
  const TableSchema* schema;
  bool operator()(const Row& a, const Row& b) const {
    for (const auto& item : *order) {
      int n = compare(item.expression.evaluate(a, true), item.expression.evaluate(b, true));
      if (n != 0) return item.descending ? n > 0 : n < 0;
    }
    return key(a, *schema) < key(b, *schema);
  }
};
std::size_t row_memory(const Row& row) {
  std::size_t bytes = sizeof(Row) + row.size() * sizeof(Cell);
  for (const auto& cell : row) {
    if (auto text = std::get_if<std::string>(&cell)) bytes += text->size();
    if (auto vector = std::get_if<std::vector<float>>(&cell)) bytes += vector->size() * sizeof(float);
  }
  return bytes;
}
// Sort runs are private, schema-directed streams, not persistent table objects:
// rows are in query order rather than PK order. At most two input rows are held
// by a merge, and at most 64 live runs exist (binary carry merge).
void write_run_row(storage::Output& out, const Row& row) {
  out.u8(1);
  for (const auto& cell : row) {
    if (auto n = std::get_if<std::int64_t>(&cell)) out.u64(std::bit_cast<std::uint64_t>(*n));
    else if (auto text = std::get_if<std::string>(&cell)) out.text(*text);
    else for (float x : std::get<std::vector<float>>(cell)) out.u32(std::bit_cast<std::uint32_t>(x));
  }
}
bool read_run_row(storage::Input& in, const TableSchema& schema, Row& row) {
  auto marker = in.u8();
  if (marker == 0) { in.end(); row.clear(); return false; }
  if (marker != 1) semantic("storage_error", "invalid sort run");
  row.clear();
  for (const auto& c : schema.columns) {
    if (c.type == ColumnType::BigInt) row.emplace_back(std::bit_cast<std::int64_t>(in.u64()));
    else if (c.type == ColumnType::Varchar) row.emplace_back(in.text());
    else {
      std::vector<float> values;
      for (std::uint64_t i = 0; i < *c.length; ++i)
        values.push_back(std::bit_cast<float>(in.u32()));
      row.emplace_back(std::move(values));
    }
  }
  return true;
}
class ExternalSort {
 public:
  ExternalSort(RootStore& store, const TableSchema& schema, RowLess less)
      : store_(store), schema_(schema), less_(less), owned_(store) {}
  void add(Row row) {
    auto bytes = row_memory(row);
    if (!chunk_.empty() && (bytes >= budget || used_ > budget - bytes)) flush();
    used_ += bytes;
    chunk_.push_back(std::move(row));
    if (used_ >= budget) flush();
  }
  void emit(const std::function<void(const Row&)>& output) {
    flush();
    std::optional<std::string> result;
    for (auto& run : runs_) if (run) {
      result = result ? merge(*result, *run) : *run;
      run.reset();
    }
    if (!result) return;
    storage::Input in(store_.object_path(*result));
    Row row;
    while (read_run_row(in, schema_, row)) output(row);
  }
 private:
  static constexpr std::size_t budget = 64 * 1024;
  RootStore& store_;
  const TableSchema& schema_;
  RowLess less_;
  OwnedObjects owned_;
  std::array<std::optional<std::string>, 64> runs_;
  std::vector<Row> chunk_;
  std::size_t used_ = 0;
  std::string merge(const std::string& a, const std::string& b) {
    auto ref = owned_.create();
    {
      storage::Input left(store_.object_path(a)), right(store_.object_path(b));
      storage::Output out(store_.object_path(ref));
      Row x, y;
      bool has_x = read_run_row(left, schema_, x), has_y = read_run_row(right, schema_, y);
      while (has_x || has_y) {
        if (has_x && (!has_y || less_(x,y))) {
          write_run_row(out, x); has_x = read_run_row(left, schema_, x);
        } else {
          write_run_row(out, y); has_y = read_run_row(right, schema_, y);
        }
      }
      out.u8(0); out.finish();
    }
    owned_.remove(a); owned_.remove(b);
    return ref;
  }
  void flush() {
    if (chunk_.empty()) return;
    std::sort(chunk_.begin(), chunk_.end(), less_);
    auto ref = owned_.create();
    {
      storage::Output out(store_.object_path(ref));
      for (const auto& row : chunk_) write_run_row(out, row);
      out.u8(0); out.finish();
    }
    chunk_.clear(); used_ = 0;
    for (auto& run : runs_) {
      if (!run) { run = ref; return; }
      ref = merge(*run, ref); run.reset();
    }
    semantic("storage_error", "sort run count overflow");
  }
};

ResultSet select_rows(const Select& statement, const TableEntry& entry, RootStore& store) {
  const auto& schema = entry.schema;
  auto where = bind_where(statement.where, schema);
  ResultSet result;
  std::vector<BoundExpression> projection;
  for (const auto& p : statement.projections) {
    if (std::holds_alternative<Star>(p.expression)) {
      if (p.alias) semantic("unsupported_sql", "star cannot have an alias");
      for (const auto& c : schema.columns) {
        result.columns.push_back(c.name);
        projection.push_back(bind_expression(ColumnRef{c.name}, schema));
      }
    } else {
      projection.push_back(bind_expression(p.expression, schema));
      if (p.alias) result.columns.push_back(*p.alias);
      else if (p.display_text) result.columns.push_back(*p.display_text);
      else if (auto c = std::get_if<ColumnRef>(&p.expression)) result.columns.push_back(c->name);
      else if (auto distance = std::get_if<L2Distance>(&p.expression))
        result.columns.push_back("L2_DISTANCE(" + distance->column + ", '" + distance->vector + "')");
      else {
        const auto& literal = std::get<Literal>(p.expression);
        result.columns.push_back(std::visit([](const auto& value) -> std::string {
          if constexpr (std::is_same_v<std::decay_t<decltype(value)>, std::string>) return value;
          else return std::to_string(value);
        }, literal));
      }
    }
  }
  if (projection.empty()) semantic("unsupported_sql", "projection cannot be empty");
  std::vector<SortItem> order;
  for (const auto& item : statement.order_by) {
    std::optional<BoundExpression> bound;
    if (auto position = std::get_if<OrderPosition>(&item.expression)) {
      if (position->position == 0 || position->position > projection.size())
        semantic("type_error", "ORDER BY position is outside the projection");
      bound = projection.at(position->position - 1);
    } else if (auto c = std::get_if<ColumnRef>(&item.expression)) {
      for (std::size_t i = 0; i < result.columns.size(); ++i)
        if (same_name(result.columns[i], c->name)) {
          if (bound && *bound != projection[i]) semantic("ambiguous_column", "ambiguous ordering alias");
          bound = projection[i];
        }
    }
    if (!bound) bound = bind_expression(item.expression, schema);
    if (bound->type == ColumnType::Vector && !bound->query) semantic("type_error", "cannot order vectors");
    order.push_back({*bound, item.descending});
  }
  // Resolve every expression even for an empty table or LIMIT 0.
  if (statement.limit == 0) return result;
  auto emit = [&](const Row& row) {
    ResultRow output;
    for (const auto& p : projection)
      std::visit([&](const auto& value) { output.emplace_back(value); }, p.evaluate(row));
    result.rows.push_back(std::move(output));
  };
  LogicalTableReader reader(table_paths(entry, store), schema);
  Row row;
  RowLess less{&order, &schema};
  if (order.empty()) {
    while (reader.next(row)) if (matches(row, where)) {
      emit(row);
      if (statement.limit && result.rows.size() >= *statement.limit) break;
    }
  } else if (statement.limit) {
    std::priority_queue<Row, std::vector<Row>, RowLess> heap(less);
    while (reader.next(row)) if (matches(row, where)) {
      if (heap.size() < *statement.limit) heap.push(std::move(row));
      else if (less(row, heap.top())) { heap.pop(); heap.push(std::move(row)); }
    }
    std::vector<Row> sorted;
    while (!heap.empty()) { sorted.push_back(heap.top()); heap.pop(); }
    std::sort(sorted.begin(), sorted.end(), less);
    for (const auto& selected : sorted) emit(selected);
  } else {
    ExternalSort sort(store, schema, less);
    while (reader.next(row)) if (matches(row, where)) sort.add(std::move(row));
    sort.emit(emit);
  }
  return result;
}

QueryResult create_table(const CreateTable& statement, Catalog& candidate, RootStore& store) {
  for (const auto& [name, entry] : candidate.tables())
    if (same_name(name, statement.table)) semantic("table_exists", "table already exists");
  TableSchema schema{statement.table, statement.columns};
  validate_schema(schema);
  OwnedObjects owned(store);
  auto ref = owned.create();
  TableFileWriter writer(store.object_path(ref), schema);
  writer.finish();
  candidate.add(schema, ref);
  owned.release(ref);
  return EmptyResult{};
}
void append_delta_or_compact(const std::string& table,
                             const std::vector<DeltaRecord>& changes,
                             Catalog& candidate, RootStore& store) {
  if (changes.empty()) return;
  const auto entry = candidate.find(table);
  const auto& schema = entry.schema;
  OwnedObjects owned(store);
  auto ref = owned.create();
  if (entry.objects.size() <= 64) {
    DeltaFileWriter writer(store.object_path(ref), schema);
    for (const auto& change : changes) writer.append(change);
    writer.finish();
    auto objects = entry.objects;
    objects.push_back(ref);
    candidate.replace_objects(table, std::move(objects));
  } else {
    // One base plus 64 deltas is the read fan-out limit. The next actual
    // modification folds the logical table and this statement into a new base.
    TableFileWriter writer(store.object_path(ref), schema);
    LogicalTableReader reader(table_paths(entry, store), schema);
    Row row;
    std::size_t i = 0;
    auto emit = [&](const DeltaRecord& change) {
      if (!change.deleted) writer.append(change.row);
    };
    while (reader.next(row)) {
      auto current_key = key(row, schema);
      while (i < changes.size() && changes[i].key < current_key) emit(changes[i++]);
      if (i < changes.size() && changes[i].key == current_key) {
        emit(changes[i++]);
        continue;
      }
      writer.append(row);
    }
    while (i < changes.size()) emit(changes[i++]);
    writer.finish();
    candidate.replace_object(table, ref);
  }
  owned.release(ref);
}
QueryResult insert_rows(const Insert& statement, Catalog& candidate, RootStore& store) {
  const auto entry = candidate.find(statement.table);
  const auto& schema = entry.schema;
  std::vector<std::size_t> columns;
  if (statement.columns) {
    std::set<std::size_t> seen;
    for (const auto& c : *statement.columns) {
      auto i = column_index(schema, c);
      if (!seen.insert(i).second) semantic("duplicate_column", "duplicate insert column");
      columns.push_back(i);
    }
  } else for (std::size_t i = 0; i < schema.columns.size(); ++i) columns.push_back(i);
  if (columns.size() != schema.columns.size()) semantic("type_error", "all columns require values");
  std::vector<Row> additions;
  for (const auto& values : statement.rows) {
    if (values.size() != columns.size()) semantic("type_error", "insert row width mismatch");
    Row row(schema.columns.size());
    for (std::size_t i = 0; i < columns.size(); ++i)
      row[columns[i]] = bind_literal(values[i], schema.columns[columns[i]]);
    additions.push_back(std::move(row));
  }
  auto less = [&](const Row& a, const Row& b) { return key(a,schema) < key(b,schema); };
  std::sort(additions.begin(), additions.end(), less);
  for (std::size_t i = 1; i < additions.size(); ++i)
    if (key(additions[i-1],schema) == key(additions[i],schema))
      semantic("duplicate_key", "duplicate primary key in insert request");
  auto existing_keys = primary_keys(entry, store);
  for (const auto& addition : additions)
    if (existing_keys->contains(key(addition, schema)))
      semantic("duplicate_key", "duplicate primary key");
  auto next_keys = std::make_shared<std::unordered_set<std::int64_t>>(*existing_keys);
  next_keys->reserve(existing_keys->size() + additions.size());
  for (const auto& addition : additions) next_keys->insert(key(addition, schema));
  std::vector<DeltaRecord> changes;
  changes.reserve(additions.size());
  for (const auto& addition : additions)
    changes.push_back({key(addition, schema), false, addition});
  append_delta_or_compact(statement.table, changes, candidate, store);
  candidate.find(statement.table).primary_keys = std::move(next_keys);
  return AffectedRows{static_cast<std::uint64_t>(additions.size())};
}
template<class Mutation>
QueryResult rewrite_rows(const Mutation& statement, Catalog& candidate, RootStore& store) {
  const auto entry = candidate.find(statement.table);
  const auto& schema = entry.schema;
  auto where = bind_where(statement.where, schema);
  std::vector<std::pair<std::size_t,Cell>> assignments;
  if constexpr (std::is_same_v<Mutation,Update>) {
    if (statement.assignments.empty()) semantic("unsupported_sql", "empty update");
    std::set<std::size_t> seen;
    for (const auto& assignment : statement.assignments) {
      auto i = column_index(schema, assignment.column);
      if (i == schema.primary_key_index) semantic("primary_key_update", "primary key is immutable");
      if (!seen.insert(i).second) semantic("duplicate_column", "duplicate update assignment");
      assignments.emplace_back(i, bind_literal(assignment.value, schema.columns[i]));
    }
  }
  LogicalTableReader reader(table_paths(entry, store), schema);
  Row row;
  std::uint64_t count = 0;
  std::vector<DeltaRecord> changes;
  while (reader.next(row)) {
    if (!matches(row, where)) continue;
    ++count;
    auto current_key = key(row, schema);
    if constexpr (std::is_same_v<Mutation,Delete>) {
      changes.push_back({current_key, true, {}});
    } else {
      for (const auto& [i,value] : assignments) row[i] = value;
      changes.push_back({current_key, false, std::move(row)});
    }
  }
  append_delta_or_compact(statement.table, changes, candidate, store);
  if constexpr (std::is_same_v<Mutation,Delete>) {
    if (!changes.empty()) {
      auto next_keys = std::make_shared<std::unordered_set<std::int64_t>>(
          *primary_keys(entry, store));
      for (const auto& change : changes) next_keys->erase(change.key);
      candidate.find(statement.table).primary_keys = std::move(next_keys);
    }
  }
  return AffectedRows{count};
}
}  // namespace
QueryResult execute_scalar(const Statement& statement, Catalog& statement_candidate, RootStore& store,
                           SearchMode mode) {
  return std::visit([&](const auto& value) -> QueryResult {
    using T = std::decay_t<decltype(value)>;
    if constexpr (std::is_same_v<T,CreateTable>) return create_table(value, statement_candidate, store);
    else if constexpr (std::is_same_v<T,Insert>) return insert_rows(value, statement_candidate, store);
    else if constexpr (std::is_same_v<T,Select>) {
      // FlatScan owns no separate row state. Ann deliberately uses the exact
      // backend over the same statement-visible table, including without an index.
      switch (mode) {
        case SearchMode::Exact:
        case SearchMode::Ann:
          return select_rows(value, statement_candidate.find(value.table), store);
      }
      semantic("invalid_request", "unknown search mode");
    }
    else if constexpr (std::is_same_v<T,CreateVectorIndex>) {
      statement_candidate.add_vector_index({value.index, value.table, value.column});
      return EmptyResult{};
    }
    else if constexpr (std::is_same_v<T,Update> || std::is_same_v<T,Delete>) return rewrite_rows(value, statement_candidate, store);
    else semantic("not_implemented", "statement requires a later execution task");
  }, statement);
}
}  // namespace minivectordb
