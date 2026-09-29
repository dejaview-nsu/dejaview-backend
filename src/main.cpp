#include "config.hpp"

#include <drogon/drogon.h>

#include <exception>
#include <functional>

using namespace drogon;

using Callback = std::function<void(const HttpResponsePtr &)>;

void healthHandler(const HttpRequestPtr &request, Callback &&callback) {
  Json::Value jsonBody;
  jsonBody["status"] = "ok";
  auto response = HttpResponse::newHttpJsonResponse(jsonBody);

  callback(response);
}

int main() {
  Config config;
  try {
    config = loadConfig();
  } catch (const std::exception &e) {
    LOG_ERROR << "Ошибка конфигурации: " << e.what();
    return 1;
  }

  LOG_INFO << "dejaview-backend слушает порт " << config.port;

  app()
      .addListener("0.0.0.0", config.port)
      .setThreadNum(0)
      .enableServerHeader(false)
      .setUploadPath("/tmp/dejaview-uploads")
      .registerHandler("/health", &healthHandler, {Get})
      .run();
}
