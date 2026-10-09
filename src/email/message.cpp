#include "email/message.hpp"

#include "email/templates.hpp"

#include <sodium.h>

#include <algorithm>
#include <format>

namespace
{
std::string base64(std::string_view bytes)
{
    constexpr int variant = sodium_base64_VARIANT_ORIGINAL;
    std::string result(sodium_base64_ENCODED_LEN(bytes.size(), variant), '\0');
    sodium_bin2base64(result.data(), result.size(),
                      reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size(), variant);
    result.pop_back();  // sodium_bin2base64 дописывает '\0'
    return result;
}

// \n -> \r\n: в SMTP строки письма разделяются только CRLF
std::string toCrlf(std::string_view text)
{
    std::string result;
    for (const char c : text)
    {
        if (c == '\n')
        {
            result += '\r';
        }
        result += c;
    }
    return result;
}

// Base64 строками по 76 символов: длиннее MIME не разрешает (RFC 2045 п. 6.8)
std::string base64Lines(std::string_view bytes)
{
    const std::string encoded = base64(bytes);
    std::string result;
    for (std::size_t i = 0; i < encoded.size(); i += 76)
    {
        result += encoded.substr(i, 76) + "\r\n";
    }
    return result;
}

// Значение для HTML: символы разметки превращаются в сущности, иначе имя вида <b> стало бы тегом
std::string htmlEscape(std::string_view text)
{
    std::string result;
    for (const char c : text)
    {
        switch (c)
        {
            case '&':
                result += "&amp;";
                break;
            case '<':
                result += "&lt;";
                break;
            case '>':
                result += "&gt;";
                break;
            case '"':
                result += "&quot;";
                break;
            case '\'':
                result += "&#39;";
                break;
            default:
                result += c;
        }
    }
    return result;
}

// Подстановка в шаблон: каждое {{name}} - экранированное значение. Не std::format: фигурные
// скобки в шаблоне могут быть и свои (CSS), и их не нужно удваивать
std::string fillTemplate(std::string_view html, std::string_view name, std::string_view value)
{
    const std::string placeholder = "{{" + std::string(name) + "}}";
    const std::string escaped = htmlEscape(value);
    std::string result(html);
    for (auto at = result.find(placeholder); at != std::string::npos;
         at = result.find(placeholder, at + escaped.size()))
    {
        result.replace(at, placeholder.size(), escaped);
    }
    return result;
}

// Граница между частями письма. «=_» не встречается в base64, а текстовая часть - наш шаблон
constexpr std::string_view kBoundary = "=_dejaview_part_=";
}  // namespace

std::optional<Email> renderEmail(std::string_view kind, std::string_view to,
                                 std::string_view username, std::string_view link)
{
    if (kind == "email_confirm")
    {
        return Email{.to = std::string(to),
                     .subject = "Подтвердите email в DejaView",
                     .body = std::format("Здравствуйте, {}!\n"
                                         "\n"
                                         "Чтобы завершить регистрацию в DejaView, перейдите по "
                                         "ссылке:\n"
                                         "{}\n"
                                         "\n"
                                         "Ссылка действует 24 часа. Если вы не регистрировались в "
                                         "DejaView, просто проигнорируйте это письмо.\n",
                                         username, link),
                     .html = fillTemplate(fillTemplate(kEmailConfirmHtml, "username", username),
                                          "link", link)};
    }
    if (kind == "password_reset")
    {
        return Email{.to = std::string(to),
                     .subject = "Восстановление пароля в DejaView",
                     .body = std::format("Здравствуйте, {}!\n"
                                         "\n"
                                         "Чтобы задать новый пароль в DejaView, перейдите по "
                                         "ссылке:\n"
                                         "{}\n"
                                         "\n"
                                         "Ссылка действует 1 час, работает только ссылка из "
                                         "последнего письма. Если вы не запрашивали "
                                         "восстановление пароля, просто проигнорируйте это "
                                         "письмо: пароль останется прежним.\n",
                                         username, link),
                     .html = fillTemplate(fillTemplate(kPasswordResetHtml, "username", username),
                                          "link", link)};
    }
    if (kind == "password_changed")
    {
        return Email{.to = std::string(to),
                     .subject = "Пароль в DejaView изменён",
                     .body = std::format("Здравствуйте, {}!\n"
                                         "\n"
                                         "Пароль вашей учётной записи DejaView был изменён. Если "
                                         "это были не вы, немедленно восстановите пароль:\n"
                                         "{}\n"
                                         "\n"
                                         "Если пароль меняли вы, ничего делать не нужно.\n",
                                         username, link),
                     .html = fillTemplate(fillTemplate(kPasswordChangedHtml, "username", username),
                                          "link", link)};
    }
    return std::nullopt;
}

