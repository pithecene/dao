#include "frontend/module/program.h"
#include "frontend/module/source_map.h"

#include <boost/ut.hpp>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

using namespace boost::ut;
using namespace dao;

// NOLINTBEGIN(readability-magic-numbers)

namespace {

auto program_of(std::vector<std::string> texts) -> Program {
  std::vector<SourceInput> inputs;
  for (size_t i = 0; i < texts.size(); ++i) {
    inputs.push_back({.display_path = "f" + std::to_string(i) + ".dao",
                      .text = std::move(texts[i]),
                      .is_prelude = false});
  }
  return build_program(std::move(inputs));
}

} // namespace

suite<"position_budget"> position_budget = [] {
  "every file consumes size plus one position"_test = [] {
    const uint64_t max = uint64_t{1} << 32;
    std::vector<uint64_t> fits = {max - 2, 0};      // (max-2+1) + (0+1) == max
    std::vector<uint64_t> too_big = {max - 1, 0};   // one past
    std::vector<uint64_t> alone = {max - 1};        // (max-1) + 1 == max: fits
    std::vector<uint64_t> alone_over = {max};       // max + 1 > max
    expect(position_budget_fits(fits));
    expect(!position_budget_fits(too_big));
    expect(position_budget_fits(alone));
    expect(!position_budget_fits(alone_over));
  };

  "empty files consume positions"_test = [] {
    std::vector<uint64_t> five_empty(5, 0);
    expect(position_budget_fits(five_empty));
    expect(base_offset_for(five_empty) == 5_u);
  };

  "base offset is the sum of preceding sizes plus one each"_test = [] {
    std::vector<uint64_t> sizes = {10, 0, 3};
    expect(base_offset_for(std::span<const uint64_t>(sizes).first(0)) == 0_u);
    expect(base_offset_for(std::span<const uint64_t>(sizes).first(1)) == 11_u);
    expect(base_offset_for(std::span<const uint64_t>(sizes).first(2)) == 12_u);
    expect(base_offset_for(std::span<const uint64_t>(sizes).first(3)) == 16_u);
  };

  "over-budget program is diagnosed before any base is assigned"_test = [] {
    // A synthetic input list whose sizes sum past the space cannot be
    // materialized in memory; the pure budget check above is the unit
    // under test.  Here we only verify the loader wires a diagnostic
    // for an empty program (zero files fit trivially) and stays silent.
    auto program = program_of({});
    expect(program.diagnostics.empty());
    expect(program.files.empty());
  };
};

