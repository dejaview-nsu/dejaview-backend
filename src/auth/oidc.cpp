#include "auth/oidc.hpp"

#include "auth/crypto.hpp"
#include "auth/http_common.hpp"
#include "auth/oidc_profile.hpp"
#include "auth/security_log.hpp"
#include "auth/session.hpp"
#include "auth/validation.hpp"
#include "error_response.hpp"

#include <drogon/drogon.h>

#include <format>
#include <optional>

using namespace drogon;

namespace
{
constexpr int kPendingSeconds = 30 * 60;  // на завершение входа (тег OIDC, допущение)

// Cookie dv_oidc: HttpOnly; Secure; SameSite=Lax; Path=/api/v1/auth; Max-Age=1800. Lax нужен,
// чтобы браузер прислал её на callback при переходе с сайта провайдера.
Cookie oidcCookie(const std::string &token)
{
    Cookie cookie("dv_oidc", token);
    cookie.setHttpOnly(true);
    cookie.setSecure(true);
    cookie.setSameSite(Cookie::SameSite::kLax);
    cookie.setPath("/api/v1/auth");
    cookie.setMaxAge(kPendingSeconds);
    return cookie;
}

Cookie clearOidcCookie()
{
    Cookie cookie("dv_oidc", "");
    cookie.setPath("/api/v1/auth");
    cookie.setMaxAge(0);
    return cookie;
}

// Приложение провайдера, если вход через него подключён
const OidcClient *clientFor(const OidcSettings &settings, std::string_view provider)
{
    if (provider == "yandex" && !settings.yandex.clientId.empty())
    {
        return &settings.yandex;
    }
    return nullptr;
}

std::string redirectUri(const OidcSettings &settings, std::string_view provider)
{
    return std::format("{}/api/v1/auth/oidc/{}/callback", settings.apiUrl, provider);
}

// 302 на страницу SPA с исходом входа. Код и описание ошибки провайдера наружу не передаются
// (#17094 п. 4.3) - только в лог
HttpResponsePtr toSpa(const OidcSettings &settings, std::string_view provider,
                      std::string_view result)
{
    return HttpResponse::newRedirectionResponse(
        std::format("{}/auth/oidc?provider={}&result={}", settings.appUrl,
                    utils::urlEncodeComponent(std::string(provider)), result));
}

// 410 AUTH_OIDC_EXPIRED: нет cookie или 30 минут на завершение входа истекли
HttpResponsePtr oidcExpired(std::string_view provider)
{
    const std::string message =
        provider.empty()
            ? "Не удалось выполнить вход. Попробуйте снова или используйте другой способ"
            : std::format(
                  "Не удалось выполнить вход через {}. Попробуйте снова или используйте "
                  "другой способ",
                  providerTitle(provider));
    auto response = errorResponse(k410Gone, "AUTH_OIDC_EXPIRED", message);
    response->addCookie(clearOidcCookie());
    return response;
}

Json::Value oidcDetails(std::string_view provider)
{
    Json::Value details;
    details["method"] = "oidc";
    details["provider"] = std::string(provider);
    return details;
}

// Обмен кода на токен и профиль у Яндекс ID (yandex.ru/dev/id). nullopt - провайдер ответил
// ошибкой, подробности в логе. Недоступность сети - исключение.
Task<std::optional<OidcProfile>> fetchYandexProfile(OidcClient client, std::string code,
                                                    std::string codeVerifier)
{
    // ponytail: новый HTTP-клиент и TLS-рукопожатие на каждый вход; при нагрузке - общий клиент
    auto oauth = HttpClient::newHttpClient("https://oauth.yandex.ru");
    auto tokenRequest = HttpRequest::newHttpFormPostRequest();
    tokenRequest->setPath("/token");
    tokenRequest->setParameter("grant_type", "authorization_code");
    tokenRequest->setParameter("code", code);
    tokenRequest->setParameter("client_id", client.clientId);
    tokenRequest->setParameter("client_secret", client.clientSecret);
    tokenRequest->setParameter("code_verifier", codeVerifier);
    const auto tokenResponse = co_await oauth->sendRequestCoro(tokenRequest, 10);
    const auto &token = tokenResponse->getJsonObject();
    const std::string accessToken = token && token->get("access_token", Json::Value()).isString()
                                        ? token->get("access_token", "").asString()
                                        : "";
    if (tokenResponse->statusCode() != k200OK || accessToken.empty())
    {
        // в ответе с ошибкой токена нет, только error и error_description - можно в лог
        LOG_WARN << "Яндекс: обмен кода на токен: " << tokenResponse->statusCode() << ' '
                 << tokenResponse->body();
        co_return std::nullopt;
    }

    auto login = HttpClient::newHttpClient("https://login.yandex.ru");
    auto infoRequest = HttpRequest::newHttpRequest();
    infoRequest->setPath("/info");
    infoRequest->setParameter("format", "json");
    infoRequest->addHeader("Authorization", "OAuth " + accessToken);
    const auto infoResponse = co_await login->sendRequestCoro(infoRequest, 10);
    const auto &info = infoResponse->getJsonObject();
    if (infoResponse->statusCode() != k200OK || !info)
    {
        LOG_WARN << "Яндекс: профиль: " << infoResponse->statusCode();
        co_return std::nullopt;
    }
    co_return parseYandexProfile(*info);
}
}  // namespace

