#pragma once

#include "config.hpp"

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>

#include <cstdint>
#include <string>

// Вход и регистрация через OIDC-провайдера (#17148 п. 3, #17149 п. 3), тег OIDC в api/openapi.yaml.
// OAuth2 authorization code flow со state и PKCE: браузер уходит к провайдеру и возвращается с
// кодом, обмен кода на токен и запрос профиля делает backend. Незавершённый вход живёт в
// oidc_pending, к браузеру его привязывает cookie dv_oidc (30 минут).
// Подключены Яндекс и Google; новый провайдер - строка в таблице kProviders (oidc.cpp).

struct OidcSettings
{
    std::string appUrl;  // страница SPA {appUrl}/auth/oidc - исход входа
    std::string apiUrl;  // redirect_uri: {apiUrl}/api/v1/auth/oidc/{provider}/callback
    OidcClient yandex;
    OidcClient google;
};

// Подключённые провайдеры для лога при запуске: «Яндекс, Google». Пусто - ни одного.
std::string enabledOidcProviders(const OidcSettings &settings);

// GET /auth/oidc/{provider}/start: запись в oidc_pending, cookie dv_oidc и переход к провайдеру.
drogon::Task<drogon::HttpResponsePtr> oidcStartHandler(drogon::orm::DbClientPtr db,
                                                       OidcSettings settings,
                                                       drogon::HttpRequestPtr req,
                                                       std::string provider);

// GET /auth/oidc/{provider}/callback: возврат от провайдера. Всегда 302 на страницу SPA с
// результатом: success, registration_required, link_required, account_blocked или error.
drogon::Task<drogon::HttpResponsePtr> oidcCallbackHandler(drogon::orm::DbClientPtr db,
                                                          OidcSettings settings,
                                                          drogon::HttpRequestPtr req,
                                                          std::string provider);

// GET /auth/oidc/pending: данные для страницы «Завершение регистрации» или формы привязки.
drogon::Task<drogon::HttpResponsePtr> oidcPendingHandler(drogon::orm::DbClientPtr db,
                                                         drogon::HttpRequestPtr req);

// POST /auth/oidc/complete: учётная запись «Активна», провайдер привязан, сессия.
drogon::Task<drogon::HttpResponsePtr> oidcCompleteHandler(drogon::orm::DbClientPtr db,
                                                          drogon::HttpRequestPtr req);

// После входа с паролем (POST /auth/login): если есть dv_oidc с исходом link_required и вход
// выполнен в ту самую учётную запись, провайдер привязывается к ней (#17148 п. 3.2 шаг 7). Вход в
// другую учётную запись привязку отменяет. dv_oidc в ответе стирается.
drogon::Task<> finishOidcLink(drogon::orm::DbClientPtr db, drogon::HttpRequestPtr req,
                              std::int64_t userId, drogon::HttpResponsePtr response);