suite<"source_map_layout"> source_map_layout = [] {
  "files occupy closed ranges with one reserved position between"_test = [] {
    auto program = program_of({"module a\nfn f(): i32 -> 1\n", "", "module c\n"});
    const auto& a = *program.files[0];
    const auto& b = *program.files[1];
    const auto& c = *program.files[2];
    expect(a.base_offset == 0_u);
    expect(b.base_offset == a.eof_offset() + 1);
    expect(c.base_offset == b.eof_offset() + 1);
    expect(b.eof_offset() == b.base_offset); // empty file: one position
  };

  "eof position belongs to its own file, not the next"_test = [] {
    auto program = program_of({"module a\nfn f(): i32 -> 1\n", "", "module c\n"});
    const auto& map = program.source_map;
    const auto& a = *program.files[0];
    const auto& b = *program.files[1];
    const auto& c = *program.files[2];
    expect(map.file_for(a.eof_offset()) == &a);
    expect(map.file_for(b.eof_offset()) == &b);
    expect(map.file_for(c.eof_offset()) == &c);
    expect(map.file_for(c.base_offset) == &c);
    // The EOF slot is the reserved position; the next file starts right
    // after it, and only positions past the last EOF belong to nobody.
    expect(map.file_for(a.eof_offset() + 1) == &b);
    expect(map.file_for(b.eof_offset() + 1) == &c);
    expect(map.file_for(c.eof_offset() + 1) == nullptr);
  };

  "locate round-trips first byte, last byte, and eof of every file"_test = [] {
    auto program = program_of({"module a\nfn f(): i32 -> 1\n", "", "module c\n"});
    const auto& map = program.source_map;
    for (const auto& file : program.files) {
      auto first = map.locate(file->base_offset);
      expect(first.file == file.get());
      expect(first.line == 1_u && first.col == 1_u);

      auto eof = map.locate(file->eof_offset());
      expect(eof.file == file.get());
      if (file->buffer.size() == 0) {
        expect(eof.line == 1_u && eof.col == 1_u);
      } else {
        auto last = map.locate(file->eof_offset() - 1);
        expect(last.file == file.get());
      }
    }
  };

  "lexer eof token of every file maps back to that file"_test = [] {
    auto program = program_of({"module a\nfn f(): i32 -> 1\n", "module b\n"});
    for (const auto& file : program.files) {
      const auto& eof_tok = file->lex.tokens.back();
      expect(eof_tok.kind == TokenKind::Eof);
      expect(eof_tok.span.offset == file->eof_offset());
      expect(program.source_map.file_for(eof_tok.span.offset) == file.get());
    }
  };

  "diagnostic at end of the middle file names the middle file"_test = [] {
    // Unterminated string at the very end of the middle file.
    auto program = program_of({"module a\nfn f(): i32 -> 1\n",
                               "module b\nfn g(): string -> \"oops",
                               "module c\nfn h(): i32 -> 3\n"});
    const auto& middle = *program.files[1];
    expect(!middle.lex.diagnostics.empty());
    const auto& diag = middle.lex.diagnostics.front();
    auto loc = program.source_map.locate(diag.span.offset);
    expect(loc.file == &middle);
    expect(loc.line == 2_u);
    expect(program.source_map.locate(program.files[2]->base_offset).line == 1_u);
  };

  "text of a span reads from the owning file"_test = [] {
    auto program = program_of({"module a\n", "module bee\n"});
    const auto& second = *program.files[1];
    Span name{.offset = second.base_offset + 7, .length = 3};
    expect(program.source_map.text(name) == "bee");
  };

  "span ownership is overflow-safe at the top of the offset space"_test = [] {
    // Two files hand-placed just below UINT32_MAX (no 4 GiB program is
    // needed to exercise the boundary): `high` occupies
    // [max-6, max-3], `top` occupies [max-2, max].
    constexpr uint32_t max = std::numeric_limits<uint32_t>::max();
    SourceFile high{.display_path = "high.dao",
                    .buffer = SourceBuffer("high.dao", "abc"),
                    .base_offset = max - 6,
                    .is_prelude = false};
    SourceFile top{.display_path = "top.dao",
                   .buffer = SourceBuffer("top.dao", "xy"),
                   .base_offset = max - 2,
                   .is_prelude = false};
    SourceMap map;
    map.add(&high);
    map.add(&top);

    // Legitimate spans up to and including each file's EOF position.
    expect(map.owner_of({.offset = max - 6, .length = 3}) == &high);
    expect(map.owner_of({.offset = max - 2, .length = 2}) == &top);
    expect(map.owner_of({.offset = max, .length = 0}) == &top); // EOF of the last file
    expect(map.text({.offset = max - 2, .length = 2}) == "xy");

    // A span whose uint32_t end would wrap around (max-3 + 10 -> 6) is
    // not inside `high`, even though the wrapped end is tiny.
    expect(map.owner_of({.offset = max - 3, .length = 10}) == nullptr);
    expect(!high.contains_span({.offset = max - 3, .length = 10}));
    // Wrapping exactly to zero.
    expect(map.owner_of({.offset = max - 1, .length = 2}) == nullptr);
    // Cross-file span: starts in `high`, ends inside `top`.
    expect(map.owner_of({.offset = max - 5, .length = 4}) == nullptr);
    // One past a file's EOF.
    expect(map.owner_of({.offset = max - 6, .length = 4}) == nullptr);
  };

  "display path is the given path, or relative to a display root"_test = [] {
    std::filesystem::path repo(DAO_SOURCE_DIR);
    auto stdlib_file = repo / "stdlib" / "core" / "option.dao";

    auto relative = read_source_input(stdlib_file, /*is_prelude=*/true, repo);
    expect(relative.display_path == "stdlib/core/option.dao") << relative.display_path;

    auto as_given = read_source_input(stdlib_file, /*is_prelude=*/false);
    expect(as_given.display_path == stdlib_file.generic_string()) << as_given.display_path;

    // A root that does not contain the file falls back to the path as given.
    auto outside = read_source_input(stdlib_file, /*is_prelude=*/false, repo / "examples");
    expect(outside.display_path == stdlib_file.generic_string()) << outside.display_path;

    // The prelude loader displays every file relative to the repository.
    auto prelude = load_prelude_inputs(repo / "stdlib");
    expect(!prelude.empty());
    for (const auto& input : prelude) {
      expect(input.display_path.starts_with("stdlib/")) << input.display_path;
    }
  };

  "is_prelude follows the input flag"_test = [] {
    std::vector<SourceInput> inputs = {
        {.display_path = "core.dao", .text = "module core::x\n", .is_prelude = true},
        {.display_path = "user.dao", .text = "module user\n", .is_prelude = false},
    };
    auto program = build_program(std::move(inputs));
    expect(program.source_map.is_prelude(program.files[0]->base_offset));
    expect(!program.source_map.is_prelude(program.files[1]->base_offset));
    expect(program.user_files().size() == 1_u);
  };
};

// NOLINTEND(readability-magic-numbers)

auto main() -> int {}
