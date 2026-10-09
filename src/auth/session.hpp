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

// Новая сессия: токен для cookie dv_session и пользователь. Сессия из cookie запроса, если была,
// удаляется: идентификатор при каждом входе новый (тег Auth).
struct NewSession
{
    std::string token;
    SessionUser user;
};
drogon::Task<NewSession> createSession(drogon::orm::DbClientPtr db, drogon::HttpRequestPtr req,
                                       std::int64_t userId);

// createSession и ответ SessionCreated: тело SessionInfo и cookie dv_session.
drogon::Task<drogon::HttpResponsePtr> startSession(drogon::orm::DbClientPtr db,
                                                   drogon::HttpRequestPtr req, std::int64_t userId,
                                                   drogon::HttpStatusCode status = drogon::k200OK);

// Проверка сессии для операций с sessionCookie: пользователь или готовый ответ 401
// SESSION_REQUIRED / SESSION_EXPIRED. Отказ пишется в журнал как access_denied (#17094 п. 5.1).
//   auto user = co_await requireSession(db, req);
//   if (!user) co_return user.error();
drogon::Task<std::expected<SessionUser, drogon::HttpResponsePtr>> requireSession(
    drogon::orm::DbClientPtr db, drogon::HttpRequestPtr req);

// Сессия определяется один раз на запрос, для всех маршрутов: main.cpp регистрирует эти функции
// как advices Drogon - до обработчика и после.
// resolveSession: если в запросе cookie dv_session - найти сессию, итог положить в атрибуты
// запроса. requireSession и GET /auth/session берут его оттуда, без второго запроса к БД.
drogon::Task<> resolveSession(drogon::orm::DbClientPtr db, drogon::HttpRequestPtr req);
// clearStaleSessionCookie: cookie недействительна, а ответ свою не выдал - стереть. Так операция
// гостя с недействительной cookie выполняется как для гостя (тег Auth).
void clearStaleSessionCookie(const drogon::HttpRequestPtr &req,
                             const drogon::HttpResponsePtr &response);

// GET /auth/session: SPA узнаёт, гость пользователь или нет (cookie HttpOnly, JS её не видит).
drogon::Task<drogon::HttpResponsePtr> getSessionHandler(drogon::orm::DbClientPtr db,
                                                        drogon::HttpRequestPtr req);

// POST /auth/logout (#17151): удаляет сессию из cookie и стирает cookie, другие сессии
// пользоват–еля остаются. Без сессии - 401: клиент считает это успешным выходом.
drogon::Task<drogon::HttpResponsePtr> logoutHandler(drogon::orm::DbClientPtr db,
                                                    drogon::HttpRequestPtr req);

// Cookie dv_session: HttpOnly; Secure; SameSite=Lax; Path=/api/v1; Max-Age=86400.
drogon::Cookie sessionCookie(const std::string &token);
// Стирает dv_session у браузера: Max-Age=0.
drogon::Cookie clearSessionCookie();
