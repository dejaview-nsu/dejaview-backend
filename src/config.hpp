#pragma once

#include <cstdint>

struct Config
{
    std::uint16_t port;
};

// Читает настройки из переменных окружения. При некорректном значении бросает std::runtime_error.
Config loadConfig();
