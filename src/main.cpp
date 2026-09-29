#include "config.hpp"

#include <drogon/drogon.h>

#include <exception>
#include <functional>

using namespace drogon;

int main()
{
    Config config;
    try
    {
        config = loadConfig();
    }
    catch (const std::exception& e)
    {
        LOG_ERROR << "Ошибка конфигурации: " << e.what();
        return 1;
    }

    app().registerHandler(
        "/health",
        [](const HttpRequestPtr&, std::function<void(const HttpResponsePtr&)>&& callback)
        {
            Json::Value body;
            body["status"] = "ok";
            callback(HttpResponse::newHttpJsonResponse(body));
        },
        {Get});

    LOG_INFO << "dejaview-backend слушает порт " << config.port;
    app().setUploadPath("/tmp/dejaview-uploads").addListener("0.0.0.0", config.port).run();
}
