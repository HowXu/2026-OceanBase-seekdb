#include "table_file.hpp"
#include "test_storage_support.hpp"
#include <fstream>
int main(int argc, char **) {
  TestDir d;
  auto s = schema();
  auto p = d.path / "table.tbl";
  Row a{std::string("中😀"), INT64_MIN, std::vector<float>{0.1f, -2}};
  Row b{std::string(""), INT64_C(9007199254740993), std::vector<float>{3, 4}};
  {
    TableFileWriter w(p, s);
    w.append(a);
    w.append(b);
    w.finish();
  }
  if (argc > 1) {
    auto size = std::filesystem::file_size(p);
    for (std::uintmax_t n = 0; n < size; ++n) {
      auto q = d.path / "cut.tbl";
      std::filesystem::copy_file(
          p, q, std::filesystem::copy_options::overwrite_existing);
      std::filesystem::resize_file(q, n);
      rejects([&] {
        TableFileReader r(q, s);
        Row row;
        while (r.next(row)) {
        };
      });
    }
    {
      std::ofstream f(p, std::ios::app | std::ios::binary);
      f.put('x');
    }
    rejects([&] {
      TableFileReader r(p, s);
      Row row;
      while (r.next(row)) {
      };
    });
    return 0;
  }
  {
    TableFileReader r(p, s);
    Row row;
    check(r.next(row) && row == a);
    check(r.next(row) && row == b);
    check(!r.next(row));
    check(!r.next(row));
  }
  auto empty = d.path / "empty.tbl";
  {
    TableFileWriter w(empty, s);
    w.finish();
  }
  {
    TableFileReader r(empty, s);
    Row row;
    check(!r.next(row));
  }
  auto ordered = d.path / "ordered.tbl";
  {
    TableFileWriter w(ordered, s);
    w.append(b);
    rejects([&] { w.append(b); });
    rejects([&] { w.append(a); });
    w.finish();
  }
  auto wrong = s;
  wrong.columns[0].length = 4;
  rejects([&] { TableFileReader r(p, wrong); });
  // A finalized immutable object cannot be reopened as a candidate writer.
  rejects([&] { TableFileWriter overwrite(p, s); });
  {
    TableFileReader r(p, s);
    Row row;
    check(r.next(row) && row == a);
  }
  // Independently inspect a minimal layout: explicit tags, row framing, LE
  // int64.
  TableSchema tiny{"t", {{"id", ColumnType::BigInt, {}, true}}, 0};
  auto q = d.path / "tiny.tbl";
  {
    TableFileWriter w(q, tiny);
    w.append(Row{INT64_C(1)});
    w.append(Row{INT64_MAX});
    w.finish();
  }
  std::ifstream input(q, std::ios::binary);
  std::string bytes((std::istreambuf_iterator<char>(input)), {});
  check(bytes.size() == 80 && bytes.substr(0, 8) == "MVDBT001");
  check(static_cast<unsigned char>(bytes[45]) == 1 &&
        static_cast<unsigned char>(bytes[46]) == 8);
  check(static_cast<unsigned char>(bytes[54]) == 1 &&
        static_cast<unsigned char>(bytes[55]) == 0);
  check(static_cast<unsigned char>(bytes[71]) == 255 &&
        static_cast<unsigned char>(bytes[78]) == 127);
  // Corrupt the second key to equal the first, preserving otherwise valid
  // framing.
  for (std::size_t i = 71; i < 79; ++i)
    bytes[i] = i == 71 ? 1 : 0;
  {
    std::ofstream output(q, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), bytes.size());
  }
  rejects([&] {
    TableFileReader r(q, tiny);
    Row row;
    while (r.next(row)) {
    };
  });
  // Large text and float blocks cross the bounded input buffer. The logical
  // remaining count must include buffered bytes and preserve every float bit.
  auto blocks = d.path / "blocks.bin";
  std::string large(70000, 'x');
  std::vector<float> floats(65536);
  for (std::size_t i = 0; i < floats.size(); ++i)
    floats[i] = i == 0 ? -0.f : std::bit_cast<float>(static_cast<std::uint32_t>(i));
  {
    storage::Output out(blocks);
    out.text(large);
    for (float value : floats) out.u32(std::bit_cast<std::uint32_t>(value));
    out.finish();
  }
  {
    storage::Input in(blocks);
    check(in.text() == large);
    std::vector<float> decoded(floats.size());
    in.floats(decoded.data(), decoded.size());
    in.end();
    for (std::size_t i = 0; i < floats.size(); ++i)
      check(std::bit_cast<std::uint32_t>(decoded[i]) ==
            std::bit_cast<std::uint32_t>(floats[i]));
  }
}
