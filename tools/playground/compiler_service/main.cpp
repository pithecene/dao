// HTTP adapter over the compiler service: every route in the tooling
// surface is registered here and forwarded to `dispatch`.

#include "run.h"
#include "service.h"

#include "analysis/tooling_surface.h"

#include <httplib.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {

auto find_repo_root() -> std::filesystem::path {
  // DAO_SOURCE_DIR is baked in at compile time.
  return DAO_SOURCE_DIR;
}

void send(const dao::playground::Reply& reply, httplib::Response& res) {
  res.status = reply.status;
  res.set_content(reply.body.dump(), "application/json");
}

void register_routes(httplib::Server& svr, const dao::playground::ServiceContext& ctx) {
  using dao::playground::dispatch;
  using dao::playground::error_reply;

  // NOLINTBEGIN(modernize-use-trailing-return-type)
  for (const auto& route : dao::tooling::kRoutes) {
    std::string name(route.name);
    std::string path(route.path);
    if (route.method == "POST") {
      svr.Post(path, [name, &ctx](const httplib::Request& req, httplib::Response& res) {
        nlohmann::json body;
        try {
          body = nlohmann::json::parse(req.body);
        } catch (const nlohmann::json::parse_error&) {
          send(error_reply(dao::playground::http_status::bad_request, "invalid JSON"), res);
          return;
        }
        send(dispatch(name, body, ctx), res);
      });
    } else {
      svr.Get(path, [name, &ctx](const httplib::Request& req, httplib::Response& res) {
        auto params = nlohmann::json::object();
        for (const auto& [key, value] : req.path_params) {
          params[key] = value;
        }
        send(dispatch(name, params, ctx), res);
      });
    }
  }
  // NOLINTEND(modernize-use-trailing-return-type)
}

} // namespace

auto main(int argc, char* argv[]) -> int {
  int port = 8090; // NOLINT(readability-magic-numbers)
  auto root = find_repo_root();

  // Simple arg parsing: --port N and --root DIR
  for (int i = 1; i < argc; ++i) {
    std::string_view arg(argv[i]);
    if (arg == "--port" && i + 1 < argc) {
      port = std::atoi(argv[++i]); // NOLINT(cert-err34-c)
    } else if (arg == "--root" && i + 1 < argc) {
      root = argv[++i];
    }
  }

  dao::playground::ServiceContext ctx{.repo_root = root, .examples_dir = root / "examples"};
  auto frontend_dir = root / "tools" / "playground" / "frontend" / "dist";

  bool serve_frontend = std::filesystem::exists(frontend_dir);
  if (!serve_frontend) {
    std::cerr << "warning: frontend dist not found: " << frontend_dir << "\n";
    std::cerr << "  API endpoints will work; use Vite dev server for the UI.\n";
  }

  // Initialize LLVM targets once for /api/run.
  dao::playground::init_run_support();

  httplib::Server svr;
  register_routes(svr, ctx);

  // Serve frontend static files when dist/ exists (prod mode).
  if (serve_frontend) {
    auto index_path = frontend_dir / "index.html";
    svr.set_mount_point("/", frontend_dir.string());

    // Explicit root handler to prevent any redirect behavior.
    // NOLINTNEXTLINE(modernize-use-trailing-return-type)
    svr.Get("/", [index_path](const httplib::Request& /*req*/, httplib::Response& res) {
      std::ifstream file(index_path);
      if (!file) {
        res.status = 500; // NOLINT(readability-magic-numbers)
        res.set_content("index.html not found", "text/plain");
        return;
      }
      std::string body{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
      res.set_content(body, "text/html");
    });
  }

  std::cout << "Dao playground: http://localhost:" << port << "\n";
  std::cout << "  frontend: " << (serve_frontend ? frontend_dir.string() : "(dev mode — use Vite)")
            << "\n";
  std::cout << "  examples: " << ctx.examples_dir << "\n";

  if (!svr.listen("127.0.0.1", port)) {
    std::cerr << "error: failed to start server on port " << port << "\n";
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
