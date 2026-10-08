#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

// Почтовый сервер для писем из email_outbox. Учётные данные - только из окружения (#17826).
struct SmtpConfig
{
    // smtps://host:465 - TLS с первого байта, для продакшена. smtp://host:port - TLS, только если
    // сервер его предложит (STARTTLS), иначе открытым текстом: для локального Mailpit
    std::string url;
    std::string from;  // адрес отправителя: noreply@dejaview.ru
    std::string user;  // пустой - без авторизации (Mailpit)
    std::string password;
};

// Приложение у OIDC-провайдера (#17148 п. 3). Пустой clientId - вход через провайдера выключен.
struct OidcClient
{
    std::string clientId;
    std::string clientSecret;
};

// ML-сервис поиска по изображению и видео. Числа по умолчанию - стартовые значения контракта
struct MlConfig
{
    std::string url = "http://ml:8000";  // ML_URL
    // ML_MAX_CONCURRENT: сколько обращений к ML одновременно на экземпляр backend
    std::size_t maxConcurrent = 4;
    // Один срок на весь поиск, включая соединение и проверку /health
    std::chrono::milliseconds imageTimeout{9'000};   // ML_IMAGE_TIMEOUT_MS
    std::chrono::milliseconds videoTimeout{14'000};  // ML_VIDEO_TIMEOUT_MS
    // ML_FAILURES_TO_OPEN: аварий подряд до отключения вызовов
    int failuresToOpen = 5;
    // ML_OPEN_FOR_MS: сколько вызовы отключены
    std::chrono::milliseconds openFor{30'000};
};

struct Config
{
    std::uint16_t port;
    // postgres://user:password@host:5432/db
    std::string databaseUrl;
    // Адрес frontend: из него собираются ссылки в письмах,
    // {appUrl}/confirm-email?token=...
    std::string appUrl;
    // Серверный ключ Yandex SmartCaptcha. Пустой - CAPTCHA при входе отключена: только для
    // локальной разработки, в продакшене обязателен (#17094 п. 3.3)
    std::string smartCaptchaServerKey;
    SmtpConfig smtp;
    // Публичный адрес backend: из него собирается redirect_uri для OIDC,
    // {apiUrl}/api/v1/auth/oidc/{provider}/callback. По умолчанию - appUrl: в продакшене
    // frontend и API на одном домене. Локально они на разных портах
    std::string apiUrl;
    OidcClient yandex;
    OidcClient google;
    OidcClient vk;  // только clientId: VK ID обходится без секрета приложения (PKCE)
    MlConfig ml;
};

// Читает настройки из переменных окружения. При некорректном значении бросает std::runtime_error.
Config loadConfig();
