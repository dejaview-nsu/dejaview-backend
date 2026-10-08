#pragma once

#include "search/ml_client.hpp"
#include "search/rules.hpp"

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>

#include <cstddef>
#include <memory>

// Лимит тела запроса Drogon: файл до 50 МБ (видео) плюс служебные части multipart. Умолчание
// Drogon - 1 МБ - отклонило бы любую настоящую загрузку до обработчика (main.cpp).
constexpr std::size_t kMaxSearchBodyBytes = 60 * kMiB;

// POST /search/image и /search/video: сессия -> проверки файла -> ML -> карточка фильма.
// Параметры по значению: корутина живёт дольше вызова.
drogon::Task<drogon::HttpResponsePtr> mediaSearchHandler(drogon::orm::DbClientPtr db,
                                                         std::shared_ptr<MlService> ml,
                                                         MediaKind kind,
                                                         drogon::HttpRequestPtr req);
