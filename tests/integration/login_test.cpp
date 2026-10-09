#include "auth/login.hpp"
#include "auth/session.hpp"
#include "integration/db_test.hpp"

using namespace drogon;

class LoginTest : public DbTest
{
  protected:
    CaptchaSettings captcha;  // без ключа - CAPTCHA выключена

    HttpResponsePtr login(const std::string &login, const std::string &password,
                          const std::string &captchaToken = "",
                          const std::map<std::string, std::string> &cookies = {})
    {
        Json::Value json;
        json["login"] = login;
        json["password"] = password;
        if (!captchaToken.empty())
        {
            json["captcha_token"] = captchaToken;
        }
        return run(loginHandler(db, captcha, request(Post, "/api/v1/auth/login", json, cookies)));
    }

    // После 3 неудачных попыток: следующий вход - только с CAPTCHA. SmartCaptcha - фейковая
    void requireCaptcha()
    {
        db->execSqlSync("UPDATE users SET failed_login_count = 3");
        captcha = {.serverKey = "server-key", .url = fakeUrl};
    }
};

TEST_F(LoginTest, SucceedsByUsernameOrEmailAndResetsCounter)
{
    createUser("ivan", "ivan@example.com");
    db->execSqlSync("UPDATE users SET failed_login_count = 2");

    const auto byName = login("IVAN", kPassword);
    EXPECT_EQ(byName->statusCode(), k200OK);
    EXPECT_EQ(body(byName)["user"]["username"].asString(), "ivan");
    EXPECT_FALSE(byName->getCookie("dv_session").value().empty());
    EXPECT_EQ(scalar("SELECT failed_login_count FROM users"), "0");

    EXPECT_EQ(login("Ivan@Example.com", kPassword)->statusCode(), k200OK);
    EXPECT_EQ(scalar("SELECT count(*) FROM security_events WHERE event_type = 'login_success'"),
              "2");
}

TEST_F(LoginTest, NewLoginReplacesSessionFromCookie)
{
    createUser("ivan", "ivan@example.com");
    const std::string first = login("ivan", kPassword)->getCookie("dv_session").value();
    login("ivan", kPassword, "", {{"dv_session", first}});
    EXPECT_EQ(scalar("SELECT count(*) FROM sessions"), "1");
}

TEST_F(LoginTest, UnknownLoginLooksLikeWrongPassword)
{
    createUser("ivan", "ivan@example.com");
    const auto unknown = login("nobody", kPassword);
    const auto wrong = login("ivan", "wrong");
    EXPECT_EQ(unknown->statusCode(), k400BadRequest);
    EXPECT_EQ(body(unknown)["message"], body(wrong)["message"]);
    EXPECT_EQ(body(login("nobody@example.com", kPassword))["message"].asString(),
              "Неверные email или пароль");
}

TEST_F(LoginTest, ValidatesFields)
{
    EXPECT_EQ(body(login("", kPassword))["field"].asString(), "login");
    EXPECT_EQ(body(login("ivan", ""))["field"].asString(), "password");
    const auto malformed = run(loginHandler(db, captcha, request(Post, "/api/v1/auth/login")));
    EXPECT_EQ(malformed->statusCode(), k400BadRequest);
    EXPECT_FALSE(body(malformed)["captcha_required"].asBool());
}

TEST_F(LoginTest, CaptchaFromThirdFailureAndLockAfterFifth)
{
    createUser("ivan", "ivan@example.com");
    captcha.serverKey = "server-key";  // CAPTCHA включена, до проверки токена дело не дойдёт

    EXPECT_FALSE(body(login("ivan", "wrong"))["captcha_required"].asBool());
    EXPECT_FALSE(body(login("ivan", "wrong"))["captcha_required"].asBool());
    EXPECT_TRUE(body(login("ivan", "wrong"))["captcha_required"].asBool());

    // 4-я попытка без токена CAPTCHA - 403, неудачной не считается
    const auto noCaptcha = login("ivan", kPassword);
    EXPECT_EQ(noCaptcha->statusCode(), k403Forbidden);
    EXPECT_EQ(body(noCaptcha)["code"].asString(), "AUTH_CAPTCHA_REQUIRED");
    EXPECT_EQ(scalar("SELECT failed_login_count FROM users"), "3");
}

