#include "email/message.hpp"

#include <gtest/gtest.h>
#include <sodium.h>

#include <chrono>
#include <string>

namespace
{
// Обратное преобразование для проверки: из =?UTF-8?B?...?= кусков - исходный текст.
std::string decodeHeader(std::string encoded)
{
    std::string result;
    std::size_t start = 0;
    while ((start = encoded.find("=?UTF-8?B?", start)) != std::string::npos)
    {
        start += 10;
        const std::size_t end = encoded.find("?=", start);
        const std::string chunk = encoded.substr(start, end - start);
        std::string bytes(chunk.size(), '\0');
        std::size_t length = 0;
        sodium_base642bin(reinterpret_cast<unsigned char *>(bytes.data()), bytes.size(),
                          chunk.data(), chunk.size(), nullptr, &length, nullptr,
                          sodium_base64_VARIANT_ORIGINAL);
        result += bytes.substr(0, length);
        start = end + 2;
    }
    return result;
}

Email confirmEmail()
{
    return *renderEmail("email_confirm", "ivan@example.com", "movie_fan_42",
                        "https://dejaview.ru/confirm-email?token=abc");
}
}  // namespace

TEST(RenderEmailTest, ConfirmEmailHasLinkAndName)
{
    const Email email = confirmEmail();
    EXPECT_EQ(email.to, "ivan@example.com");
    EXPECT_NE(email.body.find("https://dejaview.ru/confirm-email?token=abc"), std::string::npos);
    EXPECT_NE(email.body.find("movie_fan_42"), std::string::npos);
    EXPECT_NE(email.body.find("24 часа"), std::string::npos);  // срок ссылки, #17148 п. 2.5
}

TEST(RenderEmailTest, PasswordResetHasLinkAndExpiry)
{
    const Email email = *renderEmail("password_reset", "ivan@example.com", "movie_fan_42",
                                     "https://dejaview.ru/reset-password?token=abc");
    EXPECT_EQ(email.to, "ivan@example.com");
    EXPECT_NE(email.body.find("https://dejaview.ru/reset-password?token=abc"), std::string::npos);
    EXPECT_NE(email.body.find("movie_fan_42"), std::string::npos);
    EXPECT_NE(email.body.find("1 час"), std::string::npos);  // срок ссылки, #17150 п. 1.2 шаг 9
    EXPECT_NE(email.html.find("href=\"https://dejaview.ru/reset-password?token=abc\""),
              std::string::npos);
    EXPECT_EQ(email.html.find("{{"), std::string::npos) << "осталась неподставленная переменная";
}

TEST(RenderEmailTest, PasswordChangedWarnsAndLinksToRecovery)
{
    const Email email = *renderEmail("password_changed", "ivan@example.com", "movie_fan_42",
                                     "https://dejaview.ru/forgot-password");
    // текст из #17150 п. 2.5
    EXPECT_NE(email.body.find("Пароль вашей учётной записи DejaView был изменён. Если это были не "
                              "вы, немедленно восстановите пароль"),
              std::string::npos)
        << email.body;
    EXPECT_NE(email.body.find("https://dejaview.ru/forgot-password"), std::string::npos);
    EXPECT_NE(email.html.find("href=\"https://dejaview.ru/forgot-password\""), std::string::npos);
    EXPECT_EQ(email.html.find("{{"), std::string::npos) << "осталась неподставленная переменная";
}

TEST(RenderEmailTest, UnknownKindHasNoTemplate)
{
    EXPECT_EQ(renderEmail("newsletter", "a@b.co", "x", "y"), std::nullopt);
}

TEST(FormatMessageTest, HasRequiredHeadersAndCrlf)
{
    // 2026-10-04 10:15:00 UTC, воскресенье
    const auto now = std::chrono::sys_days{std::chrono::year{2026} / 10 / 4} +
                     std::chrono::hours{10} + std::chrono::minutes{15};
    const std::string message = formatMessage(confirmEmail(), "noreply@dejaview.ru", now);

    EXPECT_TRUE(message.starts_with("From: DejaView <noreply@dejaview.ru>\r\n")) << message;
    EXPECT_NE(message.find("\r\nTo: <ivan@example.com>\r\n"), std::string::npos);
    EXPECT_NE(message.find("\r\nDate: Sun, 04 Oct 2026 10:15:00 +0000\r\n"), std::string::npos);
    EXPECT_NE(message.find("\r\nContent-Type: text/plain; charset=UTF-8\r\n"), std::string::npos);
    // заголовки и тело разделяет пустая строка
    EXPECT_NE(message.find("\r\n\r\nЗдравствуйте, movie_fan_42!\r\n"), std::string::npos);

    // ни одного «голого» \n: SMTP-серверы отклоняют такие письма или портят их
    for (std::size_t i = 0; i < message.size(); ++i)
    {
        if (message[i] == '\n')
        {
            ASSERT_GT(i, 0U);
            EXPECT_EQ(message[i - 1], '\r') << "позиция " << i;
        }
    }
}

TEST(RenderEmailTest, HtmlHasLinkAndNameWithoutPlaceholders)
{
    const Email email = confirmEmail();
    EXPECT_NE(email.html.find("href=\"https://dejaview.ru/confirm-email?token=abc\""),
              std::string::npos);
    EXPECT_NE(email.html.find("Здравствуйте, movie_fan_42!"), std::string::npos);
    EXPECT_EQ(email.html.find("{{"), std::string::npos) << "осталась неподставленная переменная";
}

