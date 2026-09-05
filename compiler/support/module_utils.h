#ifndef DAO_SUPPORT_MODULE_UTILS_H
#define DAO_SUPPORT_MODULE_UTILS_H

#include <string_view>

namespace dao {

/// Detect whether source begins with a `module` declaration (after
/// optional whitespace and `//` comments). Used by test helpers and the
/// playground to decide whether to prepend a synthetic module header.
inline auto starts_with_module(std::string_view src) -> bool {
  constexpr std::string_view kModuleKeyword = "module";

  size_t pos = 0;
  while (pos < src.size()) {
    char current = src[pos];
    if (current == ' ' || current == '\t' || current == '\n' || current == '\r') {
      ++pos;
      continue;
    }
    if (current == '/' && pos + 1 < src.size() && src[pos + 1] == '/') {
      auto line_end = src.find('\n', pos);
      if (line_end == std::string_view::npos) {
        return false;
      }
      pos = line_end + 1;
      continue;
    }
    break;
  }
  if (pos + kModuleKeyword.size() >= src.size() ||
      src.substr(pos, kModuleKeyword.size()) != kModuleKeyword) {
    return false;
  }
  char after = src[pos + kModuleKeyword.size()];
  return after == ' ' || after == '\t';
}

} // namespace dao

#endif // DAO_SUPPORT_MODULE_UTILS_H
