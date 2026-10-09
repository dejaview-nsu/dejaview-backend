#include "auth/login.hpp"
#include "auth/oidc.hpp"
#include "auth/password.hpp"
#include "auth/registration.hpp"
#include "auth/session.hpp"
#include "background.hpp"
#include "config.hpp"
#include "error_response.hpp"
#include "health.hpp"

#include <drogon/drogon.h>

#include <cstdio>
#include <exception>

using namespace drogon;

int main()
{
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    Config config;
    try
    {
        config = loadConfig();
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "Ошибка конфигурации: " << e.what();
        return 1;
    }

    // Один клиент с пулом соединений на всё приложение. Если БД недоступна, запросы падают с
    // ошибкой, а клиент сам переподключается.
    auto db = orm::DbClient::newPgClient(config.databaseUrl, 4);
    db->setTimeout(2.0);

    if (config.smartCaptchaServerKey.empty())
    {
        LOG_WARN << "SMARTCAPTCHA_SERVER_KEY не задан: CAPTCHA при входе отключена. "
                    "Допустимо только локально";
    }

    const OidcSettings oidc{.appUrl = config.appUrl,
                            .apiUrl = config.apiUrl,
                            .yandex = config.yandex,
                            .google = config.google,
                            .vk = config.vk};
    if (const std::string providers = enabledOidcProviders(oidc); !providers.empty())
    {
        LOG_INFO << "OIDC: подключены " << providers << ", redirect_uri " << config.apiUrl
                 << "/api/v1/auth/oidc/<провайдер>/callback";
    }

    // Письма из очереди и очистка устаревшего
    const auto backgroundJobs = startBackgroundJobs(db, config.smtp);

    LOG_INFO << "dejaview-backend слушает порт " << config.port;

    app()
        .addListener("0.0.0.0", config.port)
        .setThreadNum(0)
        .enableServerHeader(false)
        // русский текст ошибок в JSON как есть, а не Во...: так его видно в curl и логах
        .setUnicodeEscapingInJson(false)
        // Только API. Иначе Drogon отдаёт файлы из рабочей папки (в контейнере это /), в том числе
        // загруженные пользователями: корень статики, которого нет, даёт 404 на любой другой путь
        .setDocumentRoot("/nonexistent")
        .setUploadPath("/tmp/dejaview-uploads")
        .setExceptionHandler(exceptionHandler)
        // 404, 405 и другие ошибки фреймворка - JSON Error вместо HTML-страницы с версией Drogon
        // Сессия из cookie определяется один раз до обработчика (requireSession берёт её готовой),
        // недействительная cookie стирается после - для всех маршрутов сразу
        .registerPreHandlingAdvice(
            [db](const HttpRequestPtr &req, AdviceCallback &&, AdviceChainCallback &&next)
            {
                if (req->getCookie("dv_session").empty())
                {
                    next();
                    return;
                }
                async_run(
                    [db, req, next = std::move(next)]() -> Task<>
                    {
                        try
                        {
                            co_await resolveSession(db, req);
                        }
                        catch (const std::exception &e)
                        {
                            // БД недоступна: обработчик попробует сам и ответит 500, если нужно
                            LOG_WARN << "сессия не определена: " << e.what();
                        }
                        next();
                    });
            })
        .registerPostHandlingAdvice(clearStaleSessionCookie)
        .setCustomErrorHandler([](HttpStatusCode status) { return frameworkError(status); })
        .registerHandler("/health", [db](HttpRequestPtr) { return healthHandler(db); }, {Get})
        .registerHandler("/api/v1/auth/session",
                         [db](HttpRequestPtr req) { return getSessionHandler(db, req); }, {Get})
        .registerHandler("/api/v1/auth/register", [db, appUrl = config.appUrl](HttpRequestPtr req)
                         { return registerHandler(db, appUrl, req); }, {Post})
        .registerHandler("/api/v1/auth/confirm-email",
                         [db](HttpRequestPtr req) { return confirmEmailHandler(db, req); }, {Post})
        .registerHandler("/api/v1/auth/login",
                         [db, captcha = CaptchaSettings{.serverKey = config.smartCaptchaServerKey}](
                             HttpRequestPtr req) { return loginHandler(db, captcha, req); },
                         {Post})
        .registerHandler("/api/v1/auth/logout",
                         [db](HttpRequestPtr req) { return logoutHandler(db, req); }, {Post})
        .registerHandler("/api/v1/auth/resend-confirmation",
                         [db, appUrl = config.appUrl](HttpRequestPtr req)
                         { return resendConfirmationHandler(db, appUrl, req); }, {Post})
        .registerHandler("/api/v1/auth/oidc/{provider}/start",
                         [db, oidc](HttpRequestPtr req, std::string provider)
                         { return oidcStartHandler(db, oidc, req, provider); }, {Get})
        .registerHandler("/api/v1/auth/oidc/{provider}/callback",
                         [db, oidc](HttpRequestPtr req, std::string provider)
                         { return oidcCallbackHandler(db, oidc, req, provider); }, {Get})
        .registerHandler("/api/v1/auth/oidc/pending",
                         [db](HttpRequestPtr req) { return oidcPendingHandler(db, req); }, {Get})
        .registerHandler("/api/v1/auth/oidc/complete",
                         [db](HttpRequestPtr req) { return oidcCompleteHandler(db, req); }, {Post})
        .registerHandler("/api/v1/auth/password-reset/request",
                         [db, appUrl = config.appUrl](HttpRequestPtr req)
                         { return requestPasswordResetHandler(db, appUrl, req); }, {Post})
        .registerHandler("/api/v1/auth/password-reset/check", [db](HttpRequestPtr req)
                         { return checkPasswordResetHandler(db, req); }, {Post})
        .registerHandler("/api/v1/auth/password-reset/complete",
                         [db, appUrl = config.appUrl](HttpRequestPtr req)
                         { return completePasswordResetHandler(db, appUrl, req); }, {Post})
        .registerHandler("/api/v1/users/me/password",
                         [db, appUrl = config.appUrl](HttpRequestPtr req)
                         { return changePasswordHandler(db, appUrl, req); }, {Put})
        .run();
}