TEST_F(LoginTest, CaptchaTokenIsCheckedBySmartCaptcha)
{
    createUser("ivan", "ivan@example.com");
    requireCaptcha();

    fakeReply("/validate", k200OK, R"({"status": "failed", "message": "Token invalid"})");
    const auto rejected = login("ivan", kPassword, "captcha-token");
    EXPECT_EQ(rejected->statusCode(), k403Forbidden);
    EXPECT_EQ(body(rejected)["code"].asString(), "AUTH_CAPTCHA_INVALID");
    EXPECT_EQ(scalar("SELECT failed_login_count FROM users"), "3");  // неудачей не считается
    EXPECT_EQ(scalar("SELECT details->>'reason' FROM security_events"), "captcha_invalid");
    const auto check = fakeReceived("/validate");
    ASSERT_TRUE(check);
    EXPECT_EQ(check->getParameter("secret"), "server-key");
    EXPECT_EQ(check->getParameter("token"), "captcha-token");

    fakeReply("/validate", k200OK, R"({"status": "ok"})");
    EXPECT_EQ(login("ivan", kPassword, "captcha-token")->statusCode(), k200OK);
}

TEST_F(LoginTest, SmartCaptchaFailureDoesNotBlockLogin)
{
    // Сбой у Яндекса или неверный серверный ключ вход не закрывают: от подбора защищает
    // блокировка после 5-й неудачи
    createUser("ivan", "ivan@example.com");
    for (const auto status : {k500InternalServerError, k403Forbidden})
    {
        requireCaptcha();
        fakeReply("/validate", status, "{}");
        EXPECT_EQ(login("ivan", kPassword, "captcha-token")->statusCode(), k200OK) << status;
    }
    requireCaptcha();
    captcha.url = "http://127.0.0.1:1";  // сервис недоступен: на этом порту никто не слушает
    EXPECT_EQ(login("ivan", kPassword, "captcha-token")->statusCode(), k200OK);
}

TEST_F(LoginTest, FifthFailureLocksForFifteenMinutes)
{
    createUser("ivan", "ivan@example.com");
    for (int i = 0; i < 4; ++i)
    {
        EXPECT_EQ(login("ivan", "wrong")->statusCode(), k400BadRequest);
    }
    const auto fifth = login("ivan", "wrong");
    EXPECT_EQ(fifth->statusCode(), k429TooManyRequests);
    EXPECT_EQ(body(fifth)["code"].asString(), "AUTH_LOGIN_LOCKED");
    EXPECT_EQ(fifth->getHeader("Retry-After"), "900");

    // во время блокировки не проходит и верный пароль
    const auto locked = login("ivan", kPassword);
    EXPECT_EQ(locked->statusCode(), k429TooManyRequests);
    EXPECT_EQ(body(locked)["message"].asString(),
              "Слишком много попыток входа. Повторите через 15 минут");

    // блокировка истекла - вход проходит, счётчик обнулён
    db->execSqlSync("UPDATE users SET locked_until = now() - interval '1 second'");
    EXPECT_EQ(login("ivan", kPassword)->statusCode(), k200OK);
    EXPECT_EQ(scalar("SELECT failed_login_count || ' ' || (locked_until IS NULL) FROM users"),
              "0 true");
}

TEST_F(LoginTest, AccountStateVisibleOnlyAfterCorrectPassword)
{
    createUser("new_user", "new@example.com", "unconfirmed");
    createUser("bad_user", "bad@example.com", "blocked");

    EXPECT_EQ(body(login("new_user", kPassword))["code"].asString(), "AUTH_EMAIL_NOT_CONFIRMED");
    EXPECT_EQ(body(login("new_user", "wrong"))["code"].asString(), "AUTH_INVALID_CREDENTIALS");
    EXPECT_EQ(body(login("bad_user", kPassword))["code"].asString(), "AUTH_ACCOUNT_BLOCKED");
    EXPECT_EQ(scalar("SELECT count(*) FROM sessions"), "0");
}

