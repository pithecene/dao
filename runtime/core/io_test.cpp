// io_test.cpp — Runtime tests for non-trapping file reads and process arguments.
//
// Authority: docs/contracts/CONTRACT_RUNTIME_ABI.md

#include "dao_abi.h"

#include <boost/ut.hpp>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <unistd.h>

using namespace boost::ut;

namespace {

auto as_dao_string(std::string_view text) -> dao_string {
  return {.ptr = text.data(), .len = static_cast<int64_t>(text.size())};
}

auto to_std_string(const dao_string& str) -> std::string {
  if (str.ptr == nullptr || str.len <= 0) {
    return {};
  }
  return {str.ptr, static_cast<size_t>(str.len)};
}

auto temp_dir(std::string_view label) -> std::filesystem::path {
  auto dir = std::filesystem::temp_directory_path() /
             std::format("dao_io_test_{}_{}", label, static_cast<long>(getpid()));
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  return dir;
}

} // namespace

suite<"io_hooks"> io_hooks_suite = [] {
  "try_read_file reads regular and empty files"_test = [] {
    auto dir = temp_dir("pos");

    auto regular = dir / "sample.txt";
    {
      std::ofstream out(regular, std::ios::binary);
      out << "hello\nworld";
    }
    auto regular_str = regular.string();
    auto regular_path = as_dao_string(regular_str);
    int32_t ok = 0;
    auto data = __dao_io_try_read_file(&regular_path, &ok);
    expect(eq(ok, int32_t{1})) << to_std_string(data);
    expect(to_std_string(data) == "hello\nworld");

    auto empty = dir / "empty.txt";
    {
      std::ofstream out(empty, std::ios::binary);
    }
    auto empty_str = empty.string();
    auto empty_path = as_dao_string(empty_str);
    int32_t empty_ok = 0;
    auto empty_data = __dao_io_try_read_file(&empty_path, &empty_ok);
    expect(eq(empty_ok, int32_t{1})) << to_std_string(empty_data);
    expect(eq(empty_data.len, int64_t{0}));

    std::filesystem::remove_all(dir);
  };

  "try_read_file reports missing files, directories, empty paths, and NUL bytes"_test = [] {
    auto dir = temp_dir("neg");

    int32_t ok = 1;
    dao_string empty_path{.ptr = "", .len = 0};
    auto empty_err = __dao_io_try_read_file(&empty_path, &ok);
    expect(eq(ok, int32_t{0}));
    expect(to_std_string(empty_err).find("empty") != std::string::npos);

    auto missing_str = (dir / "does_not_exist.dao").string();
    auto missing_path = as_dao_string(missing_str);
    ok = 1;
    auto missing_err = __dao_io_try_read_file(&missing_path, &ok);
    expect(eq(ok, int32_t{0}));
    expect(missing_err.len > 0);

    auto dir_str = dir.string();
    auto dir_path = as_dao_string(dir_str);
    ok = 1;
    auto dir_err = __dao_io_try_read_file(&dir_path, &ok);
    expect(eq(ok, int32_t{0}));
    expect(to_std_string(dir_err).find("directory") != std::string::npos);

    const char nul_bytes[] = {'a', '\0', 'b'};
    dao_string nul_path{.ptr = nul_bytes, .len = 3};
    ok = 1;
    auto nul_err = __dao_io_try_read_file(&nul_path, &ok);
    expect(eq(ok, int32_t{0}));
    expect(to_std_string(nul_err).find("NUL") != std::string::npos);

    std::filesystem::remove_all(dir);
  };

  "arg_count and arg_at expose process arguments in fresh buffers"_test = [] {
    auto count = __dao_io_arg_count();
    expect(count >= 1) << "process must report at least argv[0]";
    auto first_a = __dao_io_arg_at(0);
    auto first_b = __dao_io_arg_at(0);
    expect(first_a.len > 0);
    expect(first_a.ptr != first_b.ptr) << "arg_at must allocate fresh domain storage";
    expect(to_std_string(first_a) == to_std_string(first_b));
    expect(to_std_string(first_a).find("io_test") != std::string::npos);
  };
};

auto main() -> int {} // NOLINT(readability-named-parameter)
