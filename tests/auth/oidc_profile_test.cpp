#include "auth/oidc_profile.hpp"

#include <gtest/gtest.h>
#include <json/reader.h>

namespace
{
Json::Value json(const std::string &text)
{
    Json::Value value;
    Json::Reader().parse(text, value);
    return value;
}
}  // namespace

TEST(ParseYandexProfileTest, ReadsIdEmailAndName)
{
    // сокращённый настоящий ответ login.yandex.ru/info?format=json
    const auto profile = parseYandexProfile(json(R"({
        "id": "1130000012345678", "login": "vova.shar2016", "display_name": "Владимир",
        "real_name": "Владимир Шарапов", "default_email": "vova.shar2016@yandex.ru"})"));
    ASSERT_TRUE(profile);
    EXPECT_EQ(profile->subject, "1130000012345678");
    EXPECT_EQ(profile->email, "vova.shar2016@yandex.ru");
    EXPECT_TRUE(profile->emailVerified);
    EXPECT_EQ(profile->displayName, "Владимир");
}

TEST(ParseYandexProfileTest, FallsBackToRealNameAndLogin)
{
    EXPECT_EQ(parseYandexProfile(json(R"({"id": "1", "real_name": "Ivan Petrov"})"))->displayName,
              "Ivan Petrov");
    EXPECT_EQ(parseYandexProfile(json(R"({"id": "1", "login": "ivan"})"))->displayName, "ivan");
}

TEST(ParseYandexProfileTest, NoEmailIsNotVerified)
{
    // доступ к email пользователь может не дать: такой профиль для регистрации не годится
    const auto profile = parseYandexProfile(json(R"({"id": "1", "login": "ivan"})"));
    EXPECT_EQ(profile->email, "");
    EXPECT_FALSE(profile->emailVerified);
}

TEST(ParseYandexProfileTest, RejectsResponseWithoutId)
{
    EXPECT_EQ(parseYandexProfile(json(R"({"error": "invalid_token"})")), std::nullopt);
}

TEST(ParseGoogleProfileTest, ReadsStandardClaims)
{
    // ответ openidconnect.googleapis.com/v1/userinfo
    const auto profile = parseGoogleProfile(json(R"({
        "sub": "110169484474386276334", "name": "Ivan Petrov", "given_name": "Ivan",
        "email": "ivan@gmail.com", "email_verified": true, "picture": "https://..."})"));
    ASSERT_TRUE(profile);
    EXPECT_EQ(profile->subject, "110169484474386276334");
    EXPECT_EQ(profile->email, "ivan@gmail.com");
    EXPECT_TRUE(profile->emailVerified);
    EXPECT_EQ(profile->displayName, "Ivan Petrov");
}

TEST(ParseGoogleProfileTest, UnverifiedEmailIsNotTrusted)
{
    // Google сообщает, подтвердил ли он адрес: на неподтверждённый email вход не пускаем
    EXPECT_FALSE(parseGoogleProfile(json(R"({"sub": "1", "email": "a@b.co",
        "email_verified": false})"))
                     ->emailVerified);
    EXPECT_FALSE(parseGoogleProfile(json(R"({"sub": "1", "email": "a@b.co"})"))->emailVerified);
}

TEST(ParseGoogleProfileTest, RejectsResponseWithoutSub)
{
    EXPECT_EQ(parseGoogleProfile(json(R"({"error": "invalid_token"})")), std::nullopt);
}

TEST(ParseVkProfileTest, ReadsUserObject)
{
    // ответ id.vk.com/oauth2/user_info
    const auto profile = parseVkProfile(json(R"({"user": {
        "user_id": "1234567890", "first_name": "Иван", "last_name": "Петров",
        "email": "ivan@example.com", "verified": false, "phone": "79991234567"}})"));
    ASSERT_TRUE(profile);
    EXPECT_EQ(profile->subject, "1234567890");
    EXPECT_EQ(profile->email, "ivan@example.com");
    EXPECT_TRUE(profile->emailVerified);  // verified - про страницу, не про email
    EXPECT_EQ(profile->displayName, "Иван Петров");
}

TEST(ParseVkProfileTest, NumericIdAndNoEmail)
{
    // аккаунт только с телефоном: email нет - войти через VK не выйдет (вопрос к аналитикам)
    const auto profile = parseVkProfile(json(R"({"user": {"user_id": 42, "first_name": "Иван"}})"));
    ASSERT_TRUE(profile);
    EXPECT_EQ(profile->subject, "42");
    EXPECT_EQ(profile->email, "");
    EXPECT_FALSE(profile->emailVerified);
    EXPECT_EQ(profile->displayName, "Иван");
}

TEST(ParseVkProfileTest, RejectsResponseWithoutUser)
{
    EXPECT_EQ(parseVkProfile(json(R"({"error": "invalid_token"})")), std::nullopt);
}

TEST(SuggestUsernameTest, TransliteratesCyrillic)
{
    EXPECT_EQ(suggestUsername("Владимир Шарапов", ""), "vladimir_sharapov");
    EXPECT_EQ(suggestUsername("Щука Юлия Ёжикова", ""), "shchuka_yuliya_ezhikova");
    EXPECT_EQ(suggestUsername("Подъезд", ""), "podezd");  // ъ и ь не передаются
}

TEST(SuggestUsernameTest, KeepsLatinAndCleansSeparators)
{
    EXPECT_EQ(suggestUsername("Ivan Petrov", ""), "ivan_petrov");
    EXPECT_EQ(suggestUsername("  vova.shar-2016!! ", ""), "vova_shar_2016");
    EXPECT_EQ(suggestUsername("Анна 😀 Ли", ""), "anna_li");  // эмодзи отбрасываются
}

TEST(SuggestUsernameTest, ResultFollowsUsernameRules)
{
    const std::string name =
        suggestUsername("Очень Длинное Имя Пользователя Из Профиля Провайдера", "");
    EXPECT_LE(name.size(), 30U);
    EXPECT_FALSE(name.ends_with('_'));
    EXPECT_EQ(name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_"), std::string::npos)
        << name;
}

TEST(SuggestUsernameTest, FallsBackToEmailThenUser)
{
    EXPECT_EQ(suggestUsername("Ли", "ivan.petrov@example.com"), "ivan_petrov");
    EXPECT_EQ(suggestUsername("", "ab@example.com"), "user");
    EXPECT_EQ(suggestUsername("😀", ""), "user");
}

TEST(ProviderTitleTest, NamesProvidersForMessages)
{
    EXPECT_EQ(providerTitle("yandex"), "Яндекс");
    EXPECT_EQ(providerTitle("google"), "Google");
    EXPECT_EQ(providerTitle("vk"), "VK");
}