TEST_F(LoginTest, OidcOnlyAccountAndHugePasswordAreRejected)
{
    db->execSqlSync(
        "INSERT INTO users (username, email, birth_year, status) "
        "VALUES ('oidc_user', 'oidc@example.com', 2000, 'active')");
    EXPECT_EQ(body(login("oidc_user", "anything"))["code"].asString(), "AUTH_INVALID_CREDENTIALS");

    createUser("ivan", "ivan@example.com");
    EXPECT_EQ(login("ivan", std::string(200, 'a'))->statusCode(), k400BadRequest);
    EXPECT_EQ(scalar("SELECT failed_login_count FROM users WHERE username = 'ivan'"), "1");
}

TEST_F(LoginTest, SessionCheckFollowsUserState)
{
    createUser("ivan", "ivan@example.com");
    const std::string token = login("ivan", kPassword)->getCookie("dv_session").value();
    const auto withCookie = [&]
    {
        return run(getSessionHandler(
            db, request(Get, "/api/v1/auth/session", Json::Value(), {{"dv_session", token}})));
    };
    EXPECT_EQ(withCookie()->statusCode(), k200OK);

    db->execSqlSync("UPDATE users SET status = 'blocked'");  // блокировка лишает доступа сразу
    const auto blocked = withCookie();
    EXPECT_EQ(blocked->statusCode(), k401Unauthorized);
    EXPECT_EQ(body(blocked)["code"].asString(), "SESSION_EXPIRED");
    EXPECT_EQ(blocked->getCookie("dv_session").maxAge(), 0);  // cookie стирается
}

TEST_F(LoginTest, RequireSessionRejectsAndLogsAccessDenied)
{
    const auto guest = run(requireSession(db, request(Get, "/api/v1/search/text")));
    ASSERT_FALSE(guest);
    EXPECT_EQ(body(guest.error())["code"].asString(), "SESSION_REQUIRED");

    const auto stale = run(requireSession(
        db, request(Get, "/api/v1/search/text", Json::Value(), {{"dv_session", "stale"}})));
    ASSERT_FALSE(stale);
    EXPECT_EQ(body(stale.error())["code"].asString(), "SESSION_EXPIRED");

    EXPECT_EQ(scalar("SELECT count(*) || ' ' || max(details->>'path') FROM security_events "
                     "WHERE event_type = 'access_denied'"),
              "2 /api/v1/search/text");
}

TEST_F(LoginTest, LogoutEndsOnlyCurrentSession)
{
    const auto userId = createUser("ivan", "ivan@example.com");
    // вход с двух устройств - две сессии
    const std::string phone = login("ivan", kPassword)->getCookie("dv_session").value();
    const std::string laptop = login("ivan", kPassword)->getCookie("dv_session").value();
    const auto logout = [&](const std::string &token)
    {
        return run(logoutHandler(
            db, request(Post, "/api/v1/auth/logout", Json::Value(), {{"dv_session", token}})));
    };
    const auto session = [&](const std::string &token)
    {
        return run(getSessionHandler(
            db, request(Get, "/api/v1/auth/session", Json::Value(), {{"dv_session", token}})));
    };

    const auto response = logout(phone);
    EXPECT_EQ(response->statusCode(), k204NoContent);
    EXPECT_EQ(response->getCookie("dv_session").maxAge(), 0);  // cookie стёрта
    EXPECT_EQ(session(phone)->statusCode(), k401Unauthorized);
    EXPECT_EQ(session(laptop)->statusCode(), k200OK);  // другое устройство не затронуто
    EXPECT_EQ(scalar("SELECT count(*) FROM sessions"), "1");
    EXPECT_EQ(scalar("SELECT count(*) || ' ' || max(user_id) FROM security_events "
                     "WHERE event_type = 'logout'"),
              "1 " + std::to_string(userId));

    // повторный выход с той же cookie - 401: клиент считает его успешным выходом
    EXPECT_EQ(logout(phone)->statusCode(), k401Unauthorized);
}

TEST_F(LoginTest, ExpiredSessionIsRejected)
{
    createUser("ivan", "ivan@example.com");
    const std::string token = login("ivan", kPassword)->getCookie("dv_session").value();
    db->execSqlSync(
        "UPDATE sessions SET created_at = now() - interval '25 hours', "
        "expires_at = now() - interval '1 hour'");
    const auto user = run(requireSession(
        db, request(Get, "/api/v1/search/text", Json::Value(), {{"dv_session", token}})));
    EXPECT_FALSE(user);
}