Task<HttpResponsePtr> oidcStartHandler(orm::DbClientPtr db, OidcSettings settings,
                                       HttpRequestPtr req, std::string provider)
{
    const OidcClient *client = clientFor(settings, provider);
    if (!client)
    {
        LOG_WARN << "OIDC: провайдер '" << provider << "' не подключён";
        co_return toSpa(settings, provider, "error");
    }

    // В cookie - случайный токен, в БД - его хеш, state (против подмены ответа провайдера) и
    // code_verifier (PKCE). Провайдеру уходит только хеш verifier - code_challenge
    const std::string token = newToken();
    const std::string state = newToken();
    const std::string codeVerifier = newToken();
    co_await db->execSqlCoro(
        "INSERT INTO oidc_pending (token_hash, provider, state, code_verifier, expires_at) "
        "VALUES (decode($1, 'hex'), $2, $3, $4, now() + $5::int * interval '1 second')",
        tokenHash(token), provider, state, codeVerifier, kPendingSeconds);

    // Email и имя профиля (#17148 п. 3.2 шаг 7): login:email и login:info
    auto response = HttpResponse::newRedirectionResponse(std::format(
        "https://oauth.yandex.ru/authorize?response_type=code&client_id={}&redirect_uri={}"
        "&scope={}&state={}&code_challenge={}&code_challenge_method=S256",
        utils::urlEncodeComponent(client->clientId),
        utils::urlEncodeComponent(redirectUri(settings, provider)),
        utils::urlEncodeComponent("login:email login:info"), state, pkceChallenge(codeVerifier)));
    response->addCookie(oidcCookie(token));
    co_return response;
}

