#include "auth/registration.hpp"

#include "auth/crypto.hpp"
#include "auth/http_common.hpp"
#include "auth/security_log.hpp"
#include "auth/session.hpp"
#include "auth/validation.hpp"
#include "error_response.hpp"

#include <drogon/drogon.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>

using namespace drogon;

namespace
{
constexpr int kResendSeconds = 60;  // #17148 п. 2.2 шаг 10

HttpResponsePtr emailTaken()
{
    return errorResponse(k409Conflict, "AUTH_EMAIL_TAKEN",
                         "Пользователь с таким email уже существует", "email");
}

HttpResponsePtr usernameTaken()
{
    return errorResponse(k409Conflict, "AUTH_USERNAME_TAKEN", "Это имя пользователя уже занято",
                         "username");
}

// AuthConfirmationSent: через сколько секунд доступна кнопка «Отправить повторно».
HttpResponsePtr confirmationSent(int resendAfter, HttpStatusCode status)
{
    Json::Value body;
    body["resend_after"] = resendAfter;
    auto response = HttpResponse::newHttpJsonResponse(body);
    response->setStatusCode(status);
    return response;
}

int currentYear()
{
    using namespace std::chrono;
    return static_cast<int>(year_month_day{floor<days>(system_clock::now())}.year());
}

// payload письма в email_outbox: ссылка ведёт на страницу SPA, а не в API - почтовые сканеры
// открывают ссылки из писем и израсходовали бы токен (описание POST /auth/confirm-email).
std::string confirmationPayload(const std::string &appUrl, const std::string &token)
{
    Json::Value payload;
    payload["link"] = appUrl + "/confirm-email?token=" + token;
    return payload.toStyledString();
}
}  // namespace

Task<HttpResponsePtr> registerHandler(orm::DbClientPtr db, std::string appUrl, HttpRequestPtr req)
{
    const Json::Value *json = jsonBody(req);
    if (!json)
    {
        co_return malformedBody();
    }
    const std::string username = stringField(*json, "username");
    const std::string email = stringField(*json, "email");
    const std::string password = stringField(*json, "password");
    // Не целое число - та же ошибка, что и год вне диапазона: 0 в диапазон не попадает
    const Json::Value &birthYearValue = (*json)["birth_year"];
    const int birthYear = birthYearValue.isInt() ? birthYearValue.asInt() : 0;

    // Порядок проверок - порядок полей формы (#17148 п. 2.2 шаг 3): ответ - первая ошибка
    const auto error = validateUsername(username)
                           .or_else([&] { return validateEmail(email); })
                           .or_else([&] { return validateBirthYear(birthYear, currentYear()); })
                           .or_else([&] { return validatePassword(password); });
    if (error)
    {
        co_return fieldErrorResponse(*error);
    }

    // Сначала email, потом имя - как в таблице #17148 п. 2.4
    const auto taken = co_await db->execSqlCoro(
        "SELECT EXISTS (SELECT 1 FROM users WHERE lower(email) = lower($1)) AS email_taken, "
        "EXISTS (SELECT 1 FROM users WHERE lower(username) = lower($2)) AS username_taken",
        email, username);
    if (taken[0]["email_taken"].as<bool>())
    {
        co_return emailTaken();
    }
    if (taken[0]["username_taken"].as<bool>())
    {
        co_return usernameTaken();
    }

    // Пользователь, ссылка и письмо - одним запросом, поэтому атомарно: либо всё, либо ничего.
    // Год передаём строкой: Drogon шлёт int как 4 байта, а колонка smallint ждёт 2.
    const std::string token = newToken();
    try
    {
        co_await db->execSqlCoro(
            "WITH u AS (INSERT INTO users (username, email, birth_year, password_hash) "
            "VALUES ($1, $2, $3::smallint, $4) RETURNING user_id), "
            "t AS (INSERT INTO auth_tokens (token_hash, user_id, purpose, expires_at) "
            "SELECT decode($5, 'hex'), user_id, 'email_confirm', now() + interval '24 hours' "
            "FROM u) "
            "INSERT INTO email_outbox (user_id, kind, payload) "
            "SELECT user_id, 'email_confirm', $6::jsonb FROM u",
            username, email, std::to_string(birthYear), hashPassword(password), tokenHash(token),
            confirmationPayload(appUrl, token));
    }
    catch (const orm::DrogonDbException &e)
    {
        // Email или имя заняли между проверкой и вставкой: сработал уникальный индекс
        const std::string message = e.base().what();
        if (message.contains("users_email_key"))
        {
            co_return emailTaken();
        }
        if (message.contains("users_username_key"))
        {
            co_return usernameTaken();
        }
        throw;
    }

    co_return confirmationSent(kResendSeconds, k201Created);
}

