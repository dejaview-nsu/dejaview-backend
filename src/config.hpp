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
};

// Читает настройки из переменных окружения. При некорректном значении бросает std::runtime_error.
Config loadConfig();
