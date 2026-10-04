#pragma once

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
};

// Читает настройки из переменных окружения. При некорректном значении бросает std::runtime_error.
Config loadConfig();
