#include "format.h"

#include "analysis/formatter.h"
#include "pipeline.h"
#include "support/module_utils.h"

#include <algorithm>
#include <string>
#include <string_view>

namespace dao::playground {

namespace {

constexpr std::string_view kSyntheticHeader = "module playground\n";
constexpr uint32_t kSyntheticHeaderBytes = static_cast<uint32_t>(kSyntheticHeader.size());
constexpr uint32_t kSyntheticHeaderLines = 1;

auto is_blank_scratch(std::string_view text) -> bool {
  return std::ranges::all_of(text,
                             [](char chr) { return chr == ' ' || chr == '\n' || chr == '\r'; });
}

auto strip_synthetic_module(std::string formatted) -> std::string {
  constexpr std::string_view kHeaderWithBlank = "module playground\n\n";
  if (formatted.starts_with(kHeaderWithBlank)) {
    formatted.erase(0, kHeaderWithBlank.size());
    return formatted;
  }
  if (formatted.starts_with(kSyntheticHeader)) {
    formatted.erase(0, kSyntheticHeader.size());
  }
  return formatted;
}

} // namespace

auto format_document(const nlohmann::json& request, const ServiceContext& /*ctx*/) -> Reply {
  auto inputs = parse_program_request(request);
  if (!inputs) {
    return error_reply(http_status::bad_request, inputs.error());
  }

  const SourceInput* doc_input = nullptr;
  for (const auto& file : inputs->files) {
    if (file.display_path == inputs->document) {
      doc_input = &file;
      break;
    }
  }
  if (doc_input == nullptr) {
    return error_reply(http_status::bad_request, "document not found in request files");
  }

  const std::string& original = doc_input->text;
  if (is_blank_scratch(original)) {
    return {.status = http_status::ok,
            .body = {
                {"file", inputs->document},
                {"formatted", ""},
                {"changed", !original.empty()},
                {"diagnostics", nlohmann::json::array()},
            }};
  }

  const bool injected_module = !starts_with_module(original);
  std::string buffer_text = injected_module ? std::string(kSyntheticHeader) + original : original;
  SourceBuffer buffer(inputs->document, std::move(buffer_text));
  auto result = format_source(buffer);

  if (!result.ok()) {
    nlohmann::json diagnostics = nlohmann::json::array();
    for (const auto& diag : result.errors) {
      auto loc = buffer.line_col(diag.span.offset);
      uint32_t offset = diag.span.offset;
      uint32_t line = loc.line;
      if (injected_module) {
        offset = offset >= kSyntheticHeaderBytes ? offset - kSyntheticHeaderBytes : 0;
        line = line > kSyntheticHeaderLines ? line - kSyntheticHeaderLines : 1;
      }
      diagnostics.push_back({
          {"severity", diag.severity == Severity::Warning ? "warning" : "error"},
          {"file", inputs->document},
          {"offset", offset},
          {"length", diag.span.length},
          {"line", line},
          {"col", loc.col},
          {"message", diag.message},
      });
    }
    return {.status = http_status::ok,
            .body = {
                {"file", inputs->document},
                {"formatted", original},
                {"changed", false},
                {"diagnostics", std::move(diagnostics)},
            }};
  }

  std::string formatted =
      injected_module ? strip_synthetic_module(std::move(result.output)) : std::move(result.output);
  const bool changed = formatted != original;
  return {.status = http_status::ok,
          .body = {
              {"file", inputs->document},
              {"formatted", std::move(formatted)},
              {"changed", changed},
              {"diagnostics", nlohmann::json::array()},
          }};
}

} // namespace dao::playground
