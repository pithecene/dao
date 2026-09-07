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

auto json_type_matches(const nlohmann::json& value, std::string_view type) -> bool {
  if (type == "string") {
    return value.is_string();
  }
  if (type == "number") {
    return value.is_number();
  }
  if (type == "boolean") {
    return value.is_boolean();
  }
  if (type.ends_with("[]")) {
    return value.is_array();
  }
  return value.is_object();
}

} // namespace

auto error_reply(int status, std::string message) -> Reply {
  return {.status = status, .body = {{"error", std::move(message)}}};
}

auto validate_request(const nlohmann::json& request, std::string_view shape_name) -> std::string {
  if (shape_name == "void") {
    return {};
  }
  const auto* shape = find_service_shape(shape_name);
  if (shape == nullptr) {
    return std::format("unknown request shape '{}'", shape_name);
  }
  if (!request.is_object()) {
    return "request body must be a JSON object";
  }
  for (const auto& field : shape->fields) {
    auto it = request.find(std::string(field.name));
    if (it == request.end()) {
      if (field.optional) {
        continue;
      }
      return std::format("missing '{}' field", field.name);
    }
    if (!json_type_matches(*it, field.type)) {
      return std::format("'{}' must be a {}", field.name, field.type);
    }
  }
  return {};
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