std::string formatMessage(const Email &email, std::string_view from,
                          std::chrono::system_clock::time_point now)
{
    // Date и From обязательны по RFC 5322
    const auto seconds = std::chrono::floor<std::chrono::seconds>(now);
    const std::string headers = std::format(
        "From: DejaView <{}>\r\n"
        "To: <{}>\r\n"
        "Subject: {}\r\n"
        "Date: {:%a, %d %b %Y %H:%M:%S} +0000\r\n"
        "MIME-Version: 1.0\r\n",
        from, email.to, encodeHeader(email.subject), seconds);
    // Текст - 8bit: UTF-8 как есть, его принимает любой современный SMTP-сервер (8BITMIME), а
    // строки короткие. HTML - base64: в шаблоне бывают строки длиннее 998 байт, предела SMTP
    const std::string textPart =
        "Content-Type: text/plain; charset=UTF-8\r\n"
        "Content-Transfer-Encoding: 8bit\r\n"
        "\r\n" +
        toCrlf(email.body);
    if (email.html.empty())
    {
        return headers + textPart;
    }
    // Части от простой к лучшей: клиент показывает последнюю, которую умеет показать
    return std::format(
        "{}Content-Type: multipart/alternative; boundary=\"{}\"\r\n"
        "\r\n"
        "--{}\r\n"
        "{}\r\n"
        "--{}\r\n"
        "Content-Type: text/html; charset=UTF-8\r\n"
        "Content-Transfer-Encoding: base64\r\n"
        "\r\n"
        "{}"
        "--{}--\r\n",
        headers, kBoundary, kBoundary, textPart, kBoundary, base64Lines(email.html), kBoundary);
}

std::string encodeHeader(std::string_view text)
{
    // Печатные ASCII-символы - от пробела (0x20) до тильды (0x7E). Байты UTF-8 кириллицы
    // (0xD0 и выше) и управляющие символы вроде \r\n сюда не попадают
    const bool ascii = std::ranges::all_of(text,
                                           [](char c)
                                           {
                                               const auto byte = static_cast<unsigned char>(c);
                                               return byte >= 0x20 && byte < 0x7F;
                                           });
    if (ascii)
    {
        return std::string(text);
    }

    // Куски по целым символам UTF-8 не больше 45 байт: base64 от 45 байт - 60 символов, с
    // обрамлением =?UTF-8?B?...?= ровно 72, укладываемся в 75 (RFC 2047 п. 2)
    constexpr std::size_t kChunkBytes = 45;
    std::string result;
    while (!text.empty())
    {
        std::size_t size = std::min(kChunkBytes, text.size());
        // не резать символ посередине: отступаем назад, пока следующий байт - продолжение
        while (size < text.size() && (static_cast<unsigned char>(text[size]) & 0xC0) == 0x80)
        {
            --size;
        }
        if (!result.empty())
        {
            result += "\r\n ";  // перенос длинного заголовка: новая строка начинается с пробела
        }
        result += "=?UTF-8?B?" + base64(text.substr(0, size)) + "?=";
        text.remove_prefix(size);
    }
    return result;
}

int retryDelaySeconds(int failedAttempts)
{
    constexpr int kMaxDelaySeconds = 60 * 60;
    if (failedAttempts >= 7)  // 60 * 2^6 = 3840 - уже больше часа
    {
        return kMaxDelaySeconds;
    }
    return std::min(60 << std::max(failedAttempts - 1, 0), kMaxDelaySeconds);
}
