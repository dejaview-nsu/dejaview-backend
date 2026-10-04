#include "auth/oidc.hpp"
#include "auth/login.hpp"
#include "integration/db_test.hpp"

using namespace drogon;

// Вход через провайдера целиком: начало, callback, обмен кода и профиль (провайдер - фейковый
// сервер), исходы входа, незавершённая регистрация, привязка при входе с паролем.
class OidcTest : public DbTest
{
  protected:
    OidcSettings settings{.appUrl = "https://dejaview.ru",
                          .apiUrl = "https://dejaview.ru",
                          .yandex = {.clientId = "yandex-id", .clientSecret = "secret"},
                          .google = {},
                          .vk = {}};

    void SetUp() override
    {
        DbTest::SetUp();
        settings.fakeProviderUrl = fakeUrl;
    }

    // Вход целиком: начало, затем возврат от провайдера с кодом и верным state. Ответы
    // провайдера тест задаёт заранее (fakeReply)
    HttpResponsePtr signIn(const std::string &provider)
    {
        const auto start = run(oidcStartHandler(db, settings, request(Get, "/start"), provider));
        const std::string location = start->getHeader("Location");
        const auto from = location.find("&state=") + 7;
        auto req = request(Get, "/callback", Json::Value(),
                           {{"dv_oidc", start->getCookie("dv_oidc").value()}});
        req->setParameter("code", "code-1");
        req->setParameter("state", location.substr(from, location.find('&', from) - from));
        req->setParameter("device_id", "device-1");
        return run(oidcCallbackHandler(db, settings, req, provider));
    }

    // Незавершённый вход после callback, как его оставляет oidcCallbackHandler. linkUserId -
    // исход link_required. Возвращает значение cookie dv_oidc
    std::string pending(const std::string &email, const std::string &displayName,
                        std::optional<std::int64_t> linkUserId = std::nullopt)
    {
        db->execSqlSync(
            "INSERT INTO oidc_pending (token_hash, provider, state, code_verifier, "
            "subject, email, display_name, link_user_id, expires_at) "
            "VALUES (sha256('pending'), 'yandex', 's', 'v', 'sub-1', $1, $2, $3, "
            "now() + interval '30 minutes')",
            email, displayName, linkUserId);
        return "pending";  // токен, SHA-256 которого лежит в БД
    }

    // Возврат от провайдера, когда профиль уже получен: начало входа настоящее, профиль подставной
    HttpResponsePtr finish(const std::string &email, bool emailVerified = true)
    {
        const auto start = run(oidcStartHandler(db, settings, request(Get, "/start"), "yandex"));
        const auto req = request(Get, "/callback", Json::Value(),
                                 {{"dv_oidc", start->getCookie("dv_oidc").value()}});
        return run(oidcFinishWithProfile(db, settings, req, "yandex",
                                         {.subject = "sub-42",
                                          .email = email,
                                          .emailVerified = emailVerified,
                                          .displayName = "Иван Петров"}));
    }

    static std::string result(const HttpResponsePtr &response)
    {
        const std::string location = response->getHeader("Location");
        return location.substr(location.find("result=") + 7);
    }

    HttpResponsePtr getPending(const std::string &cookie)
    {
        return run(oidcPendingHandler(
            db, request(Get, "/api/v1/auth/oidc/pending", Json::Value(), {{"dv_oidc", cookie}})));
    }

    HttpResponsePtr complete(const std::string &cookie, const std::string &username)
    {
        Json::Value json;
        json["username"] = username;
        json["birth_year"] = 1998;
        return run(oidcCompleteHandler(
            db, request(Post, "/api/v1/auth/oidc/complete", json, {{"dv_oidc", cookie}})));
    }
};

TEST_F(OidcTest, StartRedirectsToProviderWithStateAndPkce)
{
    const auto response = run(oidcStartHandler(db, settings, request(Get, "/start"), "yandex"));
    EXPECT_EQ(response->statusCode(), k302Found);
    const std::string location = response->getHeader("Location");
    EXPECT_TRUE(location.starts_with("https://oauth.yandex.ru/authorize?")) << location;
    for (const char *part : {"client_id=yandex-id", "code_challenge_method=S256", "state=",
                             "redirect_uri=https%3A%2F%2Fdejaview.ru%2Fapi%2Fv1%2Fauth%2Foidc%"
                             "2Fyandex%2Fcallback"})
    {
        EXPECT_NE(location.find(part), std::string::npos) << part;
    }
    EXPECT_FALSE(response->getCookie("dv_oidc").value().empty());
    EXPECT_EQ(scalar("SELECT expires_at - created_at FROM oidc_pending"), "00:30:00");
}

