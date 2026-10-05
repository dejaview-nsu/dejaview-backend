#include "auth/registration.hpp"
#include "integration/db_test.hpp"

using namespace drogon;

namespace
{
constexpr const char *kAppUrl = "https://dejaview.ru";

Json::Value registration(const std::string &username, const std::string &email)
{
    Json::Value json;
    json["username"] = username;
    json["email"] = email;
    json["birth_year"] = 1998;
    json["password"] = "Kino#2026";
    return json;
}
}  // namespace

class RegistrationTest : public DbTest
{
  protected:
    HttpResponsePtr registerUser(const Json::Value &json)
    {
        return run(registerHandler(db, kAppUrl, request(Post, "/api/v1/auth/register", json)));
    }

    HttpResponsePtr confirm(const std::string &token)
    {
        Json::Value json;
        json["token"] = token;
        return run(confirmEmailHandler(db, request(Post, "/api/v1/auth/confirm-email", json)));
    }

    HttpResponsePtr resend(const Json::Value &json)
    {
        return run(resendConfirmationHandler(
            db, kAppUrl, request(Post, "/api/v1/auth/resend-confirmation", json)));
    }

    // Токен из ссылки в последнем письме очереди
    std::string lastLinkToken()
    {
        const std::string link =
            scalar("SELECT payload->>'link' FROM email_outbox ORDER BY email_id DESC LIMIT 1");
        return link.substr(link.find("token=") + 6);
    }
};

TEST_F(RegistrationTest, CreatesUnconfirmedUserWithLinkAndEmail)
{
    const auto response = registerUser(registration("Ivan_P", "Ivan@Example.com"));
    EXPECT_EQ(response->statusCode(), k201Created);
    EXPECT_EQ(body(response)["resend_after"].asInt(), 60);

    EXPECT_EQ(scalar("SELECT status FROM users WHERE username = 'Ivan_P'"), "unconfirmed");
    EXPECT_EQ(scalar("SELECT password_hash LIKE '$argon2id$%' FROM users"), "t");
    // ссылка живёт 24 ч, в БД - только хеш токена
    EXPECT_EQ(scalar("SELECT expires_at - created_at FROM auth_tokens"), "1 day");
    EXPECT_EQ(scalar("SELECT kind FROM email_outbox"), "email_confirm");
    const std::string link = scalar("SELECT payload->>'link' FROM email_outbox");
    EXPECT_TRUE(link.starts_with("https://dejaview.ru/confirm-email?token=")) << link;
}

TEST_F(RegistrationTest, RejectsInvalidFieldsAndMalformedBody)
{
    auto json = registration("ab", "ivan@example.com");
    EXPECT_EQ(body(registerUser(json))["field"].asString(), "username");
    json = registration("ivan", "ivan@example.com");
    json["birth_year"] = "1998";  // строка, а не число
    EXPECT_EQ(body(registerUser(json))["code"].asString(), "AUTH_BIRTH_YEAR_INVALID");

    const auto malformed =
        run(registerHandler(db, kAppUrl, request(Post, "/api/v1/auth/register")));
    EXPECT_EQ(malformed->statusCode(), k400BadRequest);
    EXPECT_EQ(scalar("SELECT count(*) FROM users"), "0");
}

TEST_F(RegistrationTest, EmailAndUsernameAreUniqueIgnoringCase)
{
    registerUser(registration("ivan", "ivan@example.com"));

    const auto emailTaken = registerUser(registration("other", "IVAN@example.com"));
    EXPECT_EQ(emailTaken->statusCode(), k409Conflict);
    EXPECT_EQ(body(emailTaken)["code"].asString(), "AUTH_EMAIL_TAKEN");

    const auto nameTaken = registerUser(registration("IVAN", "other@example.com"));
    EXPECT_EQ(body(nameTaken)["code"].asString(), "AUTH_USERNAME_TAKEN");
}

