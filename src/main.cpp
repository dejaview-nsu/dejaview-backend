#include "auth/login.hpp"
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
        .registerHandler("/health", [db](HttpRequestPtr) { return healthHandler(db); }, {Get})
        .registerHandler("/api/v1/auth/session",
                         [db](HttpRequestPtr req) { return getSessionHandler(db, req); }, {Get})
        .registerHandler("/api/v1/auth/register", [db, appUrl = config.appUrl](HttpRequestPtr req)
                         { return registerHandler(db, appUrl, req); }, {Post})
        .registerHandler("/api/v1/auth/confirm-email",
                         [db](HttpRequestPtr req) { return confirmEmailHandler(db, req); }, {Post})
        .registerHandler("/api/v1/auth/login",
                         [db, captchaKey = config.smartCaptchaServerKey](HttpRequestPtr req)
                         { return loginHandler(db, captchaKey, req); }, {Post})
        .registerHandler("/api/v1/auth/resend-confirmation",
                         [db, appUrl = config.appUrl](HttpRequestPtr req)
                         { return resendConfirmationHandler(db, appUrl, req); }, {Post})
        .run();
}
