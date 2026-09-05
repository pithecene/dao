#include "frontend/module/source_map.h"

#include <algorithm>
#include <cassert>

namespace dao {

auto position_budget_fits(std::span<const uint64_t> sizes) -> bool {
  uint64_t total = 0;
  for (auto size : sizes) {
    total += size + 1;
    if (total > kMaxProgramPositions) {
      return false;
    }
  }
  return true;
}

auto base_offset_for(std::span<const uint64_t> preceding_sizes) -> uint32_t {
  uint64_t base = 0;
  for (auto size : preceding_sizes) {
    base += size + 1;
  }
  assert(base < kMaxProgramPositions && "position budget must be checked first");
  return static_cast<uint32_t>(base);
}

void SourceMap::add(const SourceFile* file) {
  assert(file != nullptr);
  assert((files_.empty() || files_.back()->eof_offset() < file->base_offset) &&
         "files must be added in ascending, disjoint order");
  files_.push_back(file);
}

auto SourceMap::file_for(uint32_t offset) const -> const SourceFile* {
  // First file whose base is greater than offset; the candidate is the
  // one before it.  Ranges are closed, so the EOF position of a file
  // still resolves to that file and never to its successor.
  auto it = std::upper_bound(
      files_.begin(), files_.end(), offset,
      [](uint32_t off, const SourceFile* f) { return off < f->base_offset; });
  if (it == files_.begin()) {
    return nullptr;
  }
  const auto* candidate = *(it - 1);
  return candidate->contains(offset) ? candidate : nullptr;
}

auto SourceMap::locate(uint32_t offset) const -> SourceLocation {
  const auto* file = file_for(offset);
  assert(file != nullptr && "offset does not belong to any file");
  if (file == nullptr) {
    return {};
  }
  auto loc = file->buffer.line_col(file->local_offset(offset));
  return {.file = file, .line = loc.line, .col = loc.col};
}

auto SourceMap::owner_of(Span span) const -> const SourceFile* {
  const auto* file = file_for(span.offset);
  if (file == nullptr || !file->contains_span(span)) {
    return nullptr;
  }
  return file;
}

auto SourceMap::text(Span span) const -> std::string_view {
  const auto* file = owner_of(span);
  // A span that starts in no file, or runs past its file's EOF, is a
  // broken program-wide span; returning truncated or empty text would
  // hide it behind plausible output.
  assert(file != nullptr && "span is not wholly inside one file");
  return file->buffer.text({.offset = file->local_offset(span.offset), .length = span.length});
}

auto SourceMap::is_prelude(uint32_t offset) const -> bool {
  const auto* file = file_for(offset);
  return file != nullptr && file->is_prelude;
}

} // namespace dao
