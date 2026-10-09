#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>

#include <string>

// Восстановление и смена пароля (#17150), тег Password в api/openapi.yaml. Ссылка из письма ведёт
// на страницу SPA {appUrl}/reset-password?token=...: живёт 1 ч, действует только последняя.

// POST /auth/password-reset/request: всегда 202 без тела, есть учётная запись или нет. Если есть
// и не заблокирована - новая ссылка и письмо в очереди, но не чаще раза в 60 с.
drogon::Task<drogon::HttpResponsePtr> requestPasswordResetHandler(drogon::orm::DbClientPtr db,
                                                                  std::string appUrl,
                                                                  drogon::HttpRequestPtr req);

// POST /auth/password-reset/check: 204 - ссылка действует, 410 - истекла, заменена новой или уже
// использована. Ссылка не расходуется.
drogon::Task<drogon::HttpResponsePtr> checkPasswordResetHandler(drogon::orm::DbClientPtr db,
                                                                drogon::HttpRequestPtr req);

// POST /auth/password-reset/complete (#17150 п. 2): новый пароль по ссылке. Успех - все сессии
// пользователя завершены, создана новая (SessionCreated), в очереди письмо о смене пароля.
drogon::Task<drogon::HttpResponsePtr> completePasswordResetHandler(drogon::orm::DbClientPtr db,
                                                                   std::string appUrl,
                                                                   drogon::HttpRequestPtr req);

// PUT /users/me/password (#17150 п. 3): смена пароля с проверкой текущего, а у учётной записи без
// пароля (OIDC) - первый пароль. Успех - 204, сессии, кроме текущей, завершены, письмо в очереди.
drogon::Task<drogon::HttpResponsePtr> changePasswordHandler(drogon::orm::DbClientPtr db,
                                                            std::string appUrl,
                                                            drogon::HttpRequestPtr req);