Task<HttpResponsePtr> oidcCallbackHandler(orm::DbClientPtr db, OidcSettings settings,
                                          HttpRequestPtr req, std::string provider)
{
    const auto fail = [&](std::string_view why)
    {
        LOG_WARN << "OIDC " << provider << ": " << why;
        auto response = toSpa(settings, provider, "error");
        response->addCookie(clearOidcCookie());
        return response;
    };

    const OidcClient *client = clientFor(settings, provider);
    const std::string cookie = req->getCookie("dv_oidc");
    if (!client || cookie.empty())
    {
        co_return fail("провайдер не подключён или нет cookie dv_oidc");
    }
    // Пользователь отказал в доступе (error=access_denied) или ошибка у провайдера (#17148 п. 3.4)
    if (const std::string &error = req->getParameter("error"); !error.empty())
    {
        co_return fail("провайдер вернул error=" + error);
    }

    // state из ответа должен совпасть с тем, что ушёл из этого браузера. Иначе ссылку с чужим
    // кодом подсунули пользователю, и он вошёл бы в аккаунт злоумышленника (login CSRF)
    const auto pending = co_await db->execSqlCoro(
        "SELECT state, code_verifier FROM oidc_pending WHERE token_hash = decode($1, 'hex') "
        "AND provider = $2 AND subject IS NULL AND expires_at > now()",
        tokenHash(cookie), provider);
    if (pending.empty() || pending[0]["state"].as<std::string>() != req->getParameter("state"))
    {
        co_return fail("нет незавершённого входа или не совпал state");
    }

    std::optional<OidcProfile> profile;
    try
    {
        profile = co_await fetchYandexProfile(*client, req->getParameter("code"),
                                              pending[0]["code_verifier"].as<std::string>());
    }
    catch (const std::exception &e)
    {
        co_return fail(std::string("провайдер недоступен: ") + e.what());
    }
    if (!profile)
    {
        co_return fail("профиль не получен");
    }
    // Email, не подтверждённому провайдером, не доверяем: иначе адрес, заведённый у провайдера на
    // чужое имя, дал бы доступ к чужой учётной записи у нас
    if (profile->email.empty() || !profile->emailVerified)
    {
        co_return fail("провайдер не дал подтверждённый email");
    }

    // 1. Учётная запись, привязанная к этому аккаунту провайдера (#17149 п. 3.1)
    const auto linked = co_await db->execSqlCoro(
        "SELECT user_id, status FROM oidc_accounts JOIN users USING (user_id) "
        "WHERE provider = $1 AND subject = $2",
        provider, profile->subject);
    if (!linked.empty())
    {
        co_await db->execSqlCoro("DELETE FROM oidc_pending WHERE token_hash = decode($1, 'hex')",
                                 tokenHash(cookie));
        const auto userId = linked[0]["user_id"].as<std::int64_t>();
        const std::string status = linked[0]["status"].as<std::string>();
        if (status == "blocked")
        {
            Json::Value details = oidcDetails(provider);
            details["reason"] = "account_blocked";
            co_await logSecurityEvent(db, "login_failure", userId, req, details);
            auto response = toSpa(settings, provider, "account_blocked");
            response->addCookie(clearOidcCookie());
            co_return response;
        }
        if (status != "active")
        {
            co_return fail("привязанная учётная запись не активна");
        }
        co_await logSecurityEvent(db, "login_success", userId, req, oidcDetails(provider));
        const NewSession session = co_await createSession(db, req, userId);
        auto response = toSpa(settings, provider, "success");
        response->addCookie(sessionCookie(session.token));
        response->addCookie(clearOidcCookie());
        co_return response;
    }

    // 2. Учётная запись с этим email есть - привязать после входа с паролем (link_required).
    // 3. Нет - регистрация (registration_required). Учётная запись появится только на шаге
    //    «Завершение регистрации», с годом рождения; до этого данные профиля ждут в oidc_pending
    //    ещё 30 минут (решение архитектора 27.09, contracts/db-schema.md)
    const auto byEmail = co_await db->execSqlCoro(
        "SELECT user_id FROM users WHERE lower(email) = lower($1)", profile->email);
    const auto linkUserId =
        byEmail.empty() ? std::nullopt : std::optional(byEmail[0]["user_id"].as<std::int64_t>());
    co_await db->execSqlCoro(
        "UPDATE oidc_pending SET subject = $2, email = $3, display_name = NULLIF($4, ''), "
        "link_user_id = $5, created_at = now(), "
        "expires_at = now() + $6::int * interval '1 second' "
        "WHERE token_hash = decode($1, 'hex')",
        tokenHash(cookie), profile->subject, profile->email, profile->displayName, linkUserId,
        kPendingSeconds);
    auto response =
        toSpa(settings, provider, linkUserId ? "link_required" : "registration_required");
    response->addCookie(oidcCookie(cookie));  // ещё 30 минут
    co_return response;
}

Task<HttpResponsePtr> oidcPendingHandler(orm::DbClientPtr db, HttpRequestPtr req)
{
    const std::string cookie = req->getCookie("dv_oidc");
    if (cookie.empty())
    {
        co_return oidcExpired({});
    }
    const auto pending = co_await db->execSqlCoro(
        "SELECT provider, email, display_name, link_user_id IS NOT NULL AS link, "
        "expires_at > now() AS alive FROM oidc_pending "
        "WHERE token_hash = decode($1, 'hex') AND subject IS NOT NULL",
        tokenHash(cookie));
    if (pending.empty())
    {
        co_return oidcExpired({});
    }
    const auto &row = pending[0];
    const std::string provider = row["provider"].as<std::string>();
    if (!row["alive"].as<bool>())
    {
        co_return oidcExpired(provider);
    }

    Json::Value body;
    body["provider"] = provider;
    body["email"] = row["email"].as<std::string>();
    if (row["link"].as<bool>())
    {
        body["kind"] = "link";
        body["suggested_username"] = Json::Value();
    }
    else
    {
        // Имя из профиля, а если занято - со случайным числовым суффиксом (#17148 п. 3.2 шаг 7).
        // Окончательно уникальность проверит POST /auth/oidc/complete
        std::string username = suggestUsername(
            row["display_name"].isNull() ? "" : row["display_name"].as<std::string>(),
            row["email"].as<std::string>());
        const auto taken = co_await db->execSqlCoro(
            "SELECT EXISTS (SELECT 1 FROM users WHERE lower(username) = lower($1)) AS taken",
            username);
        if (taken[0]["taken"].as<bool>())
        {
            username = username.substr(0, 26) + std::to_string(1000 + randomBelow(9000));
        }
        body["kind"] = "registration";
        body["suggested_username"] = username;
    }
    co_return HttpResponse::newHttpJsonResponse(body);
}

