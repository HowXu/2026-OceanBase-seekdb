#pragma once

#include "types.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <vector>

// Internal format primitives. All integers and IEEE float bits are little
// endian.
namespace minivectordb::storage {
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559,
              "storage format requires IEEE binary32");
[[noreturn]] inline void fail(const char *message) {
  throw DbError("storage_error", message);
}
class Output {
public:
  explicit Output(const std::filesystem::path &path) {
    std::error_code ec;
    auto status = std::filesystem::symlink_status(path, ec);
    if (ec && ec != std::errc::no_such_file_or_directory)
      fail("cannot inspect output");
    if (std::filesystem::exists(status)) {
      if (!std::filesystem::is_regular_file(status))
        fail("output must be a regular file");
      auto size = std::filesystem::file_size(path, ec);
      if (ec || size != 0)
        fail("cannot overwrite immutable object");
    }
    stream_.open(path, std::ios::binary | std::ios::trunc);
    if (!stream_)
      fail("cannot open output");
  }
  void bytes(const char *p, std::size_t n) {
    stream_.write(p, n);
    if (!stream_)
      fail("write failed");
  }
  void u8(std::uint8_t n) {
    char c = static_cast<char>(n);
    bytes(&c, 1);
  }
  void u64(std::uint64_t n) {
    std::array<char, 8> raw;
    for (int i = 0; i < 8; ++i) raw[i] = static_cast<char>(n >> (8 * i));
    bytes(raw.data(), raw.size());
  }
  void u32(std::uint32_t n) {
    std::array<char, 4> raw;
    for (int i = 0; i < 4; ++i) raw[i] = static_cast<char>(n >> (8 * i));
    bytes(raw.data(), raw.size());
  }
  void floats(const float *source, std::uint64_t count) {
    if (count > UINT64_MAX / sizeof(float)) fail("vector byte size overflow");
    bytes(reinterpret_cast<const char *>(source),
          static_cast<std::size_t>(count * sizeof(float)));
  }
  void text(const std::string &s) {
    u64(s.size());
    bytes(s.data(), s.size());
  }
  void finish() {
    stream_.flush();
    if (!stream_)
      fail("flush failed");
    stream_.close();
    if (stream_.fail())
      fail("close failed");
  }

private:
  std::ofstream stream_;
};
class Input {
public:
  explicit Input(const std::filesystem::path &path)
      : stream_(path, std::ios::binary) {
    if (!stream_)
      fail("cannot open input");
    stream_.seekg(0, std::ios::end);
    auto n = stream_.tellg();
    if (n < 0)
      fail("cannot size input");
    left_ = static_cast<std::uint64_t>(n);
    stream_.seekg(0);
  }
  std::uint64_t remaining() const { return left_; }
  void bytes(char *p, std::size_t n) {
    if (n > left_)
      fail("truncated input");
    while (n > 0) {
      if (position_ == available_) {
        if (n >= buffer_size) {
          if (n > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max()))
            fail("input read too large");
          stream_.read(p, static_cast<std::streamsize>(n));
          if (stream_.gcount() != static_cast<std::streamsize>(n))
            fail("read failed");
          left_ -= n;
          return;
        }
        fill();
      }
      auto take = std::min(n, available_ - position_);
      std::memcpy(p, buffer_.data() + position_, take);
      p += take;
      n -= take;
      position_ += take;
      left_ -= take;
    }
  }
  void floats(float *destination, std::uint64_t count) {
    if (count > UINT64_MAX / sizeof(float))
      fail("vector byte size overflow");
    bytes(reinterpret_cast<char *>(destination),
          static_cast<std::size_t>(count * sizeof(float)));
  }
  std::uint8_t u8() {
    if (left_ == 0) fail("truncated input");
    if (position_ == available_) fill();
    auto value = static_cast<unsigned char>(buffer_[position_++]);
    --left_;
    return value;
  }
  std::uint64_t u64() {
    std::array<char, 8> raw;
    bytes(raw.data(), raw.size());
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i)
      value |= std::uint64_t(static_cast<unsigned char>(raw[i])) << (8 * i);
    return value;
  }
  std::uint32_t u32() {
    std::array<char, 4> raw;
    bytes(raw.data(), raw.size());
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i)
      value |= std::uint32_t(static_cast<unsigned char>(raw[i])) << (8 * i);
    return value;
  }
  std::string text() {
    auto n = u64();
    if (n > left_ || n > std::string{}.max_size())
      fail("invalid string length");
    std::string s(static_cast<std::size_t>(n), '\0');
    bytes(s.data(), s.size());
    return s;
  }
  void magic(const std::string &expected) {
    std::string value(expected.size(), '\0');
    bytes(value.data(), value.size());
    if (value != expected)
      fail("invalid file format");
  }
  void end() {
    if (left_ != 0)
      fail("trailing bytes");
  }

