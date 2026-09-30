#pragma once

#include <drogon/HttpResponse.h>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>

// GET /health: 200, если БД отвечает, иначе 503. Формат как у health check ML-сервиса
// (dejaview-docs, contracts/backend-ml.md, раздел 6).
drogon::Task<drogon::HttpResponsePtr> healthHandler(drogon::orm::DbClientPtr db);
