#ifndef DAO_PLAYGROUND_PIPELINE_H
#define DAO_PLAYGROUND_PIPELINE_H

#include "frontend/diagnostics/diagnostic.h"
#include "frontend/module/program.h"
#include "frontend/module/source_map.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace dao::playground {

// ---------------------------------------------------------------------------
// Program assembly and diagnostic serialization shared by the endpoints.
//
// The playground compiles the stdlib prelude group plus the editor buffer
// as one Program (frontend/module).  The editor buffer is the program's
// last file; JSON offsets are buffer-local, i.e. program offsets minus
// that file's base offset, so the frontend needs no knowledge of the
// prelude at all.  When the buffer lacks a leading `module` declaration
// (a fresh scratch buffer), a synthetic `module playground` header is
// prepended and its length is subtracted from every reported offset so
// positions still line up with the editor.
// ---------------------------------------------------------------------------

struct PlaygroundProgram {
  Program program;
  const SourceFile* user = nullptr; // the editor buffer's file
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
  /// Editor line of a program offset inside the user file.
  [[nodiscard]] auto editor_line(uint32_t program_offset) const -> uint32_t {
    auto line = program.source_map.locate(program_offset).line;
    return line > header_lines ? line - header_lines : line;
  }
};

/// Assemble the prelude group under <repo_root>/stdlib plus the editor
/// buffer, and lex/parse everything.
auto build_playground_program(const std::filesystem::path& repo_root,
                              std::string user_source) -> PlaygroundProgram;

/// True if any diagnostic originates in the editor buffer.
auto has_user_error(const std::vector<Diagnostic>& diags, const PlaygroundProgram& prog) -> bool;

/// Append editor-buffer diagnostics to a JSON array with buffer-local
/// offsets and lines.  Prelude-origin diagnostics are skipped.
void collect_diagnostics(nlohmann::json& out, const PlaygroundProgram& prog,
                         const std::vector<Diagnostic>& diags);

/// Build a synthetic error diagnostic entry for when a phase fails
/// with no user-visible diagnostics (possible prelude error).
auto make_internal_error(const std::string& message) -> nlohmann::json;

} // namespace dao::playground

#endif // DAO_PLAYGROUND_PIPELINE_H