Task<HttpResponsePtr> confirmEmailHandler(orm::DbClientPtr db, HttpRequestPtr req)
{
    const Json::Value *json = jsonBody(req);
    if (!json)
    {
        co_return malformedBody();
    }

    // Ссылка гасится и учётная запись активируется одним запросом. Истёкшие ссылки не удаляем:
    // по ним можно запросить новую (POST /auth/resend-confirmation с token).
    const auto activated = co_await db->execSqlCoro(
        "WITH t AS (DELETE FROM auth_tokens WHERE token_hash = decode($1, 'hex') "
        "AND purpose = 'email_confirm' AND expires_at > now() RETURNING user_id) "
        "UPDATE users SET status = 'active' FROM t "
        "WHERE users.user_id = t.user_id AND users.status = 'unconfirmed' "
        "RETURNING users.user_id",
        tokenHash(stringField(*json, "token")));
    // Истекла, заменена новой или уже использована - один ответ (#17148 п. 2.5)
    if (activated.empty())
    {
        co_return errorResponse(k410Gone, "AUTH_CONFIRMATION_LINK_EXPIRED",
                                "Срок действия ссылки истёк");
    }

    // Подтверждение - это и вход (#17148 п. 2.2 шаг 12)
    const auto userId = activated[0]["user_id"].as<std::int64_t>();
    Json::Value details;
    details["method"] = "email_confirm";
    co_await logSecurityEvent(db, "login_success", userId, req, details);
    co_return co_await startSession(db, req, userId);
}

Task<HttpResponsePtr> resendConfirmationHandler(orm::DbClientPtr db, std::string appUrl,
                                                HttpRequestPtr req)
{
    const Json::Value *json = jsonBody(req);
    if (!json)
    {
        co_return malformedBody();
    }

    // Неподтверждённая учётная запись по токену из старой ссылки (в том числе истёкшей) или
    // по login
    std::optional<std::int64_t> userId;
    if (const std::string token = stringField(*json, "token"); !token.empty())
    {
        const auto found = co_await db->execSqlCoro(
            "SELECT user_id FROM auth_tokens JOIN users USING (user_id) "
            "WHERE token_hash = decode($1, 'hex') AND purpose = 'email_confirm' "
            "AND status = 'unconfirmed'",
            tokenHash(token));
        if (!found.empty())
        {
            userId = found[0]["user_id"].as<std::int64_t>();
        }
    }
    else
    {
        const std::string login = stringField(*json, "login");
        if (const auto error = validateLogin(login))
        {
            co_return fieldErrorResponse(*error);
        }
        const auto found = co_await db->execSqlCoro(
            login.contains('@') ? "SELECT user_id FROM users WHERE lower(email) = lower($1) AND "
                                  "status = 'unconfirmed'"
                                : "SELECT user_id FROM users WHERE lower(username) = lower($1) "
                                  "AND status = 'unconfirmed'",
            login);
        if (!found.empty())
        {
            userId = found[0]["user_id"].as<std::int64_t>();
        }
    }
    // Нет учётной записи или она уже подтверждена: письмо не отправляем, а ответ тот же, что и
    // при отправке - по нему нельзя узнать, зарегистрирован ли адрес
    if (!userId)
    {
        co_return confirmationSent(kResendSeconds, k202Accepted);
    }

    // Новая ссылка заменяет старую, только если с прошлой отправки прошло 60 с. Проверка и
    // замена - в одном запросе (WHERE в ON CONFLICT): два быстрых нажатия не отправят два письма.
    // Итоговый SELECT видит таблицу до изменений запроса, то есть время прошлой отправки.
    const std::string token = newToken();
    const auto result = co_await db->execSqlCoro(
        "WITH t AS (INSERT INTO auth_tokens (token_hash, user_id, purpose, expires_at) "
        "VALUES (decode($1, 'hex'), $2, 'email_confirm', now() + interval '24 hours') "
        "ON CONFLICT (user_id, purpose) DO UPDATE SET token_hash = EXCLUDED.token_hash, "
        "created_at = now(), expires_at = EXCLUDED.expires_at "
        "WHERE auth_tokens.created_at <= now() - interval '60 seconds' "
        "RETURNING user_id), "
        "o AS (INSERT INTO email_outbox (user_id, kind, payload) "
        "SELECT user_id, 'email_confirm', $3::jsonb FROM t) "
        "SELECT EXISTS (SELECT 1 FROM t) AS sent, "
        "(SELECT ceil(extract(epoch FROM created_at + interval '60 seconds' - now()))::int "
        "FROM auth_tokens WHERE user_id = $2 AND purpose = 'email_confirm') AS wait",
        tokenHash(token), *userId, confirmationPayload(appUrl, token));

    if (result[0]["sent"].as<bool>())
    {
        co_return confirmationSent(kResendSeconds, k202Accepted);
    }
    const int wait = result[0]["wait"].isNull() ? 0 : result[0]["wait"].as<int>();
    co_return confirmationSent(std::clamp(wait, 0, kResendSeconds), k202Accepted);
}
