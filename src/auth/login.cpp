#include "auth/login.hpp"

#include "auth/captcha.hpp"
#include "auth/crypto.hpp"
#include "auth/http_common.hpp"
#include "auth/login_rules.hpp"
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
// Ошибка формы входа - AuthLoginError: Error и признак, нужна ли CAPTCHA следующей попытке.
HttpResponsePtr loginError(HttpStatusCode status, std::string_view code, std::string_view message,
                           std::string_view field, bool captchaRequired)
{
    Json::Value body = errorBody(code, message, field);
    body["captcha_required"] = captchaRequired;
    auto response = HttpResponse::newHttpJsonResponse(body);
    response->setStatusCode(status);
    return response;
}

HttpResponsePtr invalidCredentials(std::string_view login, bool captchaRequired)
{
    return loginError(k400BadRequest, "AUTH_INVALID_CREDENTIALS", invalidCredentialsMessage(login),
                      {}, captchaRequired);
}

HttpResponsePtr captchaRequiredError()
{
    return loginError(k403Forbidden, "AUTH_CAPTCHA_REQUIRED", "Подтвердите, что вы не робот",
                      "captcha_token", true);
}

// 429: тело - обычный Error, оставшееся время - в тексте и в заголовке Retry-After.
HttpResponsePtr loginLocked(int lockSeconds)
{
    auto response =
        errorResponse(k429TooManyRequests, "AUTH_LOGIN_LOCKED", loginLockedMessage(lockSeconds));
    response->addHeader("Retry-After", std::to_string(lockSeconds));
    return response;
}

// Неудачный вход в журнал (#17094 п. 5.1). Причина нужна для поиска подбора (п. 5.3).
Task<> logFailure(orm::DbClientPtr db, std::optional<std::int64_t> userId, HttpRequestPtr req,
                  std::string reason)
{
    Json::Value details;
    details["method"] = "password";
    details["reason"] = reason;
    co_await logSecurityEvent(db, "login_failure", userId, req, details);
}

// Сколько секунд осталось до конца блокировки, 0 - не заблокирован. Считает БД: и блокировку
// ставит она, часы одни.
constexpr const char *kLockSecondsSql =
    "greatest(ceil(extract(epoch FROM locked_until - now())), 0)::int AS lock_seconds";
}  // namespace

