#include "service.h"

#include "analyze.h"
#include "completions.h"
#include "examples.h"
#include "navigation.h"
#include "run.h"

#include "service_surface.h"

#include <algorithm>
#include <array>
#include <format>

namespace dao::playground {

namespace {

using RouteFn = auto (*)(const nlohmann::json&, const ServiceContext&) -> Reply;

struct RouteBinding {
  std::string_view name; // a kRoutes name (service_surface.h)
  RouteFn handler;
};

// One binding per route in kRoutes; the service test asserts
// the two lists agree.
constexpr std::array kBindings{
    RouteBinding{"analyze", analyze},
    RouteBinding{"run", run},
    RouteBinding{"hover", hover},
    RouteBinding{"gotoDef", goto_definition},
    RouteBinding{"documentSymbols", document_symbols},
    RouteBinding{"references", references},
    RouteBinding{"completions", completions},
    RouteBinding{"examples", examples_list},
    RouteBinding{"example", example_source},
};

/// Why `value` does not satisfy `type` (a scalar, a shape, or `Shape[]`),
/// else empty.  `path` names the value in the message.
auto validate_value(const nlohmann::json& value, std::string_view type, const std::string& path)
    -> std::string {
  if (type == "string" || type == "number" || type == "boolean") {
    const bool ok = (type == "string" && value.is_string()) ||
                    (type == "number" && value.is_number()) ||
                    (type == "boolean" && value.is_boolean());
    return ok ? "" : std::format("'{}' must be a {}", path, type);
  }
  if (type.ends_with("[]")) {
    if (!value.is_array()) {
      return std::format("'{}' must be an array", path);
    }
    auto element_type = type.substr(0, type.size() - 2);
    for (size_t i = 0; i < value.size(); ++i) {
      if (auto why = validate_value(value[i], element_type, std::format("{}[{}]", path, i));
          !why.empty()) {
        return why;
      }
    }
    return "";
  }
  const auto* shape = find_service_shape(type);
  if (shape == nullptr) {
    return std::format("'{}' has unknown type {}", path, type);
  }
  if (!value.is_object()) {
    return std::format("'{}' must be an object", path);
  }
  for (const auto& field : shape->fields) {
    auto it = value.find(std::string(field.name));
    auto field_path =
        path.empty() ? std::string(field.name) : std::format("{}.{}", path, field.name);
    if (it == value.end()) {
      if (field.optional) {
        continue;
      }
      return std::format("missing '{}' field", field_path);
    }
    if (auto why = validate_value(*it, field.type, field_path); !why.empty()) {
      return why;
    }
  }
  return "";
}

} // namespace

auto error_reply(int status, std::string message) -> Reply {
  return {.status = status, .body = {{"error", std::move(message)}}};
}

auto validate_request(const nlohmann::json& request, std::string_view shape_name) -> std::string {
  if (shape_name == "void") {
    return {};
  }
  if (find_service_shape(shape_name) == nullptr) {
    return std::format("unknown request shape '{}'", shape_name);
  }
  if (!request.is_object()) {
    return "request body must be a JSON object";
  }
  return validate_value(request, shape_name, "");
}

auto dispatch(std::string_view route_name, const nlohmann::json& request, const ServiceContext& ctx)
    -> Reply {
  const auto* route = find_route(route_name);
  auto binding = std::ranges::find(kBindings, route_name, &RouteBinding::name);
  if (route == nullptr || binding == kBindings.end()) {
    return error_reply(http_status::not_found, std::format("unknown route '{}'", route_name));
  }
  if (auto problem = validate_request(request, route->request); !problem.empty()) {
    return error_reply(http_status::bad_request, problem);
  }
  return binding->handler(request, ctx);
}

} // namespace dao::playground
