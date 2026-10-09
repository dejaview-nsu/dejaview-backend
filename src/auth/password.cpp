#include "auth/password.hpp"

#include "auth/crypto.hpp"
#include "auth/http_common.hpp"
#include "auth/password_rules.hpp"
#include "auth/security_log.hpp"
#include "auth/session.hpp"
#include "auth/validation.hpp"
#include "error_response.hpp"

#include <drogon/drogon.h>

#include <cstdint>
#include <optional>

using namespace drogon;

namespace
{
// Истекла, заменена новой или уже использована - один ответ (#17150 п. 2.2 шаг 2, п. 2.4)
HttpResponsePtr resetLinkExpired()
{
    return errorResponse(k410Gone, "AUTH_RESET_LINK_EXPIRED",
                         "Срок действия ссылки истёк. Запросите восстановление пароля повторно");
}

// payload письма в email_outbox: ссылка ведёт на страницу SPA, а не в API, как и у подтверждения
// email. Страница сначала проверяет ссылку (check), потом показывает форму «Новый пароль».
std::string resetPayload(const std::string &appUrl, const std::string &token)
{
    Json::Value payload;
    payload["link"] = appUrl + "/reset-password?token=" + token;
    return payload.toStyledString();
}

// payload письма о смене пароля: кнопка «Восстановить пароль» ведёт на форму запроса
std::string passwordChangedPayload(const std::string &appUrl)
{
    Json::Value payload;
    payload["link"] = appUrl + "/forgot-password";
    return payload.toStyledString();
}
}  // namespace

Task<HttpResponsePtr> requestPasswordResetHandler(orm::DbClientPtr db, std::string appUrl,
                                                  HttpRequestPtr req)
{
    const Json::Value *json = jsonBody(req);
    if (!json)
    {
        co_return malformedBody();
    }
    const std::string login = stringField(*json, "login");
    if (const auto error = validateLogin(login))
    {
        co_return fieldErrorResponse(*error);
    }

    const std::string token = newToken();
    co_await db->execSqlCoro(
        std::string("WITH u AS (SELECT user_id FROM users WHERE ") +
            (login.contains('@') ? "lower(email) = lower($1)" : "lower(username) = lower($1)") +
            " AND status <> 'blocked'), "
            "t AS (INSERT INTO auth_tokens (token_hash, user_id, purpose, expires_at) "
            "SELECT decode($2, 'hex'), user_id, 'password_reset', now() + interval '1 hour' "
            "FROM u "
            "ON CONFLICT (user_id, purpose) DO UPDATE SET token_hash = EXCLUDED.token_hash, "
            "created_at = now(), expires_at = EXCLUDED.expires_at "
            "WHERE auth_tokens.created_at <= now() - interval '60 seconds' "
            "RETURNING user_id) "
            "INSERT INTO email_outbox (user_id, kind, payload) "
            "SELECT user_id, 'password_reset', $3::jsonb FROM t",
        login, tokenHash(token), resetPayload(appUrl, token));

    co_return HttpResponse::newHttpResponse(k202Accepted, CT_NONE);
}

Task<HttpResponsePtr> checkPasswordResetHandler(orm::DbClientPtr db, HttpRequestPtr req)
{
    const Json::Value *json = jsonBody(req);
    if (!json)
    {
        co_return malformedBody();
    }
    // Ссылку учётной записи, заблокированной после отправки письма, тоже не принимаем
    const auto found = co_await db->execSqlCoro(
        "SELECT 1 FROM auth_tokens JOIN users USING (user_id) "
        "WHERE token_hash = decode($1, 'hex') AND purpose = 'password_reset' "
        "AND expires_at > now() AND status <> 'blocked'",
        tokenHash(stringField(*json, "token")));
    if (found.empty())
    {
        co_return resetLinkExpired();
    }
    co_return HttpResponse::newHttpResponse(k204NoContent, CT_NONE);
}

