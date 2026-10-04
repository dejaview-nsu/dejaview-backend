#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>

#include <string>

// POST /auth/login: вход по имени пользователя или email и паролю (#17149 п. 2) с защитой от
// подбора: CAPTCHA с 3-й неудачной попытки, блокировка на 15 минут после 5-й (п. 2.5).
// Порядок проверок и ответы - описание операции в api/openapi.yaml.
// captchaKey - серверный ключ SmartCaptcha; пустой - CAPTCHA отключена (локальная разработка).
drogon::Task<drogon::HttpResponsePtr> loginHandler(drogon::orm::DbClientPtr db,
                                                   std::string captchaKey,
                                                   drogon::HttpRequestPtr req);
