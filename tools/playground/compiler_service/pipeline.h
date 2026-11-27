#ifndef DAO_PLAYGROUND_PIPELINE_H
#define DAO_PLAYGROUND_PIPELINE_H

#include "frontend/diagnostics/diagnostic.h"
#include "frontend/lexer/lexer.h"
#include "frontend/module/program.h"
#include "frontend/module/source_map.h"
#include "frontend/resolve/resolve.h"
#include "frontend/typecheck/type_checker.h"
#include "frontend/types/type_context.h"

#include <nlohmann/json.hpp>

#include <cassert>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dao::playground {

// ---------------------------------------------------------------------------
// The editor counts a position in UTF-16 code units, as its buffer and
// the language-server protocol do, and the compiler's spans count
// bytes.  The two agree until the first character outside ASCII and
// differ after it, so every offset and length that crosses the
// service's boundary is converted here, in one place per file.
// ---------------------------------------------------------------------------

/// One file's byte positions in UTF-16 code units, and back.
///
/// A position inside a character is that character's start, in either
/// direction: a byte offset in the middle of a UTF-8 sequence, or a
/// code-unit offset between the halves of a surrogate pair.  A range
/// that covers part of a character covers all of it, so a lexer error
/// on one byte of a character marks the character.  A byte that begins
/// no UTF-8 character is one code unit.
class Utf16Index {
public:
  explicit Utf16Index(std::string_view text);
  [[nodiscard]] auto to_utf16(uint32_t byte_offset) const -> uint32_t;
  [[nodiscard]] auto to_bytes(uint32_t utf16_offset) const -> uint32_t;
  /// The code-unit range of a byte range, held inside the text.
  [[nodiscard]] auto to_utf16(Span bytes) const -> Span;
  /// The one-based column in code units of the position at
  /// `byte_offset`, whose one-based column in bytes is `byte_column`.
  [[nodiscard]] auto column(uint32_t byte_offset, uint32_t byte_column) const -> uint32_t;

private:
  /// One character whose byte count and code-unit count differ: where it
  /// ends in each unit and how wide it is.  Between two such characters
  /// the units advance together.
  struct WideCharacter {
    uint32_t bytes_end;
    uint32_t units_end;
    uint32_t byte_width;
    uint32_t unit_width;
  };
  /// The code-unit offsets of the character boundaries on either side
  /// of a byte offset; equal when the offset is itself a boundary.
  struct Boundaries {
    uint32_t before;
    uint32_t after;
  };
  [[nodiscard]] auto boundaries(uint32_t byte_offset) const -> Boundaries;

  std::vector<WideCharacter> wide_;
  uint32_t byte_size_ = 0;
};

// ---------------------------------------------------------------------------
// Program assembly and diagnostic serialization shared by the endpoints.
//
// The playground compiles the stdlib prelude group plus the files a
// request carries as one Program (frontend/module).  One of those files
// is the document the request is about: JSON offsets in the request are
// local to it, and positions in replies name their file with file-local
// offsets, so the frontend needs no knowledge of the prelude at all.
// When the document lacks a leading `module` declaration (a fresh
// scratch buffer), a synthetic `module playground` header is prepended
// and its length is subtracted from every reported offset so positions
// still line up with the editor.
// ---------------------------------------------------------------------------

struct PlaygroundProgram {
  Program program;
  const SourceFile* user = nullptr; // the document's file
  uint32_t header_bytes = 0;        // synthetic module header length, 0 if the user wrote one
  uint32_t header_lines = 0;
  /// Every file's offsets in the editor's unit, built with the program.
  std::unordered_map<const SourceFile*, Utf16Index> utf16;

  /// Length of the document as the editor sees it: in the editor's
  /// unit, without the synthetic module header.  The header is ASCII
  /// and was prepended to this file's own text, so it cannot exceed the
  /// buffer; the guard keeps a mistake from underflowing into a
  /// four-billion-unit document that would let every offset through.
  [[nodiscard]] auto document_length() const -> uint32_t {
    auto size = utf16.at(user).to_utf16(static_cast<uint32_t>(user->buffer.size()));
    return size >= header_bytes ? size - header_bytes : 0;
  }
  /// Program offset of an editor-buffer offset, which must be at most
  /// document_length() (document_offset checks a request's).
  [[nodiscard]] auto to_program_offset(uint32_t editor_offset) const -> uint32_t {
    return user->base_offset + utf16.at(user).to_bytes(header_bytes + editor_offset);
  }
  /// The editor's offset, within `file`, of a program offset inside it.
  /// The document's offsets leave out its synthetic header.
  [[nodiscard]] auto editor_offset_in(const SourceFile& file, uint32_t program_offset) const
      -> uint32_t {
    auto units = utf16.at(&file).to_utf16(file.local_offset(program_offset));
    auto skipped = &file == user ? header_bytes : 0;
    return units >= skipped ? units - skipped : 0;
  }
  /// Editor-buffer offset of a program offset inside the user file.
  [[nodiscard]] auto to_editor_offset(uint32_t program_offset) const -> uint32_t {
    return editor_offset_in(*user, program_offset);
  }
  /// A span's length in the editor's unit.  A span that starts in no
  /// file has no position to report (add_position gives it none), so
  /// it has no length either.  One that starts in a file lies wholly in
  /// it: a span that runs past its file is an internal error
  /// (SourceMap::owner_of), not a range to shorten.
  [[nodiscard]] auto editor_length(Span span) const -> uint32_t {
    const auto* file = program.source_map.file_for(span.offset);
    if (file == nullptr) {
      return 0;
    }
    assert(program.source_map.owner_of(span) == file && "a located span runs past its file");
    return utf16.at(file)
        .to_utf16({.offset = file->local_offset(span.offset), .length = span.length})
        .length;
  }
  /// The one-based column, in the editor's unit, of a program offset
  /// inside `file`.
  [[nodiscard]] auto editor_column(const SourceFile& file, uint32_t program_offset) const
      -> uint32_t {
    return utf16.at(&file).column(file.local_offset(program_offset),
                                  program.source_map.locate(program_offset).col);
  }
  /// True if the offset lies in the editor buffer (not prelude, not gap).
  [[nodiscard]] auto in_user_file(uint32_t program_offset) const -> bool {
    return program.source_map.file_for(program_offset) == user;
  }
  /// True if the offset lies in any file the request sent — the document
  /// or another of its files — rather than in the prelude.  The IR views
  /// hide the prelude, not the rest of the program: a document that
  /// calls into a sibling file must not be shown a call with no
  /// definition.
  [[nodiscard]] auto in_request_files(uint32_t program_offset) const -> bool {
    const auto* file = program.source_map.file_for(program_offset);
    return file != nullptr && !file->is_prelude;
  }
  /// True if the offset lies in the editor buffer past the synthetic header.
  [[nodiscard]] auto in_editor_text(uint32_t program_offset) const -> bool {
    return in_user_file(program_offset) && user->local_offset(program_offset) >= header_bytes;
  }
  /// Editor line of a program offset inside the user file.
  [[nodiscard]] auto editor_line(uint32_t program_offset) const -> uint32_t {
    auto line = program.source_map.locate(program_offset).line;
    return line > header_lines ? line - header_lines : line;
  }
};

