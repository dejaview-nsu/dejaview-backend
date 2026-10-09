#include "auth/password.hpp"
#include "auth/login.hpp"
#include "auth/session.hpp"
#include "integration/db_test.hpp"

using namespace drogon;

namespace
{
constexpr const char *kAppUrl = "https://dejaview.ru";

Json::Value passwords(const std::string &current, const std::string &newPassword)
{
    Json::Value json;
    json["current_password"] = current;
    json["new_password"] = newPassword;
    return json;
}
}  // namespace

class PasswordTest : public DbTest
{
  protected:
    HttpResponsePtr requestReset(const std::string &login)
    {
        Json::Value json;
        json["login"] = login;
        return run(requestPasswordResetHandler(
            db, kAppUrl, request(Post, "/api/v1/auth/password-reset/request", json)));
    }

    HttpResponsePtr check(const std::string &token)
    {
        Json::Value json;
        json["token"] = token;
        return run(checkPasswordResetHandler(
            db, request(Post, "/api/v1/auth/password-reset/check", json)));
    }

    HttpResponsePtr complete(const std::string &token, const std::string &newPassword)
    {
        Json::Value json;
        json["token"] = token;
        json["new_password"] = newPassword;
        return run(completePasswordResetHandler(
            db, kAppUrl, request(Post, "/api/v1/auth/password-reset/complete", json)));
    }

    HttpResponsePtr login(const std::string &login, const std::string &password)
    {
        Json::Value json;
        json["login"] = login;
        json["password"] = password;
        return run(loginHandler(db, {}, request(Post, "/api/v1/auth/login", json)));
    }

    HttpResponsePtr changePassword(const std::string &session, const Json::Value &json)
    {
        return run(changePasswordHandler(
            db, kAppUrl,
            request(Put, "/api/v1/users/me/password", json, {{"dv_session", session}})));
    }

    HttpStatusCode sessionStatus(const std::string &token)
    {
        return run(getSessionHandler(db, request(Get, "/api/v1/auth/session", Json::Value(),
                                                 {{"dv_session", token}})))
            ->statusCode();
    }
};

TEST_F(PasswordTest, RequestSendsOneHourLink)
{
    createUser("ivan", "ivan@example.com");
    const auto response = requestReset("Ivan@Example.com");
    EXPECT_EQ(response->statusCode(), k202Accepted);
    EXPECT_TRUE(response->body().empty());

    EXPECT_EQ(scalar("SELECT kind FROM email_outbox"), "password_reset");
    const std::string link = scalar("SELECT payload->>'link' FROM email_outbox");
    EXPECT_TRUE(link.starts_with("https://dejaview.ru/reset-password?token=")) << link;
    // ссылка живёт 1 ч, в БД - только хеш токена
    EXPECT_EQ(scalar("SELECT expires_at - created_at FROM auth_tokens"), "01:00:00");

    EXPECT_EQ(check(lastLinkToken())->statusCode(), k204NoContent);
    EXPECT_EQ(check(lastLinkToken())->statusCode(), k204NoContent);  // проверка не расходует
}

TEST_F(PasswordTest, RequestDoesNotRevealAccounts)
{
    createUser("new_user", "new@example.com", "unconfirmed");
    createUser("bad_user", "bad@example.com", "blocked");
    for (const char *login : {"nobody@example.com", "bad_user", "new_user"})
    {
        EXPECT_EQ(requestReset(login)->statusCode(), k202Accepted) << login;
    }
    // Письмо только неподтверждённой учётной записи: сброс по ссылке подтвердит её email.
    // Заблокированной и несуществующей - нет, а ответ тот же
    EXPECT_EQ(
        scalar("SELECT string_agg(username, ',') FROM email_outbox JOIN users USING (user_id)"),
        "new_user");

    const auto invalid = requestReset("a b");
    EXPECT_EQ(invalid->statusCode(), k400BadRequest);
    EXPECT_EQ(body(invalid)["field"].asString(), "login");
    const auto malformed = run(requestPasswordResetHandler(
        db, kAppUrl, request(Post, "/api/v1/auth/password-reset/request")));
    EXPECT_EQ(malformed->statusCode(), k400BadRequest);
}

TEST_F(PasswordTest, OnlyLastLinkWorksAndNewOneNotMoreOftenThanMinute)
{
    createUser("ivan", "ivan@example.com");
    requestReset("ivan");
    const std::string first = lastLinkToken();

    // повтор раньше чем через 60 с ничего не меняет: письма нет, прежняя ссылка действует
    EXPECT_EQ(requestReset("ivan")->statusCode(), k202Accepted);
    EXPECT_EQ(scalar("SELECT count(*) FROM email_outbox"), "1");
    EXPECT_EQ(check(first)->statusCode(), k204NoContent);

    // «прошла минута»: сдвигаются обе метки, иначе срок ссылки превысил бы 1 ч (CHECK)
    db->execSqlSync(
        "UPDATE auth_tokens SET created_at = created_at - interval '61 seconds', "
        "expires_at = expires_at - interval '61 seconds'");
    requestReset("ivan");
    EXPECT_EQ(scalar("SELECT count(*) FROM email_outbox"), "2");
    EXPECT_EQ(check(first)->statusCode(), k410Gone);  // прежняя ссылка аннулирована
    EXPECT_EQ(check(lastLinkToken())->statusCode(), k204NoContent);
}

