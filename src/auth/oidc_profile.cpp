#include "auth/oidc_profile.hpp"

#include <algorithm>
#include <array>

namespace
{
// Строчные а-я по порядку кодов Unicode (U+0430-U+044F): упрощённая транслитерация, как в
// загранпаспорте. ъ и ь не передаются.
constexpr std::array<std::string_view, 32> kCyrillic = {
    "a", "b", "v", "g", "d", "e",  "zh", "z",  "i",  "y",    "k", "l", "m", "n", "o",  "p",
    "r", "s", "t", "u", "f", "kh", "ts", "ch", "sh", "shch", "",  "y", "",  "e", "yu", "ya"};

// Латиница для одного символа имени профиля: буквы и цифры - строчными, разделители - `_`,
// остальное - пусто. codePoint - символ Unicode.
std::string_view latin(char32_t codePoint, std::string &buffer)
{
    if (codePoint >= U'A' && codePoint <= U'Z')
    {
        buffer = static_cast<char>(codePoint - U'A' + U'a');
        return buffer;
    }
    if ((codePoint >= U'a' && codePoint <= U'z') || (codePoint >= U'0' && codePoint <= U'9'))
    {
        buffer = static_cast<char>(codePoint);
        return buffer;
    }
    if (codePoint == U' ' || codePoint == U'_' || codePoint == U'-' || codePoint == U'.')
    {
        return "_";
    }
    if (codePoint == U'ё' || codePoint == U'Ё')
    {
        return "e";
    }
    if (codePoint >= U'А' && codePoint <= U'Я')
    {
        codePoint += U'а' - U'А';  // в строчную
    }
    if (codePoint >= U'а' && codePoint <= U'я')
    {
        return kCyrillic[codePoint - U'а'];
    }
    return "";
}

// Следующий символ UTF-8 из начала text: 1-4 байта. Испорченный байт пропускается.
char32_t nextCodePoint(std::string_view &text)
{
    const auto lead = static_cast<unsigned char>(text[0]);
    const std::size_t size = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
    if ((lead >= 0x80 && lead < 0xC0) || size > text.size())
    {
        text.remove_prefix(1);
        return 0;
    }
    char32_t codePoint = size == 1 ? lead : lead & (0xFF >> (size + 1));
    for (std::size_t i = 1; i < size; ++i)
    {
        codePoint = (codePoint << 6) | (static_cast<unsigned char>(text[i]) & 0x3F);
    }
    text.remove_prefix(size);
    return codePoint;
}

// Имя по правилам AuthUsername или nullopt, если короче 3 символов
std::optional<std::string> toUsername(std::string_view text)
{
    std::string result;
    std::string buffer;
    while (!text.empty())
    {
        const std::string_view piece = latin(nextCodePoint(text), buffer);
        // подряд идущие разделители - один `_`, в начале - не нужен
        if (piece == "_" && (result.empty() || result.ends_with('_')))
        {
            continue;
        }
        result += piece;
    }
    while (result.ends_with('_'))
    {
        result.pop_back();
    }
    if (result.size() > 30)
    {
        result.resize(30);
        while (result.ends_with('_'))
        {
            result.pop_back();
        }
    }
    if (result.size() < 3)
    {
        return std::nullopt;
    }
    return result;
}
}  // namespace

std::optional<OidcProfile> parseYandexProfile(const Json::Value &info)
{
    // Ответ login.yandex.ru/info: id, login, display_name, real_name, default_email
    const std::string id = info.get("id", "").asString();
    if (id.empty())
    {
        return std::nullopt;
    }
    // Имя - первое непустое: отображаемое, настоящее, логин
    std::string name;
    for (const char *field : {"display_name", "real_name", "login"})
    {
        name = info.get(field, "").asString();
        if (!name.empty())
        {
            break;
        }
    }
    const std::string email = info.get("default_email", "").asString();
    // Адреса в Яндекс ID подтверждены: это ящик Яндекса или адрес, который пользователь подтвердил
    return OidcProfile{
        .subject = id, .email = email, .emailVerified = !email.empty(), .displayName = name};
}

std::optional<OidcProfile> parseGoogleProfile(const Json::Value &info)
{
    const std::string subject = info.get("sub", "").asString();
    if (subject.empty())
    {
        return std::nullopt;
    }
    // email_verified по стандарту - true/false; asString даёт "true" и для строки "true"
    return OidcProfile{.subject = subject,
                       .email = info.get("email", "").asString(),
                       .emailVerified = info.get("email_verified", false).asString() == "true",
                       .displayName = info.get("name", "").asString()};
}

std::string_view providerTitle(std::string_view provider)
{
    // Справочник: новый провайдер - новая строка. constexpr - таблица готова при компиляции
    struct Title
    {
        std::string_view provider;
        std::string_view title;
    };
    static constexpr Title kTitles[] = {{"google", "Google"}, {"yandex", "Яндекс"}, {"vk", "VK"}};

    const auto found = std::ranges::find(kTitles, provider, &Title::provider);
    return found != std::ranges::end(kTitles) ? found->title : provider;
}

std::string suggestUsername(std::string_view displayName, std::string_view email)
{
    // Из имени профиля, не вышло - из начала email, не вышло и так - «user»
    return toUsername(displayName)
        .or_else([&] { return toUsername(email.substr(0, email.find('@'))); })
        .value_or("user");
}
