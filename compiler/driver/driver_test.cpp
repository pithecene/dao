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
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
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

/// Run an executable for its exit status, which is how these fixtures
/// report a value without needing the real prelude's `print`.
auto exit_status(const std::filesystem::path& exe) -> int {
  std::string exe_str = exe.string();
  std::vector<llvm::StringRef> args = {exe_str};
  return llvm::sys::ExecuteAndWait(exe_str, args);
}

/// A scratch program the CLI tests lay out file by file, with a stdlib
/// root of its own.  That root is empty unless a test fills it, so the
/// real prelude is neither read nor parsed and every name a fixture
/// uses is one the fixture wrote.
struct Scratch {
  std::filesystem::path dir;
  std::filesystem::path stdlib;

  explicit Scratch(std::string_view name) : dir(scratch_dir(name)), stdlib(dir / "stdlib") {
    std::filesystem::create_directories(stdlib);
  }

  auto file(const std::filesystem::path& relative, std::string_view text) const
      -> std::filesystem::path {
    auto path = dir / relative;
    std::filesystem::create_directories(path.parent_path());
    write(path, text);
    return path;
  }

  /// Where `daoc build` puts the executable for a source file: beside
  /// the file it names its output after.
  [[nodiscard]] auto output_for(const std::filesystem::path& source) const
      -> std::filesystem::path {
    auto named = dao::canonical_or_self(source);
    return named.parent_path() / named.stem();
  }
};

/// What one `daoc` invocation did.  Every CLI test asserts on the exit
/// code and on the stream the driver actually writes to: diagnostics go
/// to stderr, results to stdout.
struct Output {
  int exit_code = 0;
  std::string out;
  std::string err;

  [[nodiscard]] auto err_says(std::string_view text) const -> bool {
    return err.find(text) != std::string::npos;
  }
};

auto run_daoc(const Scratch& scratch, const std::vector<std::string>& args) -> Output {
  auto out_path = (scratch.dir / "stdout.txt").string();
  auto err_path = (scratch.dir / "stderr.txt").string();
  std::vector<llvm::StringRef> argv;
  argv.reserve(args.size() + 1);
  argv.emplace_back(DAO_DAOC);
  for (const auto& arg : args) {
    argv.emplace_back(arg);
  }
  std::optional<llvm::StringRef> to_out = llvm::StringRef(out_path);
  std::optional<llvm::StringRef> to_err = llvm::StringRef(err_path);
  int status =
      llvm::sys::ExecuteAndWait(DAO_DAOC, argv, std::nullopt, {std::nullopt, to_out, to_err});
  return {.exit_code = status, .out = dao::read_file(out_path), .err = dao::read_file(err_path)};
}

