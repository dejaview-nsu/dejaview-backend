#include "auth/password.hpp"
#include "integration/db_test.hpp"

using namespace drogon;

namespace
{
constexpr const char *kAppUrl = "https://dejaview.ru";
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