TEST_F(OidcTest, UnknownOrDisabledProviderEndsWithError)
{
    for (const char *provider : {"facebook", "google"})  // google без ключей - выключен
    {
        const auto response = run(oidcStartHandler(db, settings, request(Get, "/start"), provider));
        EXPECT_NE(response->getHeader("Location").find("result=error"), std::string::npos);
    }
    EXPECT_EQ(scalar("SELECT count(*) FROM oidc_pending"), "0");
}

TEST_F(OidcTest, CallbackRejectsDenialMissingCookieAndWrongState)
{
    const auto start = run(oidcStartHandler(db, settings, request(Get, "/start"), "yandex"));
    const std::string cookie = start->getCookie("dv_oidc").value();
    const auto callback = [&](const std::string &query, const std::string &dvOidc)
    {
        auto req =
            request(Get, "/callback", Json::Value(),
                    dvOidc.empty() ? std::map<std::string, std::string>{}
                                   : std::map<std::string, std::string>{{"dv_oidc", dvOidc}});
        req->setParameter("state", query);
        if (query == "denied")
        {
            req->setParameter("error", "access_denied");
        }
        return run(oidcCallbackHandler(db, settings, req, "yandex"))->getHeader("Location");
    };
    EXPECT_NE(callback("denied", cookie).find("result=error"), std::string::npos);
    EXPECT_NE(callback("any", "").find("result=error"), std::string::npos);
    EXPECT_NE(callback("wrong-state", cookie).find("result=error"), std::string::npos);
}

TEST_F(OidcTest, PendingSuggestsUsernameWithSuffixWhenTaken)
{
    const std::string cookie = pending("ivan@yandex.ru", "Иван Петров");
    auto json = body(getPending(cookie));
    EXPECT_EQ(json["kind"].asString(), "registration");
    EXPECT_EQ(json["email"].asString(), "ivan@yandex.ru");
    EXPECT_EQ(json["suggested_username"].asString(), "ivan_petrov");

    createUser("ivan_petrov", "other@example.com");
    json = body(getPending(cookie));
    const std::string suggested = json["suggested_username"].asString();
    EXPECT_TRUE(suggested.starts_with("ivan_petrov")) << suggested;
    EXPECT_EQ(suggested.size(), std::string("ivan_petrov").size() + 4);  // 4 цифры
}

TEST_F(OidcTest, PendingExpiredOrMissing)
{
    EXPECT_EQ(getPending("")->statusCode(), k410Gone);
    const std::string cookie = pending("ivan@yandex.ru", "Ivan");
    db->execSqlSync(
        "UPDATE oidc_pending SET created_at = now() - interval '31 minutes', "
        "expires_at = now() - interval '1 minute'");
    const auto expired = getPending(cookie);
    EXPECT_EQ(expired->statusCode(), k410Gone);
    EXPECT_EQ(body(expired)["message"].asString(),
              "Не удалось выполнить вход через Яндекс. Попробуйте снова или используйте "
              "другой способ");
}

TEST_F(OidcTest, CompleteCreatesActiveLinkedUser)
{
    const std::string cookie = pending("ivan@yandex.ru", "Ivan");
    const auto response = complete(cookie, "ivan");

    EXPECT_EQ(response->statusCode(), k201Created);
    EXPECT_FALSE(response->getCookie("dv_session").value().empty());
    EXPECT_EQ(response->getCookie("dv_oidc").maxAge(), 0);
    EXPECT_EQ(scalar("SELECT status || ' ' || (password_hash IS NULL) FROM users"), "active true");
    EXPECT_EQ(scalar("SELECT provider || ' ' || subject FROM oidc_accounts"), "yandex sub-1");
    EXPECT_EQ(scalar("SELECT count(*) FROM oidc_pending"), "0");
    EXPECT_EQ(complete(cookie, "ivan2")->statusCode(), k410Gone);  // второй раз - уже нечего
}