Task<HttpResponsePtr> oidcCompleteHandler(orm::DbClientPtr db, HttpRequestPtr req)
{
    const std::string cookie = req->getCookie("dv_oidc");
    if (cookie.empty())
    {
        co_return oidcExpired({});
    }
    const auto pending = co_await db->execSqlCoro(
        "SELECT provider, expires_at > now() AS alive FROM oidc_pending "
        "WHERE token_hash = decode($1, 'hex') AND subject IS NOT NULL AND link_user_id IS NULL",
        tokenHash(cookie));
    if (pending.empty() || !pending[0]["alive"].as<bool>())
    {
        co_return oidcExpired(pending.empty() ? "" : pending[0]["provider"].as<std::string>());
    }
    const std::string provider = pending[0]["provider"].as<std::string>();

    const Json::Value *json = jsonBody(req);
    if (!json)
    {
        co_return malformedBody();
    }
    const std::string username = stringField(*json, "username");
    const int birthYear = birthYearField(*json);
    // Проверки как при регистрации (#17148 п. 3.2 шаги 9-10)
    if (const auto error = validateUsername(username).or_else(
            [&] { return validateBirthYear(birthYear, currentYear()); }))
    {
        co_return fieldErrorResponse(*error);
    }

    // Учётная запись «Активна» без подтверждения email (адрес подтвердил провайдер), привязка
    // провайдера и удаление oidc_pending - одним запросом, то есть атомарно
    std::optional<orm::Result> created;
    try
    {
        created = co_await db->execSqlCoro(
            "WITH p AS (DELETE FROM oidc_pending WHERE token_hash = decode($1, 'hex') "
            "AND subject IS NOT NULL AND link_user_id IS NULL AND expires_at > now() "
            "RETURNING provider, subject, email), "
            "u AS (INSERT INTO users (username, email, birth_year, status) "
            "SELECT $2, email, $3::smallint, 'active' FROM p RETURNING user_id), "
            "a AS (INSERT INTO oidc_accounts (provider, subject, user_id) "
            "SELECT p.provider, p.subject, u.user_id FROM p, u) "
            "SELECT user_id FROM u",
            tokenHash(cookie), username, std::to_string(birthYear));
    }
    catch (const orm::DrogonDbException &e)
    {
        // Имя или email заняли, пока пользователь был на странице: сработал уникальный индекс,
        // запрос откатился целиком, oidc_pending на месте - можно выбрать другое имя
        const std::string message = e.base().what();
        if (message.contains("users_username_key"))
        {
            co_return errorResponse(k409Conflict, "AUTH_USERNAME_TAKEN",
                                    "Это имя пользователя уже занято", "username");
        }
        if (message.contains("users_email_key"))
        {
            co_return errorResponse(k409Conflict, "AUTH_EMAIL_TAKEN",
                                    "Пользователь с таким email уже существует");
        }
        throw;
    }
    if (created->empty())
    {
        co_return oidcExpired(provider);
    }

    const auto userId = (*created)[0]["user_id"].as<std::int64_t>();
    co_await logSecurityEvent(db, "login_success", userId, req, oidcDetails(provider));
    auto response = co_await startSession(db, req, userId, k201Created);
    response->addCookie(clearOidcCookie());
    co_return response;
}

Task<> finishOidcLink(orm::DbClientPtr db, HttpRequestPtr req, std::int64_t userId,
                      HttpResponsePtr response)
{
    const std::string cookie = req->getCookie("dv_oidc");
    if (cookie.empty())
    {
        co_return;
    }
    // Запись удаляется в любом случае, привязка - только к той учётной записи, что нашлась по
    // email провайдера. ON CONFLICT: провайдер уже привязан - ничего не делать
    co_await db->execSqlCoro(
        "WITH p AS (DELETE FROM oidc_pending WHERE token_hash = decode($1, 'hex') "
        "RETURNING provider, subject, link_user_id, expires_at) "
        "INSERT INTO oidc_accounts (provider, subject, user_id) "
        "SELECT provider, subject, link_user_id FROM p "
        "WHERE link_user_id = $2 AND expires_at > now() ON CONFLICT DO NOTHING",
        tokenHash(cookie), userId);
    response->addCookie(clearOidcCookie());
}
