#pragma once

#include <drogon/HttpResponse.h>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>

// GET /health: 200, если БД отвечает, иначе 503 - как GET /health.
drogon::Task<drogon::HttpResponsePtr> healthHandler(drogon::orm::DbClientPtr db);
