#pragma once

#include <drogon/Cookie.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>

#include <cstdint>
#include <expected>
#include <string>

// Серверные сессии (тег Auth в api/openapi.yaml): в cookie dv_session случайный токен, в
// таблице sessions - его SHA-256. Срок 24 ч с момента входа, без продления (#17094 п. 1.3).

// Пользователь текущей сессии.
struct SessionUser
{
    std::int64_t userId;
    std::string username;
    bool hasPassword;
    std::string expiresAt;  // ISO 8601 в UTC, как SessionInfo.expires_at
};

// Создаёт сессию и возвращает ответ SessionCreated: тело SessionInfo и cookie dv_session.
// Сессия из cookie запроса, если была, удаляется. db может быть транзакцией: тогда сессия
// появится вместе с остальными изменениями (например, подтверждением email).
drogon::Task<drogon::HttpResponsePtr> startSession(drogon::orm::DbClientPtr db,
                                                   drogon::HttpRequestPtr req, std::int64_t userId,
                                                   drogon::HttpStatusCode status = drogon::k200OK);

// Проверка сессии для операций с sessionCookie: пользователь или готовый ответ 401
// SESSION_REQUIRED / SESSION_EXPIRED. Отказ пишется в журнал как access_denied (#17094 п. 5.1).
//   auto user = co_await requireSession(db, req);
//   if (!user) co_return user.error();
drogon::Task<std::expected<SessionUser, drogon::HttpResponsePtr>> requireSession(
    drogon::orm::DbClientPtr db, drogon::HttpRequestPtr req);

// GET /auth/session: SPA узнаёт, гость пользователь или нет (cookie HttpOnly, JS её не видит).
drogon::Task<drogon::HttpResponsePtr> getSessionHandler(drogon::orm::DbClientPtr db,
                                                        drogon::HttpRequestPtr req);

// Cookie dv_session: HttpOnly; Secure; SameSite=Lax; Path=/api/v1; Max-Age=86400.
drogon::Cookie sessionCookie(const std::string &token);
// Стирает dv_session у браузера: Max-Age=0.
drogon::Cookie clearSessionCookie();