TEST_F(RegistrationTest, ConfirmActivatesAndStartsSession)
{
    registerUser(registration("ivan", "ivan@example.com"));
    const auto response = confirm(lastLinkToken());

    EXPECT_EQ(response->statusCode(), k200OK);
    EXPECT_EQ(body(response)["user"]["username"].asString(), "ivan");
    EXPECT_FALSE(response->getCookie("dv_session").value().empty());
    EXPECT_EQ(scalar("SELECT status FROM users"), "active");
    EXPECT_EQ(scalar("SELECT count(*) FROM auth_tokens"), "0");  // ссылка одноразовая
    EXPECT_EQ(scalar("SELECT count(*) FROM sessions"), "1");
    EXPECT_EQ(scalar("SELECT details->>'method' FROM security_events "
                     "WHERE event_type = 'login_success'"),
              "email_confirm");
}

TEST_F(RegistrationTest, UsedOrExpiredLinkIsGone)
{
    registerUser(registration("ivan", "ivan@example.com"));
    const std::string token = lastLinkToken();
    confirm(token);
    EXPECT_EQ(confirm(token)->statusCode(), k410Gone);  // повторно

    registerUser(registration("petr", "petr@example.com"));
    db->execSqlSync(
        "UPDATE auth_tokens SET created_at = now() - interval '25 hours', "
        "expires_at = now() - interval '1 hour'");
    const auto expired = confirm(lastLinkToken());
    EXPECT_EQ(expired->statusCode(), k410Gone);
    EXPECT_EQ(body(expired)["code"].asString(), "AUTH_CONFIRMATION_LINK_EXPIRED");
    EXPECT_EQ(scalar("SELECT status FROM users WHERE username = 'petr'"), "unconfirmed");
}

TEST_F(RegistrationTest, ResendRespectsSixtySecondsAndReplacesLink)
{
    registerUser(registration("ivan", "ivan@example.com"));
    const std::string oldToken = lastLinkToken();

    Json::Value byLogin;
    byLogin["login"] = "ivan@example.com";
    const auto tooEarly = resend(byLogin);
    EXPECT_EQ(tooEarly->statusCode(), k202Accepted);
    EXPECT_GT(body(tooEarly)["resend_after"].asInt(), 0);
    EXPECT_EQ(scalar("SELECT count(*) FROM email_outbox"), "1");  // второго письма нет

    // «прошла минута»: сдвигаются обе метки, иначе срок ссылки превысил бы 24 ч (CHECK)
    db->execSqlSync(
        "UPDATE auth_tokens SET created_at = created_at - interval '61 seconds', "
        "expires_at = expires_at - interval '61 seconds'");
    EXPECT_EQ(body(resend(byLogin))["resend_after"].asInt(), 60);
    EXPECT_EQ(scalar("SELECT count(*) FROM email_outbox"), "2");
    EXPECT_EQ(confirm(oldToken)->statusCode(), k410Gone);  // старая ссылка заменена
    EXPECT_EQ(confirm(lastLinkToken())->statusCode(), k200OK);
}

TEST_F(RegistrationTest, ResendByExpiredLinkToken)
{
    registerUser(registration("ivan", "ivan@example.com"));
    db->execSqlSync(
        "UPDATE auth_tokens SET created_at = now() - interval '25 hours', "
        "expires_at = now() - interval '1 hour'");
    Json::Value byToken;
    byToken["token"] = lastLinkToken();
    EXPECT_EQ(body(resend(byToken))["resend_after"].asInt(), 60);
    EXPECT_EQ(scalar("SELECT count(*) FROM email_outbox"), "2");
}

TEST_F(RegistrationTest, ResendDoesNotRevealAccounts)
{
    // нет такого адреса - ответ как при отправке, письма нет
    Json::Value unknown;
    unknown["login"] = "nobody@example.com";
    EXPECT_EQ(body(resend(unknown))["resend_after"].asInt(), 60);
    EXPECT_EQ(scalar("SELECT count(*) FROM email_outbox"), "0");

    Json::Value invalid;
    invalid["login"] = "a b";
    EXPECT_EQ(resend(invalid)->statusCode(), k400BadRequest);
}