TEST_F(OidcTest, CompleteValidatesAndKeepsPendingOnConflict)
{
    const std::string cookie = pending("ivan@yandex.ru", "Ivan");
    EXPECT_EQ(body(complete(cookie, "ab"))["field"].asString(), "username");

    createUser("taken", "taken@example.com");
    const auto taken = complete(cookie, "TAKEN");
    EXPECT_EQ(taken->statusCode(), k409Conflict);
    EXPECT_EQ(body(taken)["code"].asString(), "AUTH_USERNAME_TAKEN");
    // запрос откатился целиком: можно выбрать другое имя
    EXPECT_EQ(complete(cookie, "ivan")->statusCode(), k201Created);
}

TEST_F(OidcTest, PasswordLoginLinksProviderToSameAccountOnly)
{
    const auto ivan = createUser("ivan", "ivan@yandex.ru");
    createUser("petr", "petr@example.com");
    const auto login = [&](const std::string &name, const std::string &cookie)
    {
        Json::Value json;
        json["login"] = name;
        json["password"] = kPassword;
        return run(
            loginHandler(db, {}, request(Post, "/api/v1/auth/login", json, {{"dv_oidc", cookie}})));
    };

    // вход в другую учётную запись привязку отменяет
    std::string cookie = pending("ivan@yandex.ru", "Ivan", ivan);
    EXPECT_EQ(body(getPending(cookie))["kind"].asString(), "link");
    EXPECT_EQ(login("petr", cookie)->getCookie("dv_oidc").maxAge(), 0);
    EXPECT_EQ(scalar("SELECT count(*) FROM oidc_accounts"), "0");

    // вход в ту самую - провайдер привязан
    cookie = pending("ivan@yandex.ru", "Ivan", ivan);
    EXPECT_EQ(login("ivan", cookie)->statusCode(), k200OK);
    EXPECT_EQ(scalar("SELECT username FROM oidc_accounts JOIN users USING (user_id)"), "ivan");
}

TEST_F(OidcTest, NewEmailNeedsRegistration)
{
    const auto response = finish("ivan@yandex.ru");
    EXPECT_EQ(result(response), "registration_required");
    EXPECT_EQ(response->getCookie("dv_oidc").maxAge(), 1800);  // ещё 30 минут на завершение
    EXPECT_EQ(scalar("SELECT subject || ' ' || email || ' ' || display_name || ' ' || "
                     "(link_user_id IS NULL) FROM oidc_pending"),
              "sub-42 ivan@yandex.ru Иван Петров true");
    EXPECT_EQ(scalar("SELECT count(*) FROM users"), "0");  // учётная запись - только на complete
}

TEST_F(OidcTest, KnownEmailNeedsLinkWithPassword)
{
    const auto ivan = createUser("ivan", "Ivan@Yandex.ru");
    EXPECT_EQ(result(finish("ivan@yandex.ru")), "link_required");
    EXPECT_EQ(scalar("SELECT link_user_id FROM oidc_pending"), std::to_string(ivan));
}

TEST_F(OidcTest, LinkedAccountSignsInDirectly)
{
    const auto ivan = createUser("ivan", "ivan@example.com");
    db->execSqlSync(
        "INSERT INTO oidc_accounts (provider, subject, user_id) VALUES ('yandex', 'sub-42', " +
        std::to_string(ivan) + ")");
    const auto response = finish("other@yandex.ru");  // email у провайдера мог смениться: важен sub

    EXPECT_EQ(result(response), "success");
    EXPECT_FALSE(response->getCookie("dv_session").value().empty());
    EXPECT_EQ(response->getCookie("dv_oidc").maxAge(), 0);
    EXPECT_EQ(scalar("SELECT count(*) FROM oidc_pending"), "0");
    EXPECT_EQ(
        scalar("SELECT details->>'method' FROM security_events WHERE event_type = 'login_success'"),
        "oidc");
}

TEST_F(OidcTest, LinkedBlockedAccountIsRejected)
{
    const auto ivan = createUser("ivan", "ivan@example.com", "blocked");
    db->execSqlSync(
        "INSERT INTO oidc_accounts (provider, subject, user_id) VALUES ('yandex', 'sub-42', " +
        std::to_string(ivan) + ")");
    EXPECT_EQ(result(finish("ivan@example.com")), "account_blocked");
    EXPECT_EQ(scalar("SELECT count(*) FROM sessions"), "0");
    EXPECT_EQ(
        scalar("SELECT details->>'reason' FROM security_events WHERE event_type = 'login_failure'"),
        "account_blocked");
}

