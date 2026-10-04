#include "config.hpp"

#include <charconv>
#include <cstdlib>
#include <format>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace
{
constexpr std::uint16_t kDefaultPort = 8081;

// Значение переменной окружения или nullopt, если она не задана
std::optional<std::string_view> env(const char *name)
{
    if (const char *value = std::getenv(name))
    {
        return value;
    }
    return std::nullopt;
}

std::uint16_t readPort()
{
    const auto value = env("PORT");
    if (!value)
    {
        return kDefaultPort;
    }
    std::uint16_t port = 0;
    const char *last = value->data() + value->size();
    const auto [end, error] = std::from_chars(value->data(), last, port);
    if (error != std::errc{} || end != last || port == 0)
    {
        throw std::runtime_error(
            std::format("PORT: ожидается число от 1 до 65535, получено '{}'", *value));
    }
    return port;
}

std::string readDatabaseUrl()
{
    const std::string_view url = env("DATABASE_URL").value_or("");
    if (!url.starts_with("postgres://") && !url.starts_with("postgresql://"))
    {
        throw std::runtime_error("DATABASE_URL: не задан или не начинается с postgres://");
    }
    return std::string(url);
}

std::string readAppUrl()
{
    std::string_view url = env("APP_URL").value_or("");
    if (!url.starts_with("http://") && !url.starts_with("https://"))
    {
        throw std::runtime_error("APP_URL: не задан или не начинается с http:// или https://");
    }
    while (url.ends_with('/'))
    {
        url.remove_suffix(1);
    }
    return std::string(url);
}
}  // namespace

Config loadConfig()
{
    // Элементы в фигурных скобках вычисляются слева направо: переменные проверяются по порядку.
    return Config{
        .port = readPort(),
        .databaseUrl = readDatabaseUrl(),
        .appUrl = readAppUrl(),
    };
}