TEST_F(PasswordTest, CheckRejectsExpiredBlockedAndForeignLinks)
{
    const auto ivanId = createUser("ivan", "ivan@example.com");
    requestReset("ivan");
    db->execSqlSync(
        "UPDATE auth_tokens SET created_at = now() - interval '2 hours', "
        "expires_at = now() - interval '1 hour'");
    const auto expired = check(lastLinkToken());
    EXPECT_EQ(expired->statusCode(), k410Gone);
    EXPECT_EQ(body(expired)["code"].asString(), "AUTH_RESET_LINK_EXPIRED");

    // учётную запись заблокировали после отправки письма
    createUser("petr", "petr@example.com");
    requestReset("petr");
    db->execSqlSync("UPDATE users SET status = 'blocked' WHERE username = 'petr'");
    EXPECT_EQ(check(lastLinkToken())->statusCode(), k410Gone);

    // ссылка подтверждения email - не ссылка сброса пароля
    db->execSqlSync(
        "INSERT INTO auth_tokens (token_hash, user_id, purpose, expires_at) "
        "VALUES (sha256('confirm-token'), " +
        std::to_string(ivanId) + ", 'email_confirm', now() + interval '1 hour')");
    EXPECT_EQ(check("confirm-token")->statusCode(), k410Gone);
    EXPECT_EQ(check("")->statusCode(), k410Gone);
}

// Путь из #17976: запрос сброса, письмо, новый пароль, вход. Старый пароль и прежние сессии
// больше не действуют
TEST_F(PasswordTest, ResetRequestEmailNewPasswordLogin)
{
    createUser("ivan", "ivan@example.com");
    const std::string oldSession = login("ivan", kPassword)->getCookie("dv_session").value();

    requestReset("ivan@example.com");
    const std::string token = lastLinkToken();  // из ссылки в письме
    const auto response = complete(token, "Nova#2027");
    EXPECT_EQ(response->statusCode(), k200OK);
    EXPECT_EQ(body(response)["user"]["username"].asString(), "ivan");
    const std::string newSession = response->getCookie("dv_session").value();

    EXPECT_EQ(sessionStatus(oldSession), k401Unauthorized);
    EXPECT_EQ(sessionStatus(newSession), k200OK);  // вошёл сразу после сброса
    EXPECT_EQ(login("ivan", kPassword)->statusCode(), k400BadRequest);
    EXPECT_EQ(login("ivan", "Nova#2027")->statusCode(), k200OK);

    EXPECT_EQ(complete(token, "Other#2028")->statusCode(), k410Gone);  // ссылка одноразовая
    EXPECT_EQ(scalar("SELECT payload->>'link' FROM email_outbox WHERE kind = 'password_changed'"),
              "https://dejaview.ru/forgot-password");
    EXPECT_EQ(scalar("SELECT string_agg(event_type, ',' ORDER BY event_id) FROM security_events "
                     "WHERE details->>'method' = 'password_reset'"),
              "password_change,login_success");
}

TEST_F(PasswordTest, RejectedNewPasswordKeepsLink)
{
    createUser("ivan", "ivan@example.com");
    requestReset("ivan");
    const std::string token = lastLinkToken();

    const auto same = complete(token, kPassword);
    EXPECT_EQ(same->statusCode(), k400BadRequest);
    EXPECT_EQ(body(same)["code"].asString(), "AUTH_PASSWORD_SAME_AS_CURRENT");
    EXPECT_EQ(body(same)["field"].asString(), "new_password");
    const auto weak = complete(token, "");
    EXPECT_EQ(body(weak)["message"].asString(), "Введите новый пароль");

    EXPECT_EQ(complete(token, "Nova#2027")->statusCode(), k200OK);
}

TEST_F(PasswordTest, ResetConfirmsEmailAndClearsLoginLock)
{
    createUser("ivan", "ivan@example.com", "unconfirmed");
    db->execSqlSync(
        "UPDATE users SET failed_login_count = 5, locked_until = now() + interval '15 minutes'");
    requestReset("ivan");
    EXPECT_EQ(complete(lastLinkToken(), "Nova#2027")->statusCode(), k200OK);

    EXPECT_EQ(scalar("SELECT status || ' ' || failed_login_count || ' ' || (locked_until IS NULL) "
                     "FROM users"),
              "active 0 true");
    EXPECT_EQ(login("ivan", "Nova#2027")->statusCode(), k200OK);
}

