#include "health.hpp"

#include <drogon/drogon.h>

using namespace drogon;

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
