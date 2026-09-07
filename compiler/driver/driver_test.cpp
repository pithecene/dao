// Process-level tests of the `daoc` driver: what it does when invoked
// as users invoke it, not what its pieces do in isolation.

#include "support/test_utils.h"

#include <llvm/ADT/SmallVector.h>
#include <llvm/Support/Process.h>
#include <llvm/Support/Program.h>

#include <boost/ut.hpp>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <vector>

using namespace boost::ut;

namespace {

/// A directory of this invocation's own: two driver_test processes (say
/// from two build trees) must not share or delete each other's fixtures.
auto scratch_dir(std::string_view name) -> std::filesystem::path {
  auto root = std::filesystem::temp_directory_path() /
              std::format("dao_driver_test-{}", llvm::sys::Process::getProcessId());
  auto dir = root / name;
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  return dir;
}

/// Write a fixture, or fail the test naming the path: a fixture that did
/// not land would otherwise surface as a misleading build error later.
void write(const std::filesystem::path& path, std::string_view text) {
  std::ofstream out(path);
  out << text;
  out.close();
  expect(out.good()) << "cannot write fixture " << path.string();
}

/// Start `daoc build <source>` without waiting for it.
auto start_build(const std::filesystem::path& source) -> llvm::sys::ProcessInfo {
  std::string build = "build";
  std::string path = source.string();
  std::vector<llvm::StringRef> args = {DAO_DAOC, build, path};
  std::string error;
  bool failed = false;
  auto info = llvm::sys::ExecuteNoWait(DAO_DAOC, args, std::nullopt, {}, 0, &error, &failed);
  expect(!failed) << "cannot start daoc: " << error;
  return info;
}

/// Run an executable and return what it printed.
auto run_and_capture(const std::filesystem::path& exe, const std::filesystem::path& out)
    -> std::string {
  std::string exe_str = exe.string();
  std::string out_str = out.string();
  std::vector<llvm::StringRef> args = {exe_str};
  std::optional<llvm::StringRef> stdout_file = llvm::StringRef(out_str);
  int status = llvm::sys::ExecuteAndWait(
      exe_str, args, std::nullopt, {std::nullopt, stdout_file, std::nullopt});
  expect(status == 0) << exe.string() << " exited " << status;
  return dao::read_file(out);
}

} // namespace

suite<"driver"> driver_suite = [] {
  "concurrent builds of same-named files keep their own code"_test = [] {
    // Each build writes a temporary object; two builds of `hello.dao` in
    // different directories must not share it.
    auto a = scratch_dir("a");
    auto b = scratch_dir("b");
    write(a / "hello.dao", "module hello\nfn main(): i32\n  print(\"from a\")\n  return 0\n");
    write(b / "hello.dao", "module hello\nfn main(): i32\n  print(\"from b\")\n  return 0\n");

    for (int round = 0; round < 3; ++round) {
      auto build_a = start_build(a / "hello.dao");
      auto build_b = start_build(b / "hello.dao");
      std::string error;
      auto done_a = llvm::sys::Wait(build_a, std::nullopt, &error);
      expect(done_a.ReturnCode == 0) << "round " << round << " build a: " << error;
      auto done_b = llvm::sys::Wait(build_b, std::nullopt, &error);
      expect(done_b.ReturnCode == 0) << "round " << round << " build b: " << error;

      expect(run_and_capture(a / "hello", a / "out.txt") == "from a\n") << "round " << round;
      expect(run_and_capture(b / "hello", b / "out.txt") == "from b\n") << "round " << round;
    }
  };
};

auto main(int argc, const char** argv) -> int {
  return boost::ut::cfg<>.run({.report_errors = true, .argc = argc, .argv = argv}) ? 1 : 0;
}
