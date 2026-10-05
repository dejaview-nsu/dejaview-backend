#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>

#include <string>

// Регистрация по имени пользователя, email и паролю (#17148 п. 2). Ответы и тексты - тег Auth
// в api/openapi.yaml. appUrl - адрес frontend для ссылки в письме (Config::appUrl).

// POST /auth/register: учётная запись «Не подтверждена» и письмо со ссылкой в очереди
// email_outbox. Сессия не создаётся: вход - после подтверждения email.
drogon::Task<drogon::HttpResponsePtr> registerHandler(drogon::orm::DbClientPtr db,
                                                      std::string appUrl,
                                                      drogon::HttpRequestPtr req);

// POST /auth/confirm-email: по токену из ссылки статус «Активна» и новая сессия (#17148 п. 2.5).
drogon::Task<drogon::HttpResponsePtr> confirmEmailHandler(drogon::orm::DbClientPtr db,
                                                          drogon::HttpRequestPtr req);

// POST /auth/resend-confirmation: новая ссылка взамен старой, не чаще раза в 60 с.
drogon::Task<drogon::HttpResponsePtr> resendConfirmationHandler(drogon::orm::DbClientPtr db,
                                                                std::string appUrl,
                                                                drogon::HttpRequestPtr req);