private:
  static constexpr std::size_t buffer_size = 64 * 1024;
  std::ifstream stream_;
  std::uint64_t left_ = 0;
  std::vector<char> buffer_;
  std::size_t position_ = 0;
  std::size_t available_ = 0;

  void fill() {
    auto want = static_cast<std::size_t>(std::min<std::uint64_t>(left_, buffer_size));
    buffer_.resize(buffer_size);
    stream_.read(buffer_.data(), static_cast<std::streamsize>(want));
    if (stream_.gcount() != static_cast<std::streamsize>(want))
      fail("read failed");
    position_ = 0;
    available_ = want;
  }
};
inline void write_schema(Output &out, const TableSchema &s) {
  out.text(s.name);
  out.u64(s.columns.size());
  for (const auto &c : s.columns) {
    out.text(c.name);
    out.u8(c.type == ColumnType::BigInt    ? 1
           : c.type == ColumnType::Varchar ? 2
                                           : 3);
    out.u64(c.length.value_or(0));
    out.u8(c.primary_key ? 1 : 0);
  }
}
inline TableSchema read_schema(Input &in) {
  TableSchema s;
  s.name = in.text();
  auto n = in.u64();
  if (n > in.remaining() / 19)
    fail("invalid column count");
  for (std::uint64_t i = 0; i < n; ++i) {
    auto name = in.text();
    auto tag = in.u8();
    auto length = in.u64();
    auto pk = in.u8();
    if (tag < 1 || tag > 3 || pk > 1 || (tag == 1 && length != 0))
      fail("invalid column encoding");
    s.columns.push_back(
        {name,
         tag == 1   ? ColumnType::BigInt
         : tag == 2 ? ColumnType::Varchar
                    : ColumnType::Vector,
         tag == 1 ? std::nullopt : std::optional<std::uint64_t>(length),
         pk == 1});
  }
  validate_schema(s);
  return s;
}
inline bool same_schema(const TableSchema &a, const TableSchema &b) {
  if (a.name != b.name || a.columns.size() != b.columns.size())
    return false;
  for (std::size_t i = 0; i < a.columns.size(); ++i) {
    const auto &x = a.columns[i];
    const auto &y = b.columns[i];
    if (x.name != y.name || x.type != y.type || x.length != y.length ||
        x.primary_key != y.primary_key)
      return false;
  }
  return true;
}
inline void validate_reference(const std::string &ref,
                               const std::string &directory,
                               const std::string &extension) {
  const std::string prefix = directory + "/";
  if (!ref.starts_with(prefix))
    fail("invalid object directory");
  auto leaf = ref.substr(prefix.size());
  if (leaf.size() <= extension.size() || !leaf.ends_with(extension))
    fail("invalid object extension");
  auto stem = leaf.substr(0, leaf.size() - extension.size());
  for (char c : stem)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_'))
      fail("unsafe object reference");
}
} // namespace minivectordb::storage
