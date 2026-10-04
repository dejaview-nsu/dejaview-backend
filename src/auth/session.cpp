#include "auth/session.hpp"

#include "auth/crypto.hpp"
#include "auth/security_log.hpp"
#include "error_response.hpp"

#include <drogon/drogon.h>

using namespace drogon;

namespace
{
constexpr int kSessionSeconds = 24 * 60 * 60;  // #17094 п. 1.3

// Время окончания в формате SessionInfo.expires_at: '2026-09-28T12:00:00Z'.
constexpr const char *kExpiresAtSql =
    "to_char(s.expires_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') AS expires_at";

SessionUser toSessionUser(const orm::Row &row)
{
    return {.userId = row["user_id"].as<std::int64_t>(),
            .username = row["username"].as<std::string>(),
            .hasPassword = row["has_password"].as<bool>(),
            .expiresAt = row["expires_at"].as<std::string>()};
}

Json::Value sessionInfo(const SessionUser &user)
{
    Json::Value body;
    body["user"]["username"] = user.username;
    body["user"]["avatar_url"] = Json::Value();  // null до работ по профилю (#17030)
    body["user"]["has_password"] = user.hasPassword;
    body["expires_at"] = user.expiresAt;
    return body;
}

// Сессия по cookie без побочных эффектов: пользователь или ответ 401, журнал не пишется.
Task<std::expected<SessionUser, HttpResponsePtr>> checkSession(orm::DbClientPtr db,
                                                               HttpRequestPtr req)
{
    const std::string &token = req->getCookie("dv_session");
    if (token.empty())
    {
        co_return std::unexpected(
            errorResponse(k401Unauthorized, "SESSION_REQUIRED", "Войдите, чтобы продолжить"));
    }

    // Заблокированный пользователь теряет доступ сразу, не дожидаясь конца сессии
    const auto result = co_await db->execSqlCoro(
        std::string("SELECT u.user_id, u.username, u.password_hash IS NOT NULL AS has_password, ") +
            kExpiresAtSql +
            " FROM sessions s JOIN users u USING (user_id)"
            " WHERE s.token_hash = decode($1, 'hex') AND s.expires_at > now()"
            " AND u.status = 'active'",
        tokenHash(token));
    if (result.empty())
    {
        auto response =
            errorResponse(k401Unauthorized, "SESSION_EXPIRED", "Сессия истекла. Войдите снова");
        response->addCookie(clearSessionCookie());
        co_return std::unexpected(response);
    }
    co_return toSessionUser(result[0]);
}
}  // namespace

Cookie sessionCookie(const std::string &token)
{
    Cookie cookie("dv_session", token);
    cookie.setHttpOnly(true);
    cookie.setSecure(true);
    cookie.setSameSite(Cookie::SameSite::kLax);
    cookie.setPath("/api/v1");
    cookie.setMaxAge(kSessionSeconds);
    return cookie;
}

Cookie clearSessionCookie()
{
    Cookie cookie("dv_session", "");
    cookie.setPath("/api/v1");
    cookie.setMaxAge(0);
    return cookie;
}

Task<NewSession> createSession(orm::DbClientPtr db, HttpRequestPtr req, std::int64_t userId)
{
    // Идентификатор каждый раз новый, прежняя сессия из cookie удаляется (тег Auth)
    if (const std::string &oldToken = req->getCookie("dv_session"); !oldToken.empty())
    {
        co_await db->execSqlCoro("DELETE FROM sessions WHERE token_hash = decode($1, 'hex')",
                                 tokenHash(oldToken));
    }

    // Срок считает БД, а не часы backend: created_at тоже из часов БД, и расхождение даже на
    // секунду нарушило бы CHECK «не больше 24 ч» (contracts/db-schema.md)
    const std::string token = newToken();
    const auto result = co_await db->execSqlCoro(
        std::string(
            "WITH s AS (INSERT INTO sessions (token_hash, user_id, expires_at)"
            " VALUES (decode($1, 'hex'), $2, now() + interval '24 hours')"
            " RETURNING user_id, expires_at)"
            " SELECT u.user_id, u.username, u.password_hash IS NOT NULL AS has_password, ") +
            kExpiresAtSql + " FROM s JOIN users u USING (user_id)",
        tokenHash(token), userId);

    co_return NewSession{.token = token, .user = toSessionUser(result[0])};
}

Task<HttpResponsePtr> startSession(orm::DbClientPtr db, HttpRequestPtr req, std::int64_t userId,
                                   HttpStatusCode status)
{
    const NewSession session = co_await createSession(db, req, userId);
    auto response = HttpResponse::newHttpJsonResponse(sessionInfo(session.user));
    response->setStatusCode(status);
    response->addCookie(sessionCookie(session.token));
    co_return response;
}

Task<std::expected<SessionUser, HttpResponsePtr>> requireSession(orm::DbClientPtr db,
                                                                 HttpRequestPtr req)
{
    auto user = co_await checkSession(db, req);
    if (!user)
    {
        Json::Value details;
        details["method"] = req->getMethodString();
        details["path"] = req->path();
        co_await logSecurityEvent(db, "access_denied", std::nullopt, req, details);
    }
    co_return user;
}

Task<HttpResponsePtr> getSessionHandler(orm::DbClientPtr db, HttpRequestPtr req)
{
    // Без журнала: SPA спрашивает сессию при каждой загрузке, у гостя это не попытка доступа
    const auto user = co_await checkSession(db, req);
    co_return user ? HttpResponse::newHttpJsonResponse(sessionInfo(*user)) : user.error();
}
