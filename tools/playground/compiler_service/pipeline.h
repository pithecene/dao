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

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace dao::playground {

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

  /// Program offset of an editor-buffer offset.
  [[nodiscard]] auto to_program_offset(uint32_t editor_offset) const -> uint32_t {
    return user->base_offset + header_bytes + editor_offset;
  }
  /// Editor-buffer offset of a program offset inside the user file.
  [[nodiscard]] auto to_editor_offset(uint32_t program_offset) const -> uint32_t {
    auto local = user->local_offset(program_offset);
    return local >= header_bytes ? local - header_bytes : 0;
  }
  /// True if the offset lies in the editor buffer (not prelude, not gap).
  [[nodiscard]] auto in_user_file(uint32_t program_offset) const -> bool {
    return program.source_map.file_for(program_offset) == user;
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
auto build_playground_program(const std::filesystem::path& repo_root, ProgramRequest request)
    -> PlaygroundProgram;

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

/// Build a synthetic error diagnostic entry (no location) for when a
/// phase fails without reporting where.
auto make_internal_error(const std::string& message) -> nlohmann::json;

} // namespace dao::playground

#endif // DAO_PLAYGROUND_PIPELINE_H
