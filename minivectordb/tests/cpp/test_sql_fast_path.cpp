#include "sql_fast_path.hpp"
#include "test_storage_support.hpp"

int main() {
  auto parsed = try_parse_insert_values(
      "INSERT INTO items VALUES (1,2,'a','[0,1]'),(2,3,'b','[1,0]')");
  check(parsed.has_value());
  check(parsed->table == "items" && parsed->rows.size() == 2);
  check(std::get<std::int64_t>(parsed->rows[0][0]) == 1);
  check(std::get<std::string>(parsed->rows[0][2]) == "a");
  auto escaped = try_parse_insert_values(
      "insert into T values (-9223372036854775808,'a''b','中😀');");
  check(escaped.has_value() && escaped->table == "T");
  check(std::get<std::int64_t>(escaped->rows[0][0]) == INT64_MIN);
  check(std::get<std::string>(escaped->rows[0][1]) == "a'b");
  check(std::get<std::string>(escaped->rows[0][2]) == "中😀");
  check(!try_parse_insert_values("SELECT 1"));
  check(!try_parse_insert_values("INSERT INTO items(id) VALUES (1)"));
  check(!try_parse_insert_values("INSERT INTO items VALUES (1,'a\\\\b')"));
  check(!try_parse_insert_values("INSERT INTO items VALUES (+1)"));
  for (auto sql : {
           "INSERT INTO items VALUES (1,2),",
           "INSERT INTO items VALUES (1,2);;RESTART",
           "INSERT INTO items VALUES (1,2",
           "INSERT INTO items VALUES (9223372036854775808)"}) {
    rejects([&] { (void)try_parse_insert_values(sql); });
  }
}