/// Compile a C snippet with the same `cc` the driver links through, so
/// the link-passthrough test has a real object to pass.
auto compile_object(const Scratch& scratch, std::string_view code) -> std::filesystem::path {
  auto source = scratch.dir / "helper.c";
  write(source, code);
  auto object = scratch.dir / "helper.o";
  auto cc = llvm::sys::findProgramByName("cc");
  expect(static_cast<bool>(cc)) << "cannot find 'cc'";
  auto source_str = source.string();
  auto object_str = object.string();
  std::vector<llvm::StringRef> args = {*cc, "-c", source_str, "-o", object_str};
  expect(llvm::sys::ExecuteAndWait(*cc, args) == 0) << "cc could not compile " << source_str;
  return object;
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

// The CLI forms of Task 31 §13, each exercised as the command line a
// user types.  The loaders are tested directly in module_graph_test;
// what these add is that the driver reaches them with the arguments it
// was given, in the order it was given them.
suite<"driver_cli"> driver_cli_suite = [] {
  "a root file's imports are discovered from its own directory"_test = [] {
    const Scratch scratch("root-discovery");
    auto root = scratch.file(
        "main.dao", "module main\nimport app::util\n\nfn main(): i32\n  return util::one()\n");
    scratch.file("app/util.dao", "module app::util\n\nfn one(): i32 -> 3\n");

    // `check`, not `build`: a qualified cross-module call resolves and
    // type-checks here, but lowering one is D4's work (§17.4).  What
    // this asks is whether discovery found `app/util.dao` at all --
    // unfound, the qualified call would not resolve.
    auto checked =
        run_daoc(scratch, {"check", root.string(), "--stdlib-root", scratch.stdlib.string()});
    expect(checked.exit_code == 0) << checked.err;
    expect(checked.out == "ok\n") << "the imported module was not discovered: " << checked.out;

    // The dumps report every user module, each under its path when
    // there is more than one -- the root and the import it discovered.
    auto dumped =
        run_daoc(scratch, {"tokens", root.string(), "--stdlib-root", scratch.stdlib.string()});
    expect(dumped.exit_code == 0) << dumped.err;
    expect(dumped.out.find("decl.function main") != std::string::npos) << dumped.out;
    expect(dumped.out.find("== ") != std::string::npos &&
           dumped.out.find("app/util.dao") != std::string::npos)
        << "the discovered import must be reported under its own path: " << dumped.out;
    expect(dumped.out.find("decl.function one") != std::string::npos)
        << "the discovered import's tokens are missing: " << dumped.out;
  };

  "module roots are searched in command-line order"_test = [] {
    const Scratch scratch("ordered-module-roots");
    // The two candidates EXPORT DIFFERENT NAMES, so which one was loaded
    // is decided in resolution and visible to `check`: lowering a
    // cross-module call is D4's work (§17.4) and type-checking one is
    // D3's, so neither execution nor a type mismatch can be the witness.
    auto root = scratch.file(
        "main.dao", "module main\nimport ext::thing\n\nfn main(): i32\n  return thing::value()\n");
    scratch.file("first/ext/thing.dao", "module ext::thing\n\nfn value(): i32 -> 1\n");
    scratch.file("second/ext/thing.dao", "module ext::thing\n\nfn other(): i32 -> 2\n");

    auto build_with = [&](std::string_view earlier, std::string_view later) {
      return run_daoc(scratch,
                      {"check",
                       root.string(),
                       "--module-root",
                       (scratch.dir / earlier).string(),
                       "--module-root",
                       (scratch.dir / later).string(),
                       "--stdlib-root",
                       scratch.stdlib.string()});
    };
    auto first_wins = build_with("first", "second");
    expect(first_wins.exit_code == 0) << "the later --module-root won: " << first_wins.err;
    auto second_wins = build_with("second", "first");
    expect(second_wins.exit_code != 0)
        << "the roots are not searched in order: the earlier root did not win";
    expect(second_wins.err.find("has no export 'value'") != std::string::npos ||
           second_wins.out.find("has no export 'value'") != std::string::npos)
        << second_wins.err << second_wins.out;
  };

  "an explicit set with no entry is an error even under check"_test = [] {
    const Scratch scratch("explicit-no-entry");
    auto a = scratch.file("a.dao", "module a\n\nfn one(): i32 -> 1\n");
    auto b = scratch.file("b.dao", "module b\n\nfn two(): i32 -> 2\n");
    auto checked = run_daoc(scratch,
                            {"check",
                             "--source",
                             a.string(),
                             "--source",
                             b.string(),
                             "--stdlib-root",
                             scratch.stdlib.string()});
    expect(checked.exit_code != 0)
        << "an explicit set without an entry was accepted: " << checked.out;
    expect(checked.err_says("no entry module")) << checked.err;
  };

  "an unfound import names every root, in the order they were searched"_test = [] {
    const Scratch scratch("searched-roots");
    auto root =
        scratch.file("main.dao", "module main\nimport ext::thing\n\nfn main(): i32\n  return 0\n");
    auto first = scratch.dir / "first";
    auto second = scratch.dir / "second";

    auto result = run_daoc(scratch,
                           {"check",
                            root.string(),
                            "--module-root",
                            first.string(),
                            "--module-root",
                            second.string(),
                            "--stdlib-root",
                            scratch.stdlib.string()});
    // §8.2 fixes the order — the root's directory, each --module-root in
    // command-line order, then the stdlib root — and this message is
    // what tells the user which directory was expected to hold the file.
    auto expected = "searched " + scratch.dir.generic_string() + " " + first.generic_string() +
                    " " + second.generic_string() + " " + scratch.stdlib.generic_string();
    expect(result.exit_code != 0) << result.out;
    expect(result.err_says(expected)) << result.err << "\nwant: " << expected;
  };

  "--stdlib-root replaces the prelude the driver would load"_test = [] {
    const Scratch scratch("stdlib-root");
    scratch.file("stdlib/core/greet.dao", "module core::greet\n\nfn greeting(): i32 -> 5\n");
    auto root = scratch.file("main.dao", "module main\n\nfn main(): i32\n  return greeting()\n");

    auto overridden =
        run_daoc(scratch, {"build", root.string(), "--stdlib-root", scratch.stdlib.string()});
    expect(overridden.exit_code == 0) << overridden.err;
    expect(exit_status(scratch.output_for(root)) == 5);

    auto defaulted = run_daoc(scratch, {"check", root.string()});
    expect(defaulted.exit_code != 0) << "the built-in prelude declares no 'greeting'";
  };

  "--source takes an explicit set and --entry names its entry"_test = [] {
    const Scratch scratch("explicit-sources");
    auto library = scratch.file("lib.dao", "module lib\n\nfn helper(): i32 -> 8\n");
    auto app = scratch.file("app.dao",
                            "module app\nimport lib\n\nfn main(): i32\n  return lib::helper()\n");
    auto sources = [&](std::vector<std::string> extra) {
      std::vector<std::string> args = {"check",
                                       "--source",
                                       library.string(),
                                       "--source",
                                       app.string(),
                                       "--stdlib-root",
                                       scratch.stdlib.string()};
      args.insert(args.end(), extra.begin(), extra.end());
      return run_daoc(scratch, args);
    };

    auto implicit = sources({});
    expect(implicit.exit_code == 0) << implicit.err;
    expect(implicit.out == "ok\n") << implicit.out;

    auto named = sources({"--entry", "app"});
    expect(named.exit_code == 0) << named.err;

    // An explicit file set owes an entry module whatever the command
    // (§8.3): naming one that declares no `fn main` is an error.
    auto without_main = sources({"--entry", "lib"});
    expect(without_main.exit_code != 0) << without_main.out;
    expect(without_main.err_says("entry module 'lib' (--entry) declares no 'fn main'"))
        << without_main.err;
  };

  "several mains without --entry are rejected"_test = [] {
    const Scratch scratch("ambiguous-entry");
    auto first = scratch.file("a.dao", "module a\n\nfn main(): i32\n  return 1\n");
    auto second = scratch.file("b.dao", "module b\n\nfn main(): i32\n  return 2\n");

    auto result = run_daoc(scratch,
                           {"check",
                            "--source",
                            first.string(),
                            "--source",
                            second.string(),
                            "--stdlib-root",
                            scratch.stdlib.string()});
    expect(result.exit_code != 0) << result.out;
    expect(result.err_says("ambiguous entry module: 'fn main' declared in a, b (use --entry)"))
        << result.err;
  };

  "a root file given with --source is rejected"_test = [] {
    const Scratch scratch("mixed-inputs");
    auto root = scratch.file("root.dao", "module root\n\nfn main(): i32\n  return 0\n");
    auto source = scratch.file("other.dao", "module other\n\nfn one(): i32 -> 1\n");

    // The two describe different programs; taking one and dropping the
    // other would compile something the command line did not ask for.
    auto result = run_daoc(scratch, {"check", root.string(), "--source", source.string()});
    expect(result.exit_code != 0) << result.out;
    expect(result.err_says("give a root file or --source inputs, not both")) << result.err;
  };

  "the single-file dumps reject --source inputs"_test = [] {
    const Scratch scratch("single-file-dumps");
    auto source = scratch.file("a.dao", "module a\n\nfn one(): i32 -> 1\n");

    for (const auto* command : {"lex", "parse", "ast"}) {
      auto result = run_daoc(scratch, {command, "--source", source.string()});
      expect(result.exit_code != 0) << command << ": " << result.out;
      expect(result.err_says("this command takes a single file, not --source inputs"))
          << command << ": " << result.err;
    }
  };

  "link inputs after --source still reach the linker"_test = [] {
    const Scratch scratch("link-passthrough");
    auto source = scratch.file(
        "app.dao", "module app\nextern fn helper(): i32\n\nfn main(): i32\n  return helper()\n");
    auto object = compile_object(scratch, "int helper(void) { return 7; }\n");

    // `build <inputs> [link-inputs...]`: a positional after --source is
    // a link input, not a second way to name the program.
    auto built = run_daoc(scratch,
                          {"build",
                           "--source",
                           source.string(),
                           "--stdlib-root",
                           scratch.stdlib.string(),
                           object.string()});
    expect(built.exit_code == 0) << built.err;
    expect(exit_status(scratch.output_for(source)) == 7) << "the object never reached 'cc'";
  };

  "an explicit set names its output by the set, not by the order"_test = [] {
    const Scratch scratch("alias-order");
    auto real = scratch.file("real.dao", "module app\n\nfn main(): i32\n  return 4\n");
    auto alias = scratch.dir / "zzz.dao";
    std::error_code ec;
    std::filesystem::create_symlink(real, alias, ec);
    expect(!ec) << "cannot create symlink: " << ec.message();

    // Two spellings of one file are one source — the loader already
    // deduplicates them — so the same set in either order must build the
    // same program under the same name (§8.4).  An alias with its own
    // stem is what makes a spelling-ranked choice visible.
    auto build = [&](const std::filesystem::path& first, const std::filesystem::path& second) {
      return run_daoc(scratch,
                      {"build",
                       "--source",
                       first.string(),
                       "--source",
                       second.string(),
                       "--stdlib-root",
                       scratch.stdlib.string()});
    };
    auto forward = build(real, alias);
    auto reversed = build(alias, real);
    expect(forward.exit_code == 0) << forward.err;
    expect(reversed.exit_code == 0) << reversed.err;
    expect(forward.out == reversed.out) << forward.out << " vs " << reversed.out;
    expect(forward.out == scratch.output_for(real).string() + "\n") << forward.out;
    expect(exit_status(scratch.output_for(real)) == 4);
  };

  "a prelude root that imports a user module is still reported"_test = [] {
    // The root is a prelude file, and it imports a user module, so the
    // user-file set is not empty -- the root must be reported anyway,
    // since it is what the command named.
    const Scratch scratch("prelude-root-imports");
    auto base = scratch.file("stdlib/core/base.dao",
                             "module core::base\nimport ext::thing\n\nfn base_one(): i32 -> 1\n");
    scratch.file("ext/thing.dao", "module ext::thing\n\nfn thing_one(): i32 -> 2\n");
    auto dumped = run_daoc(scratch,
                           {"tokens",
                            base.string(),
                            "--module-root",
                            scratch.dir.string(),
                            "--stdlib-root",
                            scratch.stdlib.string()});
    expect(dumped.exit_code == 0) << dumped.err;
    expect(dumped.out.find("decl.function base_one") != std::string::npos)
        << "the prelude root vanished from the dump: " << dumped.out;
    expect(dumped.out.find("decl.function thing_one") != std::string::npos)
        << "the imported user module is missing: " << dumped.out;
  };

  "what leaves a resource block is copied out per type"_test = [] {
    // The MIR the driver hands the backend carries the copies the
    // block's exit makes: a class with its own `copy_out` through that
    // method (the prelude's Vector through its specialization), a
    // string through the prelude's copy, a plain class field by field,
    // an enum per variant -- and no identity `copy_out$T` left behind
    // for any of them (a non-owning T is the only one that keeps it).
    const Scratch scratch("copy-out-mir");
    auto root = scratch.file("main.dao",
                             "module app::main\n\n"
                             "class Box:\n"
                             "  text: string\n\n"
                             "  fn copy_out(self): Box\n"
                             "    return Box(self.text)\n\n"
                             "class Label:\n"
                             "  name: string\n"
                             "  count: i32\n\n"
                             "class Odd:\n"
                             "  text: string\n\n"
                             "  fn copy_out(self, extra: i32): Odd\n"
                             "    return Odd(self.text)\n\n"
                             "class Other:\n"
                             "  text: string\n\n"
                             "  fn copy_out(self): string\n"
                             "    return self.text\n\n"
                             "class Wrap<T>:\n"
                             "  item: T\n\n"
                             "  fn copy_out(self): Wrap<i32>\n"
                             "    return Wrap(0)\n\n"
                             "class Tag<T>:\n"
                             "  text: string\n\n"
                             "  fn copy_out(self): Tag<i32>\n"
                             "    return Tag(\"wrong\")\n\n"
                             "class Stat:\n"
                             "  text: string\n\n"
                             "  fn copy_out(value: Stat): Stat\n"
                             "    return Stat(\"wrong\")\n\n"
                             "class Gen:\n"
                             "  text: string\n\n"
                             "  fn copy_out<U>(self): Gen\n"
                             "    return Gen(\"wrong\")\n\n"

                             "enum class Slot:\n"
                             "  Empty\n"
                             "  Full(text: string)\n\n"
                             "fn numbers(): i64\n"
                             "  let nums: Vector<i64> = Vector<i64>::new()\n"
                             "  resource memory pool =>\n"
                             "    nums = nums.push(1)\n"
                             "  return nums.length()\n\n"
                             "fn main(): i32\n"
                             "  let box: Box = Box(\"b\")\n"
                             "  let items: Vector<string> = Vector<string>::new()\n"
                             "  let label: Label = Label(\"l\", 1)\n"
                             "  let odd: Odd = Odd(\"o\")\n"
                             "  let other: Other = Other(\"o\")\n"
                             "  let wrap: Wrap<string> = Wrap(\"w\")\n"
                             "  let tag: Tag<string> = Tag(\"t\")\n"
                             "  let stat: Stat = Stat(\"s\")\n"
                             "  let gen: Gen = Gen(\"g\")\n"

                             "  let slot: Slot = Slot::Empty\n"
                             "  resource memory pool =>\n"
                             "    box = Box(\"in\")\n"
                             "    items = items.push(\"in\")\n"
                             "    label = Label(\"in\", 2)\n"
                             "    odd = Odd(\"in\")\n"
                             "    other = Other(\"in\")\n"
                             "    wrap = Wrap(\"in\")\n"
                             "    tag = Tag(\"in\")\n"
                             "    stat = Stat(\"in\")\n"
                             "    gen = Gen(\"in\")\n"

                             "    slot = Slot::Full(text = \"in\")\n"
                             "  return label.count\n");
    auto dumped = run_daoc(scratch, {"mir", root.string()});
    expect(dumped.exit_code == 0) << dumped.err;
    // Each vector element type gets its own copier: the string one is
    // made first and must not answer for the i64 one.
    for (auto expected : {"fn_ref Box.copy_out ",
                          "fn_ref Vector.copy_out$string ",
                          "fn_ref Vector.copy_out$i64 ",
                          "fn_ref copy_out_string ",
                          "enum_discriminant"}) {
      expect(dumped.out.find(expected) != std::string::npos)
          << "no `" << expected << "` in the MIR: " << dumped.out;
    }
    // The one identity specialization is for Vector<i64>'s non-owning
    // element type; every owning type's copy was expanded.
    for (size_t at = dumped.out.find("fn copy_out$"); at != std::string::npos;
         at = dumped.out.find("fn copy_out$", at + 1)) {
      expect(dumped.out.compare(at, 15, "fn copy_out$i64") == 0)
          << "an identity copy_out specialization was left behind: " << dumped.out.substr(at, 40);
    }
    // A method named copy_out with another signature is not the copier:
    // extra parameters, another return type, another instantiation of
    // the class (even one a phantom parameter cannot tell apart), no
    // receiver at all, or type parameters of its own.
    for (auto not_a_copier : {"fn_ref Odd.copy_out ",
                              "fn_ref Other.copy_out ",
                              "fn_ref Wrap.copy_out",
                              "fn_ref Tag.copy_out",
                              "fn_ref Stat.copy_out",
                              "fn_ref Gen.copy_out"}) {
      expect(dumped.out.find(not_a_copier) == std::string::npos)
          << "`" << not_a_copier << "` was taken for the copier: " << dumped.out;
    }
  };

  "a copier declared in a conformance block is diagnosed"_test = [] {
    // Conformance-block methods are lowered without a symbol, so such a
    // copy_out cannot be called; the class must declare it directly.
    const Scratch scratch("copy-out-conformance");
    auto root = scratch.file("main.dao",
                             "module app::main\n\n"
                             "concept Copier:\n"
                             "  fn copy_out(self): Conf\n\n"
                             "class Conf:\n"
                             "  text: string\n\n"
                             "  as Copier:\n"
                             "    fn copy_out(self): Conf\n"
                             "      return Conf(self.text)\n\n"
                             "fn main(): i32\n"
                             "  let conf: Conf = Conf(\"c\")\n"
                             "  resource memory pool =>\n"
                             "    conf = Conf(\"in\")\n"
                             "  return 0\n");
    auto dumped = run_daoc(scratch, {"mir", root.string()});
    expect(dumped.exit_code != 0) << "a conformance-block copier was silently ignored";
    expect(dumped.err_says("declares its copy_out inside a conformance block")) << dumped.err;
  };

  "an extension's copy_out is not the class's copier"_test = [] {
    // An `extend` method is emitted under the same `<Class>.copy_out`
    // symbol shape; only a method the class declares itself copies it.
    const Scratch scratch("copy-out-extension");
    auto root = scratch.file("main.dao",
                             "module app::main\n\n"
                             "class Label:\n"
                             "  name: string\n\n"
                             "concept Copier:\n"
                             "  fn copy_out(self): Label\n\n"
                             "extend Label as Copier:\n"
                             "  fn copy_out(self): Label\n"
                             "    return Label(\"wrong\")\n\n"
                             "fn main(): i32\n"
                             "  let label: Label = Label(\"l\")\n"
                             "  resource memory pool =>\n"
                             "    label = Label(\"in\")\n"
                             "  return 0\n");
    auto dumped = run_daoc(scratch, {"mir", root.string()});
    expect(dumped.exit_code == 0) << dumped.err;
    expect(dumped.out.find("fn_ref Label.copy_out ") == std::string::npos)
        << "the extension's copy_out was taken for the copier: " << dumped.out;
    expect(dumped.out.find("fn_ref copy_out_string ") != std::string::npos)
        << "the label was not copied field by field: " << dumped.out;
  };

  "a container of generators may not leave a resource block"_test = [] {
    // A vector reaches its elements through a raw pointer; a generator
    // among them has no copier, so the store is rejected up front.
    const Scratch scratch("copy-out-generator-container");
    auto root = scratch.file("main.dao",
                             "module app::main\n\n"
                             "fn gen(): Generator<i32>\n"
                             "  yield 1\n\n"
                             "fn main(): i32\n"
                             "  let items: Vector<Generator<i32>> = Vector<Generator<i32>>::new()\n"
                             "  resource memory pool =>\n"
                             "    items = items.push(gen())\n"
                             "  return 0\n");
    auto checked = run_daoc(scratch, {"check", root.string()});
    expect(checked.exit_code != 0) << "a vector of generators left the block";
    expect(checked.err_says("a generator cannot be copied out of the block")) << checked.err;

    // Pointers among the elements are pointer values, the author's.
    auto pointers =
        scratch.file("pointers.dao",
                     "module app::pointers\n\n"
                     "fn main(): i32\n"
                     "  let ptrs: Vector<*Generator<i32>> = Vector<*Generator<i32>>::new()\n"
                     "  resource memory pool =>\n"
                     "    ptrs = ptrs.push(null_ptr<Generator<i32>>())\n"
                     "  return 0\n");
    auto accepted = run_daoc(scratch, {"check", pointers.string()});
    expect(accepted.exit_code == 0) << accepted.err;
  };

  "a stdlib file compiled as the root keeps its root role"_test = [] {
    const Scratch scratch("prelude-root");
    auto library = scratch.file("stdlib/core/lib.dao", "module core::lib\n\nfn one(): i32 -> 1\n");
    auto app =
        scratch.file("stdlib/core/app.dao", "module core::app\n\nfn main(): i32\n  return 6\n");

    // The program holds one copy of the root, in the prelude group, so
    // it has no user file at all.  Entry selection must still find the
    // root through the spelling the program kept.
    // `check` only advises on a missing entry (§8.1), so the witness is
    // the diagnostic naming the root, not the exit status.
    auto no_main =
        run_daoc(scratch, {"check", library.string(), "--stdlib-root", scratch.stdlib.string()});
    expect(no_main.exit_code == 0) << no_main.err;
    expect(no_main.err_says("entry module 'core::lib' (the root file) declares no 'fn main'"))
        << no_main.err;

    auto dumped =
        run_daoc(scratch, {"tokens", app.string(), "--stdlib-root", scratch.stdlib.string()});
    expect(dumped.exit_code == 0) << "tokens indexed an empty user-file set: " << dumped.err;
    expect(dumped.out.find("decl.function main") != std::string::npos) << dumped.out;

    auto built =
        run_daoc(scratch, {"build", app.string(), "--stdlib-root", scratch.stdlib.string()});
    expect(built.exit_code == 0) << built.err;
    expect(exit_status(scratch.output_for(app)) == 6);
  };
};

auto main(int argc, const char** argv) -> int {
  return boost::ut::cfg<>.run({.report_errors = true, .argc = argc, .argv = argv}) ? 1 : 0;
}
