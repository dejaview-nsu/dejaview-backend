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

// Адрес вида http(s)://host без '/' на конце. name - для текста ошибки.
std::string readUrl(const char *name, std::string_view url)
{
    if (!url.starts_with("http://") && !url.starts_with("https://"))
    {
        throw std::runtime_error(
            std::format("{}: не задан или не начинается с http:// или https://", name));
    }
    while (url.ends_with('/'))
    {
        url.remove_suffix(1);
    }
    return std::string(url);
}

std::string readAppUrl() { return readUrl("APP_URL", env("APP_URL").value_or("")); }

SmtpConfig readSmtp()
{
    const std::string_view url = env("SMTP_URL").value_or("");
    if (!url.starts_with("smtp://") && !url.starts_with("smtps://"))
    {
        throw std::runtime_error("SMTP_URL: не задан или не начинается с smtp:// или smtps://");
    }
    // Адрес попадает в заголовок письма From: перевод строки в нём дописал бы чужие заголовки
    const std::string_view from = env("SMTP_FROM").value_or("");
    if (!from.contains('@') || from.find_first_of("\r\n <>") != std::string_view::npos)
    {
        throw std::runtime_error("SMTP_FROM: ожидается адрес вида noreply@dejaview.ru");
    }
    return SmtpConfig{
        .url = std::string(url),
        .from = std::string(from),
        .user = std::string(env("SMTP_USER").value_or("")),
        .password = std::string(env("SMTP_PASSWORD").value_or("")),
    };
}
}  // namespace

Config loadConfig()
{
    // Элементы в фигурных скобках вычисляются слева направо: переменные проверяются по порядку.
    return Config{
        .port = readPort(),
        .databaseUrl = readDatabaseUrl(),
        .appUrl = readAppUrl(),
        .smartCaptchaServerKey = std::string(env("SMARTCAPTCHA_SERVER_KEY").value_or("")),
        .smtp = readSmtp(),
        .apiUrl = readUrl("API_URL", env("API_URL").value_or(env("APP_URL").value_or(""))),
        .yandex = {.clientId = std::string(env("OIDC_YANDEX_CLIENT_ID").value_or("")),
                   .clientSecret = std::string(env("OIDC_YANDEX_CLIENT_SECRET").value_or(""))},
        .google = {.clientId = std::string(env("OIDC_GOOGLE_CLIENT_ID").value_or("")),
                   .clientSecret = std::string(env("OIDC_GOOGLE_CLIENT_SECRET").value_or(""))},
        .vk = {.clientId = std::string(env("OIDC_VK_CLIENT_ID").value_or(""))},
    };
}
