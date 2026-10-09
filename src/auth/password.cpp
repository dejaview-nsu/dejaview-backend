#include "auth/password.hpp"

#include "auth/crypto.hpp"
#include "auth/http_common.hpp"
#include "auth/validation.hpp"
#include "error_response.hpp"

#include <drogon/drogon.h>

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
