#include "auth/captcha.hpp"

#include <drogon/drogon.h>

using namespace drogon;

Task<bool> verifyCaptcha(CaptchaSettings settings, std::string token, std::string ip)
{
    // Новый клиент и TLS-рукопожатие на каждую проверку; CAPTCHA нужна только после
    // 3 неудачных попыток, это редко. Если станет часто - один клиент на всё приложение
    auto client = HttpClient::newHttpClient(settings.url);
    auto request = HttpRequest::newHttpFormPostRequest();
    request->setPath("/validate");
    request->setParameter("secret", settings.serverKey);
    request->setParameter("token", token);
    request->setParameter("ip", ip);

    try
    {
        const auto response = co_await client->sendRequestCoro(request, 5.0);
        // 4xx - ошибка у нас (неверный серверный ключ): CAPTCHA фактически выключена, это должен
        // заметить мониторинг - ERROR. 5xx - сбой у Яндекса, достаточно WARN
        if (response->statusCode() >= 400 && response->statusCode() < 500)
        {
            LOG_ERROR << "SmartCaptcha ответила " << response->statusCode()
                      << " - проверьте SMARTCAPTCHA_SERVER_KEY. Проверка пропущена: "
                      << response->body();
            co_return true;
        }
        if (response->statusCode() != k200OK)
        {
            LOG_WARN << "SmartCaptcha ответила " << response->statusCode()
                     << ", проверка пропущена";
            co_return true;
        }
        const auto &json = response->getJsonObject();
        co_return json && json->get("status", "").asString() == "ok";
    }
    catch (const std::exception &e)
    {
        LOG_WARN << "SmartCaptcha недоступна, проверка пропущена: " << e.what();
        co_return true;
    }
}
