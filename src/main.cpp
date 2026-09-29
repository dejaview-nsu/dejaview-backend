#include "config.hpp"

#include <drogon/drogon.h>

#include <exception>

using namespace drogon;

// 200, если БД отвечает, иначе 503 - как GET /health у ML-сервиса.
// db принимаем по значению: корутина может продолжиться после co_await, когда ссылка уже
// недействительна.
Task<HttpResponsePtr> healthHandler(orm::DbClientPtr db)
{
    bool dbOk = true;
    try
    {
        co_await db->execSqlCoro("SELECT 1");
    }
    catch (const orm::DrogonDbException &e)
    {
        LOG_WARN << "health: БД недоступна: " << e.base().what();
        dbOk = false;
    }

    Json::Value body;
    body["status"] = dbOk ? "ok" : "unavailable";
    body["database"] = dbOk ? "ok" : "unavailable";
    auto response = HttpResponse::newHttpJsonResponse(body);
    response->setStatusCode(dbOk ? k200OK : k503ServiceUnavailable);
    co_return response;
}

int main()
{
    Config config;
    try
    {
        config = loadConfig();
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "Ошибка конфигурации: " << e.what();
        return 1;
    }

    // Один клиент с пулом соединений на всё приложение. Если БД недоступна, запросы падают с
    // ошибкой, а клиент сам переподключается.
    auto db = orm::DbClient::newPgClient(config.databaseUrl, 4);
    db->setTimeout(2.0);

    LOG_INFO << "dejaview-backend слушает порт " << config.port;

    app()
        .addListener("0.0.0.0", config.port)
        .setThreadNum(0)
        .enableServerHeader(false)
        .setUploadPath("/tmp/dejaview-uploads")
        .registerHandler("/health", [db](HttpRequestPtr) { return healthHandler(db); }, {Get})
        .run();
}