Task<HttpResponsePtr> loginHandler(orm::DbClientPtr db, std::string captchaKey, HttpRequestPtr req)
{
    const Json::Value *json = jsonBody(req);
    if (!json)
    {
        co_return loginError(k400BadRequest, "AUTH_VALIDATION_ERROR",
                             "Произошла ошибка. Попробуйте позже", {}, false);
    }
    const std::string login = stringField(*json, "login");
    const std::string password = stringField(*json, "password");
    if (const auto error = validateLogin(login))
    {
        co_return loginError(k400BadRequest, error->code, error->message, error->field, false);
    }
    if (password.empty())
    {
        co_return loginError(k400BadRequest, "AUTH_VALIDATION_ERROR", "Введите пароль", "password",
                             false);
    }
    const bool captchaEnabled = !captchaKey.empty();

    // Учётная запись и её защита от подбора. Конец блокировки обнуляет счётчик (#17149 п. 2.5),
    // поэтому истёкшая блокировка читается как 0 неудачных попыток.
    const auto found = co_await db->execSqlCoro(
        std::string("SELECT user_id, password_hash, status, "
                    "CASE WHEN locked_until <= now() THEN 0 ELSE failed_login_count END "
                    "AS failed_count, ") +
            kLockSecondsSql + " FROM users WHERE " +
            (login.contains('@') ? "lower(email) = lower($1)" : "lower(username) = lower($1)"),
        login);

    // Нет такого login или учётная запись только для входа через OIDC (пароля нет): ответ как на
    // неверный пароль. Счётчика нет: подбирать нечего
    if (found.empty() || found[0]["password_hash"].isNull())
    {
        const auto userId =
            found.empty() ? std::nullopt : std::optional(found[0]["user_id"].as<std::int64_t>());
        co_await logFailure(db, userId, req, "invalid_credentials");
        co_return invalidCredentials(login, false);
    }
    const auto &user = found[0];
    const auto userId = user["user_id"].as<std::int64_t>();

    // 1. Блокировка: пароль даже не проверяем
    const int lockSeconds = user["lock_seconds"].as<int>();
    const LoginGate gate = loginGate(user["failed_count"].as<int>(), lockSeconds);
    if (gate == LoginGate::Locked)
    {
        co_await logFailure(db, userId, req, "locked");
        co_return loginLocked(lockSeconds);
    }

    // 2. CAPTCHA. Эти ответы неудачной попыткой не считаются. После блока captchaChecked
    // означает «проверена и пройдена»: при неудаче мы уже вернули ответ
    const bool captchaChecked = gate == LoginGate::CaptchaRequired && captchaEnabled;
    if (captchaChecked)
    {
        const std::string captchaToken = stringField(*json, "captcha_token");
        if (captchaToken.empty())
        {
            co_return captchaRequiredError();
        }
        if (!co_await verifyCaptcha(captchaKey, captchaToken, clientIp(req)))
        {
            co_await logFailure(db, userId, req, "captcha_invalid");
            co_return loginError(k403Forbidden, "AUTH_CAPTCHA_INVALID",
                                 "Проверка не пройдена. Попробуйте ещё раз", "captcha_token", true);
        }
    }

    // 3. Попытка засчитывается ДО проверки пароля, одним атомарным UPDATE. Если сначала
    // проверять, а потом увеличивать счётчик, 100 параллельных запросов прочитают «0 неудач»
    // и получат 100 попыток вместо 5. Здесь каждая попытка занимает место в счётчике, а WHERE
    // повторяет проверки шагов 1-2 уже на свежей строке. Верный пароль потом обнулит счётчик.
    // Истёкшая блокировка (locked_until в прошлом) сбрасывает счётчик: эта попытка - первая.
    const auto reserved = co_await db->execSqlCoro(
        "UPDATE users SET "
        "failed_login_count = CASE WHEN locked_until IS NULL THEN failed_login_count + 1 ELSE 1 "
        "END, "
        "locked_until = CASE WHEN locked_until IS NULL AND failed_login_count + 1 >= $3::int "
        "THEN now() + $4::int * interval '1 second' END "
        "WHERE user_id = $1 AND (locked_until IS NULL OR locked_until <= now()) "
        "AND (locked_until IS NOT NULL OR failed_login_count < $5::int OR $2::boolean) "
        "RETURNING failed_login_count",
        userId, captchaChecked || !captchaEnabled, kLockAfterFailures, kLockSeconds,
        kCaptchaAfterFailures);
    if (reserved.empty())
    {
        // Пока мы проверяли, параллельные попытки заблокировали вход или потребовали CAPTCHA
        const auto now = co_await db->execSqlCoro(
            std::string("SELECT ") + kLockSecondsSql + " FROM users WHERE user_id = $1", userId);
        const int seconds = now[0]["lock_seconds"].as<int>();
        if (seconds > 0)
        {
            co_await logFailure(db, userId, req, "locked");
            co_return loginLocked(seconds);
        }
        co_return captchaRequiredError();
    }

    // 4. Пароль. Длиннее 128 символов не хешируем: таких паролей не бывает (#17148 п. 2.1), а
    // хешировать мегабайтную строку - лишняя нагрузка
    const bool passwordOk = utf8Length(password) <= 128 &&
                            verifyPassword(password, user["password_hash"].as<std::string>());
    if (!passwordOk)
    {
        const FailedLogin failed = afterFailedLogin(reserved[0]["failed_login_count"].as<int>());
        co_await logFailure(db, userId, req, "invalid_credentials");
        if (failed.locked)
        {
            co_return loginLocked(kLockSeconds);
        }
        co_return invalidCredentials(login, failed.captchaRequired && captchaEnabled);
    }

    // Верный пароль - не подбор: счётчик обнуляется, даже если войти нельзя (шаги 4-5 в описании)
    co_await db->execSqlCoro(
        "UPDATE users SET failed_login_count = 0, locked_until = NULL WHERE user_id = $1", userId);

    // 5. Состояние учётной записи видно только после верного пароля
    const std::string status = user["status"].as<std::string>();
    if (status == "unconfirmed")
    {
        co_await logFailure(db, userId, req, "email_not_confirmed");
        co_return loginError(k403Forbidden, "AUTH_EMAIL_NOT_CONFIRMED",
                             "Email не подтверждён. Проверьте почту или запросите новую ссылку", {},
                             false);
    }
    if (status == "blocked")
    {
        co_await logFailure(db, userId, req, "account_blocked");
        co_return loginError(k403Forbidden, "AUTH_ACCOUNT_BLOCKED",
                             "Ваша учётная запись заблокирована. Обратитесь в службу поддержки", {},
                             false);
    }

    Json::Value details;
    details["method"] = "password";
    co_await logSecurityEvent(db, "login_success", userId, req, details);
    co_return co_await startSession(db, req, userId);
}