TEST_F(PasswordTest, ResetGivesOidcAccountItsFirstPassword)
{
    db->execSqlSync(
        "INSERT INTO users (username, email, birth_year, status) "
        "VALUES ('ivan', 'ivan@example.com', 2000, 'active')");
    requestReset("ivan");
    EXPECT_EQ(complete(lastLinkToken(), "Nova#2027")->statusCode(), k200OK);
    EXPECT_EQ(login("ivan", "Nova#2027")->statusCode(), k200OK);
}

TEST_F(PasswordTest, BlockedAccountCannotCompleteReset)
{
    createUser("ivan", "ivan@example.com");
    requestReset("ivan");
    db->execSqlSync("UPDATE users SET status = 'blocked'");

    const auto response = complete(lastLinkToken(), "Nova#2027");
    EXPECT_EQ(response->statusCode(), k410Gone);
    EXPECT_EQ(body(response)["code"].asString(), "AUTH_RESET_LINK_EXPIRED");
    EXPECT_EQ(scalar("SELECT count(*) FROM email_outbox WHERE kind = 'password_changed'"), "0");
}

TEST_F(PasswordTest, ChangeEndsOtherSessionsButKeepsCurrent)
{
    createUser("ivan", "ivan@example.com");
    const std::string phone = login("ivan", kPassword)->getCookie("dv_session").value();
    const std::string laptop = login("ivan", kPassword)->getCookie("dv_session").value();

    EXPECT_EQ(changePassword(phone, passwords(kPassword, "Nova#2027"))->statusCode(),
              k204NoContent);
    EXPECT_EQ(sessionStatus(phone), k200OK);
    EXPECT_EQ(sessionStatus(laptop), k401Unauthorized);
    EXPECT_EQ(login("ivan", kPassword)->statusCode(), k400BadRequest);
    EXPECT_EQ(login("ivan", "Nova#2027")->statusCode(), k200OK);

    EXPECT_EQ(scalar("SELECT payload->>'link' FROM email_outbox WHERE kind = 'password_changed'"),
              "https://dejaview.ru/forgot-password");
    EXPECT_EQ(scalar("SELECT details->>'method' FROM security_events "
                     "WHERE event_type = 'password_change'"),
              "password_change");
}

TEST_F(PasswordTest, ChangeRequiresCurrentPassword)
{
    createUser("ivan", "ivan@example.com");
    const std::string session = login("ivan", kPassword)->getCookie("dv_session").value();

    const auto wrong = changePassword(session, passwords("Wrong#2026", "Nova#2027"));
    EXPECT_EQ(wrong->statusCode(), k400BadRequest);
    EXPECT_EQ(body(wrong)["code"].asString(), "AUTH_CURRENT_PASSWORD_INVALID");
    EXPECT_EQ(body(wrong)["field"].asString(), "current_password");
    // неверный текущий пароль - неудачная аутентификация, её видно в журнале
    EXPECT_EQ(scalar("SELECT details->>'method' FROM security_events "
                     "WHERE event_type = 'login_failure'"),
              "password_change");

    EXPECT_EQ(body(changePassword(session, passwords("", "Nova#2027")))["message"].asString(),
              "Введите текущий пароль");
    EXPECT_EQ(body(changePassword(session, passwords(kPassword, kPassword)))["code"].asString(),
              "AUTH_PASSWORD_SAME_AS_CURRENT");
    EXPECT_EQ(scalar("SELECT count(*) FROM email_outbox"), "0");  // пароль не менялся
    EXPECT_EQ(changePassword("", passwords(kPassword, "Nova#2027"))->statusCode(),
              k401Unauthorized);
    EXPECT_EQ(changePassword(session, Json::Value())->statusCode(), k400BadRequest);  // не JSON
}

TEST_F(PasswordTest, OidcAccountSetsPasswordWithoutCurrentOne)
{
    db->execSqlSync(
        "INSERT INTO users (username, email, birth_year, status) "
        "VALUES ('ivan', 'ivan@example.com', 2000, 'active')");
    db->execSqlSync(
        "INSERT INTO sessions (token_hash, user_id, expires_at) "
        "SELECT sha256('oidc-session'), user_id, now() + interval '1 hour' FROM users");

    Json::Value json;
    json["new_password"] = "Nova#2027";  // поля «Текущий пароль» у такой учётной записи нет
    EXPECT_EQ(changePassword("oidc-session", json)->statusCode(), k204NoContent);

    const auto session = run(getSessionHandler(
        db, request(Get, "/api/v1/auth/session", Json::Value(), {{"dv_session", "oidc-session"}})));
    EXPECT_TRUE(body(session)["user"]["has_password"].asBool());
    EXPECT_EQ(login("ivan", "Nova#2027")->statusCode(), k200OK);
}
