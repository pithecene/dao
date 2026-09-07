#ifndef DAO_PLAYGROUND_SERVICE_H
#define DAO_PLAYGROUND_SERVICE_H

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <string_view>

// ---------------------------------------------------------------------------
// Transport-free view of the compiler service.
//
// Every route is a function from a JSON request to a Reply; the HTTP
// server in main.cpp is a thin adapter over `dispatch`, and the service
// tests call `dispatch` directly.  Route names, request shapes, and
// response shapes come from compiler/analysis/tooling_surface.h.
// ---------------------------------------------------------------------------

namespace dao::playground {

struct ServiceContext {
  std::filesystem::path repo_root;
  std::filesystem::path examples_dir;
};

namespace http_status {
inline constexpr int ok = 200;
inline constexpr int bad_request = 400;
inline constexpr int not_found = 404;
} // namespace http_status

struct Reply {
  int status = http_status::ok;
  nlohmann::json body;
};

auto error_reply(int status, std::string message) -> Reply;

/// Check a request document against a request shape from the tooling
/// surface: required fields present, every field of its declared JSON
/// type.  Returns the first problem found, or an empty string.
auto validate_request(const nlohmann::json& request, std::string_view shape_name)
    -> std::string;

/// Serve one route by its tooling-surface name.  Unknown routes reply
/// 404; requests that fail validation reply 400.
auto dispatch(std::string_view route_name, const nlohmann::json& request,
              const ServiceContext& ctx) -> Reply;

} // namespace dao::playground

#endif // DAO_PLAYGROUND_SERVICE_H
