#include "config.hpp"

#include <charconv>
#include <cstdlib>
#include <format>
#include <stdexcept>
#include <string_view>

Config loadConfig()
{
    Config config{.port = 8081};

    if (const char *raw = std::getenv("PORT"))
    {
        const std::string_view value = raw;
        const auto [end, error] =
            std::from_chars(value.data(), value.data() + value.size(), config.port);
        if (error != std::errc{} || end != value.data() + value.size() || config.port == 0)
        {
            throw std::runtime_error(
                std::format("PORT: ожидается число от 1 до 65535, получено '{}'", value));
        }
    }

    return config;
}