TEST_F(OidcTest, UnverifiedOrMissingEmailIsRejected)
{
    createUser("ivan", "ivan@example.com");
    // иначе чужой адрес, заведённый у провайдера, привёл бы к чужой учётной записи
    EXPECT_EQ(result(finish("ivan@example.com", false)), "error");
    EXPECT_EQ(result(finish("")), "error");
    EXPECT_EQ(scalar("SELECT count(*) FROM sessions"), "0");
}

TEST_F(OidcTest, YandexExchangesCodeAndReadsProfile)
{
    fakeReply("/token", k200OK, R"({"access_token": "yandex-token"})");
    fakeReply("/info", k200OK,
              R"({"id": "y-1", "display_name": "Иван", "default_email": "ivan@yandex.ru"})");
    EXPECT_EQ(result(signIn("yandex")), "registration_required");
    EXPECT_EQ(scalar("SELECT subject || ' ' || email FROM oidc_pending"), "y-1 ivan@yandex.ru");

    const auto token = fakeReceived("/token");
    ASSERT_TRUE(token);
    EXPECT_EQ(token->getParameter("grant_type"), "authorization_code");
    EXPECT_EQ(token->getParameter("code"), "code-1");
    EXPECT_EQ(token->getParameter("client_id"), "yandex-id");
    EXPECT_EQ(token->getParameter("client_secret"), "secret");
    // PKCE: провайдер сверит verifier с хешем, полученным на старте
    EXPECT_EQ(token->getParameter("code_verifier"),
              scalar("SELECT code_verifier FROM oidc_pending"));
    EXPECT_EQ(fakeReceived("/info")->getHeader("Authorization"), "OAuth yandex-token");
}

TEST_F(OidcTest, GoogleSendsRedirectUriAndBearerToken)
{
    settings.google = {.clientId = "google-id", .clientSecret = "google-secret"};
    fakeReply("/token", k200OK, R"({"access_token": "google-token"})");
    fakeReply(
        "/v1/userinfo", k200OK,
        R"({"sub": "g-1", "email": "ivan@gmail.com", "email_verified": true, "name": "Ivan"})");
    EXPECT_EQ(result(signIn("google")), "registration_required");
    // тот же redirect_uri, что ушёл на старте: иначе Google код не обменяет
    EXPECT_EQ(fakeReceived("/token")->getParameter("redirect_uri"),
              "https://dejaview.ru/api/v1/auth/oidc/google/callback");
    EXPECT_EQ(fakeReceived("/v1/userinfo")->getHeader("Authorization"), "Bearer google-token");
}

TEST_F(OidcTest, VkSendsDeviceIdAndStateWithoutSecret)
{
    settings.vk = {.clientId = "vk-id"};
    fakeReply("/oauth2/auth", k200OK, R"({"access_token": "vk-token"})");
    fakeReply("/oauth2/user_info", k200OK,
              R"({"user": {"user_id": 777, "first_name": "Иван", "email": "ivan@vk.com"}})");
    EXPECT_EQ(result(signIn("vk")), "registration_required");

    const auto token = fakeReceived("/oauth2/auth");
    ASSERT_TRUE(token);
    EXPECT_EQ(token->getParameter("device_id"), "device-1");
    EXPECT_EQ(token->getParameter("state"), scalar("SELECT state FROM oidc_pending"));
    EXPECT_EQ(token->getParameter("client_secret"), "");  // VK ID - без секрета, только PKCE
    EXPECT_EQ(fakeReceived("/oauth2/user_info")->getParameter("access_token"), "vk-token");
}

TEST_F(OidcTest, ProviderFailureEndsWithError)
{
    // код не обменялся: истёк или уже использован
    fakeReply("/token", k400BadRequest, R"({"error": "invalid_grant"})");
    const auto badCode = signIn("yandex");
    EXPECT_EQ(result(badCode), "error");
    EXPECT_EQ(badCode->getCookie("dv_oidc").maxAge(), 0);

    // токен выдан, профиль - нет
    fakeReply("/token", k200OK, R"({"access_token": "yandex-token"})");
    fakeReply("/info", k401Unauthorized, "{}");
    EXPECT_EQ(result(signIn("yandex")), "error");

    settings.fakeProviderUrl = "http://127.0.0.1:1";  // провайдер недоступен
    EXPECT_EQ(result(signIn("yandex")), "error");
    EXPECT_EQ(scalar("SELECT count(*) FROM oidc_pending WHERE subject IS NOT NULL"), "0");
}
