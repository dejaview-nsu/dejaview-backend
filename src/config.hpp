#pragma once

#include <cstdint>
#include <string>

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
};

// Читает настройки из переменных окружения. При некорректном значении бросает std::runtime_error.
Config loadConfig();