Task<HttpResponsePtr> completePasswordResetHandler(orm::DbClientPtr db, std::string appUrl,
                                                   HttpRequestPtr req)
{
    const Json::Value *json = jsonBody(req);
    if (!json)
    {
        co_return malformedBody();
    }
    const std::string hash = tokenHash(stringField(*json, "token"));
    const std::string newPassword = stringField(*json, "new_password");

    // Сначала ссылка: если она истекла, исправлять пароль бессмысленно
    const auto found = co_await db->execSqlCoro(
        "SELECT user_id, password_hash FROM auth_tokens JOIN users USING (user_id) "
        "WHERE token_hash = decode($1, 'hex') AND purpose = 'password_reset' "
        "AND expires_at > now() AND status <> 'blocked'",
        hash);
    if (found.empty())
    {
        co_return resetLinkExpired();
    }
    const auto &user = found[0];
    const auto currentHash = user["password_hash"].isNull()
                                 ? std::nullopt
                                 : std::optional(user["password_hash"].as<std::string>());
    // Ошибка пароля ссылку не расходует: его можно исправить и отправить снова
    if (const auto error = checkResetPassword(newPassword, currentHash))
    {
        co_return fieldErrorResponse(*error);
    }

    // Ссылка гасится, пароль меняется, все сессии завершаются, письмо в очередь - одним запросом.
    // Сброс подтверждает email (допущение контракта) и, как успешный вход, обнуляет счётчик подбора
    const auto changed = co_await db->execSqlCoro(
        "WITH t AS (DELETE FROM auth_tokens WHERE token_hash = decode($1, 'hex') "
        "AND purpose = 'password_reset' AND expires_at > now() RETURNING user_id), "
        "u AS (UPDATE users SET password_hash = $2, status = 'active', failed_login_count = 0, "
        "locked_until = NULL FROM t WHERE users.user_id = t.user_id AND users.status <> 'blocked' "
        "RETURNING users.user_id), "
        "s AS (DELETE FROM sessions WHERE user_id IN (SELECT user_id FROM u)), "
        "o AS (INSERT INTO email_outbox (user_id, kind, payload) "
        "SELECT user_id, 'password_changed', $3::jsonb FROM u) "
        "SELECT user_id FROM u",
        hash, hashPassword(newPassword), passwordChangedPayload(appUrl));
    if (changed.empty())
    {
        co_return resetLinkExpired();  // ссылку израсходовал параллельный запрос
    }

    const auto userId = changed[0]["user_id"].as<std::int64_t>();
    Json::Value details;
    details["method"] = "password_reset";
    co_await logSecurityEvent(db, "password_change", userId, req, details);
    co_await logSecurityEvent(db, "login_success", userId, req, details);
    co_return co_await startSession(db, req, userId);
}

Task<HttpResponsePtr> changePasswordHandler(orm::DbClientPtr db, std::string appUrl,
                                            HttpRequestPtr req)
{
    const auto user = co_await requireSession(db, req);
    if (!user)
    {
        co_return user.error();
    }
    const Json::Value *json = jsonBody(req);
    if (!json)
    {
        co_return malformedBody();
    }
    const std::string currentPassword = stringField(*json, "current_password");
    const std::string newPassword = stringField(*json, "new_password");

    // .at, а не []: строки нет, только если пользователя удалили сразу после проверки сессии
    const auto found = co_await db->execSqlCoro(
        "SELECT password_hash FROM users WHERE user_id = $1", user->userId);
    const auto &row = found.at(0);
    const auto currentHash = row["password_hash"].isNull()
                                 ? std::nullopt
                                 : std::optional(row["password_hash"].as<std::string>());
    if (const auto error = checkPasswordChange(currentPassword, newPassword, currentHash))
    {
        // Неверный текущий пароль - неудачная повторная аутентификация (#17094 п. 5.1)
        if (error->code == "AUTH_CURRENT_PASSWORD_INVALID")
        {
            Json::Value details;
            details["method"] = "password_change";
            details["reason"] = "invalid_credentials";
            co_await logSecurityEvent(db, "login_failure", user->userId, req, details);
        }
        co_return fieldErrorResponse(*error);
    }

    // Пароль меняется, только если он всё ещё тот, что проверен выше (compare-and-swap): иначе
    // запрос, начатый до сброса пароля, перезаписал бы новый. Сессии, кроме текущей, завершаются
    const auto changed = co_await db->execSqlCoro(
        "WITH u AS (UPDATE users SET password_hash = $2 WHERE user_id = $1 "
        "AND password_hash IS NOT DISTINCT FROM $3 RETURNING user_id), "
        "s AS (DELETE FROM sessions WHERE user_id IN (SELECT user_id FROM u) "
        "AND token_hash <> decode($4, 'hex')), "
        "o AS (INSERT INTO email_outbox (user_id, kind, payload) "
        "SELECT user_id, 'password_changed', $5::jsonb FROM u) "
        "SELECT user_id FROM u",
        user->userId, hashPassword(newPassword), currentHash,
        tokenHash(req->getCookie("dv_session")), passwordChangedPayload(appUrl));
    if (changed.empty())
    {
        co_return fieldErrorResponse(currentPasswordInvalid());
    }

    Json::Value details;
    details["method"] = "password_change";
    co_await logSecurityEvent(db, "password_change", user->userId, req, details);
    co_return HttpResponse::newHttpResponse(k204NoContent, CT_NONE);
}