TEST(RenderEmailTest, HtmlEscapesValues)
{
    // имя пользователя и ссылка в HTML - данные, а не разметка
    const Email email = *renderEmail("email_confirm", "a@b.co", "<script>alert(1)</script>",
                                     "https://dejaview.ru/x?a=1&b=\"2\"");
    EXPECT_EQ(email.html.find("<script>"), std::string::npos);
    EXPECT_NE(email.html.find("&lt;script&gt;alert(1)&lt;/script&gt;"), std::string::npos);
    EXPECT_NE(email.html.find("href=\"https://dejaview.ru/x?a=1&amp;b=&quot;2&quot;\""),
              std::string::npos);
}

TEST(FormatMessageTest, SendsTextAndHtmlAsAlternatives)
{
    const Email email = confirmEmail();
    const std::string message = formatMessage(email, "noreply@dejaview.ru", {});

    EXPECT_NE(
        message.find("\r\nContent-Type: multipart/alternative; boundary=\"=_dejaview_part_=\"\r\n"),
        std::string::npos);
    // сначала текст, потом HTML: клиент показывает последнюю часть, которую умеет
    const auto text = message.find("--=_dejaview_part_=\r\nContent-Type: text/plain");
    const auto html = message.find("--=_dejaview_part_=\r\nContent-Type: text/html");
    ASSERT_NE(text, std::string::npos);
    ASSERT_NE(html, std::string::npos);
    EXPECT_LT(text, html);
    EXPECT_TRUE(message.ends_with("--=_dejaview_part_=--\r\n"));

    // HTML-часть - base64 строками до 76 символов, и раскодируется в исходный HTML
    const std::string marker = "Content-Transfer-Encoding: base64\r\n\r\n";
    const auto start = message.find(marker) + marker.size();
    const std::string encoded = message.substr(start, message.find("--=_", start) - start);
    std::string decoded(encoded.size(), '\0');
    std::size_t length = 0;
    ASSERT_EQ(sodium_base642bin(reinterpret_cast<unsigned char *>(decoded.data()), decoded.size(),
                                encoded.data(), encoded.size(), "\r\n", &length, nullptr,
                                sodium_base64_VARIANT_ORIGINAL),
              0);
    EXPECT_EQ(decoded.substr(0, length), email.html);

    // ни одна строка письма не длиннее 998 байт (предел SMTP), строки base64 - не длиннее 76
    std::size_t lineStart = 0;
    for (auto end = message.find("\r\n"); end != std::string::npos;
         lineStart = end + 2, end = message.find("\r\n", lineStart))
    {
        EXPECT_LE(end - lineStart,
                  start <= lineStart && lineStart < start + encoded.size() ? 76U : 998U);
    }
}

TEST(FormatMessageTest, TextOnlyWithoutHtml)
{
    Email email = confirmEmail();
    email.html.clear();
    const std::string message = formatMessage(email, "noreply@dejaview.ru", {});
    EXPECT_EQ(message.find("multipart"), std::string::npos);
    EXPECT_NE(message.find("\r\nContent-Type: text/plain; charset=UTF-8\r\n"), std::string::npos);
}

TEST(EncodeHeaderTest, KeepsAsciiAsIs)
{
    EXPECT_EQ(encodeHeader("Welcome to DejaView"), "Welcome to DejaView");
}

TEST(EncodeHeaderTest, EncodesCyrillicAndDecodesBack)
{
    const std::string subject = "Подтвердите email в DejaView";
    const std::string encoded = encodeHeader(subject);
    EXPECT_TRUE(encoded.starts_with("=?UTF-8?B?")) << encoded;
    EXPECT_EQ(decodeHeader(encoded), subject);
}

TEST(EncodeHeaderTest, SplitsLongTextIntoShortWordsOnCharBoundaries)
{
    // 100 кириллических символов = 200 байт: несколько кусков, ни один символ не разрезан
    std::string subject;
    for (int i = 0; i < 100; ++i)
    {
        subject += "ж";
    }
    const std::string encoded = encodeHeader(subject);
    EXPECT_EQ(decodeHeader(encoded), subject);

    std::size_t start = 0;
    int words = 0;
    while (start < encoded.size())
    {
        const std::size_t end = encoded.find("\r\n ", start);
        const std::string word =
            encoded.substr(start, end == std::string::npos ? std::string::npos : end - start);
        EXPECT_LE(word.size(), 75U) << word;  // предел RFC 2047
        ++words;
        start = end == std::string::npos ? encoded.size() : end + 3;
    }
    EXPECT_GT(words, 1);
}

TEST(EncodeHeaderTest, LineBreaksCannotInjectHeaders)
{
    // \r\n в теме не должен стать новой строкой заголовков: такой текст кодируется целиком
    for (const char *subject : {"Тема\r\nBcc: victim@example.com", "Hi\r\nBcc: victim@example.com"})
    {
        SCOPED_TRACE(subject);
        const std::string encoded = encodeHeader(subject);
        EXPECT_EQ(encoded.find("\r\nBcc"), std::string::npos) << encoded;
        EXPECT_EQ(decodeHeader(encoded), subject);
    }
}

TEST(RetryDelayTest, DoublesUpToAnHour)
{
    EXPECT_EQ(retryDelaySeconds(1), 60);
    EXPECT_EQ(retryDelaySeconds(2), 120);
    EXPECT_EQ(retryDelaySeconds(3), 240);
    EXPECT_EQ(retryDelaySeconds(6), 1920);
    EXPECT_EQ(retryDelaySeconds(7), 3600);
    EXPECT_EQ(retryDelaySeconds(50), 3600);  // без переполнения сдвига
}