/// The files of a request and which of them is the document.
struct ProgramRequest {
  std::vector<SourceInput> files;
  std::string document;
};

/// The `files` and `document` of a request body, or why they are not
/// usable: the document must name one of the files.
auto parse_program_request(const nlohmann::json& request)
    -> std::expected<ProgramRequest, std::string>;

/// Assemble the prelude group under <repo_root>/stdlib plus the
/// request's files, and lex/parse everything.
auto build_playground_program(const std::filesystem::path& repo_root,
                              ProgramRequest request,
                              EntryPolicy entry_policy = EntryPolicy::Optional)
    -> PlaygroundProgram;

/// The document-local offset a position request names, or why it is
/// unusable: an integer from 0 to the document's length, inclusive.
auto document_offset(const nlohmann::json& request, const PlaygroundProgram& prog)
    -> std::expected<uint32_t, std::string>;

/// lex → parse → resolve → typecheck over the whole program: the input
/// to every navigation and completion query.  Parse errors in the
/// buffer are tolerated (it is being typed); `ok` is false only when the
/// buffer produced no tree or a prelude file failed.
struct FrontendPipeline {
  PlaygroundProgram prog;
  ResolveResult resolve_result;
  TypeCheckResult check_result;
  TypeContext types;
  bool ok = false;
};

auto run_frontend_pipeline(const std::filesystem::path& repo_root, ProgramRequest request)
    -> FrontendPipeline;

/// Start offset of the token containing `offset`, or `offset` itself
/// when no token does.
auto token_start_at(uint32_t offset, const LexResult& lex) -> uint32_t;

/// True if any diagnostic has error severity.
auto has_error_severity(const std::vector<Diagnostic>& diags) -> bool;

/// Set `file`, `offset`, `line`, `col` on a JSON object for a program
/// offset: the file's display path with file-local position, editor
/// adjusted when the file is the buffer.
void add_position(nlohmann::json& out, const PlaygroundProgram& prog, uint32_t program_offset);

/// Diagnostics minus prelude-origin warnings, which concern the stdlib
/// maintainers rather than the person editing the buffer.
auto without_prelude_warnings(const std::vector<Diagnostic>& diags, const PlaygroundProgram& prog)
    -> std::vector<Diagnostic>;

/// Append diagnostics to a JSON array with file identity and file-local
/// positions.
void collect_diagnostics(nlohmann::json& out,
                         const PlaygroundProgram& prog,
                         const std::vector<Diagnostic>& diags);

/// Append everything assembling the program had to say — graph, lex,
/// and parse — in the one §8.4 order (`assembly_diagnostics`): a module
/// declaration that disagrees with its path points at a file, while an
/// import cycle or a missing entry module has nowhere to point and is
/// reported without a position.  Called before any early return: a
/// graph error must not hide the parse error in another file that
/// explains it.  Some of these are warnings analysis continues past.
void collect_program_diagnostics(nlohmann::json& out, const PlaygroundProgram& prog);

/// A diagnostic entry with no location, for a phase that failed without
/// reporting where and for program-assembly diagnostics that have
/// nowhere to point.  The severity is the reported one: an advisory is
/// serialized as a warning, not silently promoted to an error.
/// Put a reply's diagnostics in program order: by the position of the
/// file they lie in, then by offset within it.  The program's file
/// order is the prelude group first, then display path (Task 31 §8.4;
/// CONTRACT_MODULE_SYSTEM.md §9 makes the result independent of the
/// order files were given), which the file's name alone does not give.  Entries with no location
/// sort first, where a reader meets them before any file's own complaints.  The service collects
/// diagnostics phase by phase, which would otherwise let a later
/// phase's diagnostic in an earlier file follow an earlier phase's in
/// a later one.
void sort_diagnostics(nlohmann::json& diagnostics, const PlaygroundProgram& prog);

auto make_unlocated_diagnostic(const std::string& message, Severity severity = Severity::Error)
    -> nlohmann::json;

} // namespace dao::playground

#endif // DAO_PLAYGROUND_PIPELINE_H
