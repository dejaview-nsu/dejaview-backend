#include "auth/validation.hpp"

#include <gtest/gtest.h>

#include <string>

namespace
{
// Текст ошибки или "", если ошибки нет: так каждая проверка помещается в одну строку.
std::string messageOf(const std::optional<FieldError> &error)
{
    return error ? error->message : "";
}

std::string repeat(std::string_view piece, int times)
{
    std::string result;
    for (int i = 0; i < times; ++i)
    {
        result += piece;
    }
    return result;
}

const std::string kUsernameChars =
    "Имя пользователя может содержать только латинские буквы, цифры и нижнее подчёркивание";
const std::string kEmailInvalid = "Введите корректный email";
const std::string kPasswordClasses =
    "Пароль должен содержать хотя бы одну заглавную букву, одну строчную букву и одну цифру";
const std::string kPasswordSpecial =
    "Пароль должен содержать хотя бы одну заглавную букву, одну строчную букву, одну цифру и "
    "один специальный символ";
const std::string kLoginInvalid = "Введите корректные имя пользователя или email";
}  // namespace

// --- Имя пользователя ---

TEST(ValidateUsernameTest, AcceptsValid)
{
    for (const char *username :
         {"abc", "movie_fan_42", "Ivan", "___", "a23456789012345678901234567890"})
    {
        SCOPED_TRACE(username);
        EXPECT_EQ(validateUsername(username), std::nullopt);
    }
}

TEST(ValidateUsernameTest, ErrorHasCodeAndField)
{
    const auto error = validateUsername("");
    ASSERT_TRUE(error);
    EXPECT_EQ(error->code, "AUTH_VALIDATION_ERROR");
    EXPECT_EQ(error->field, "username");
}

TEST(ValidateUsernameTest, RejectsEmpty)
{
    EXPECT_EQ(messageOf(validateUsername("")), "Введите имя пользователя");
}

TEST(ValidateUsernameTest, RejectsShort)
{
    EXPECT_EQ(messageOf(validateUsername("ab")),
              "Имя пользователя должно содержать не менее 3 символов");
}

TEST(ValidateUsernameTest, CountsCharactersNotBytes)
{
    // 2 символа, но 4 байта: сначала срабатывает длина, а не недопустимые символы
    EXPECT_EQ(messageOf(validateUsername("аб")),
              "Имя пользователя должно содержать не менее 3 символов");
}

TEST(ValidateUsernameTest, RejectsLong)
{
    EXPECT_EQ(messageOf(validateUsername(repeat("a", 31))),
              "Имя пользователя должно содержать не более 30 символов");
}

TEST(ValidateUsernameTest, RejectsInvalidCharacters)
{
    for (const char *username : {"ivan petrov", "ivan-petrov", "иван", "ivan.p", "ivan@"})
    {
        SCOPED_TRACE(username);
        EXPECT_EQ(messageOf(validateUsername(username)), kUsernameChars);
    }
}

// --- Email ---

TEST(ValidateEmailTest, AcceptsValid)
{
    for (const char *email : {"ivan@example.com", "Ivan.Petrov+kino@mail.example.ru", "a@b.co",
                              "o'neil@sub-domain.example.org"})
    {
        SCOPED_TRACE(email);
        EXPECT_EQ(validateEmail(email), std::nullopt);
    }
}

TEST(ValidateEmailTest, ErrorHasCodeAndField)
{
    const auto error = validateEmail("ivan");
    ASSERT_TRUE(error);
    EXPECT_EQ(error->code, "AUTH_VALIDATION_ERROR");
    EXPECT_EQ(error->field, "email");
}

TEST(ValidateEmailTest, RejectsEmpty) { EXPECT_EQ(messageOf(validateEmail("")), "Введите email"); }

TEST(ValidateEmailTest, RejectsInvalidFormat)
{
    for (const char *email :
         {"ivan", "ivan@", "@example.com", "ivan@example", "iv an@example.com", "ivan@@example.com",
          "ivan@a@example.com", ".ivan@example.com", "ivan.@example.com", "iv..an@example.com",
          "ivan@example..com", "ivan@.example.com", "ivan@example.com.", "ivan@exa_mple.com",
          "иван@example.com", "ivan@пример.рф"})
    {
        SCOPED_TRACE(email);
        EXPECT_EQ(messageOf(validateEmail(email)), kEmailInvalid);
    }
}

TEST(ValidateEmailTest, LimitsLocalPartTo64)
{
    EXPECT_EQ(validateEmail(repeat("a", 64) + "@example.com"), std::nullopt);
    EXPECT_EQ(messageOf(validateEmail(repeat("a", 65) + "@example.com")), kEmailInvalid);
}

TEST(ValidateEmailTest, LimitsLengthTo254)
{
    // 64 + 1 + 189 = 254 символа: части домена по 63 символа, как разрешает DNS
    const std::string domain = repeat("a", 63) + "." + repeat("b", 63) + "." + repeat("c", 61);
    const std::string email = repeat("x", 64) + "@" + domain;
    ASSERT_EQ(email.size(), 254U);
    EXPECT_EQ(validateEmail(email), std::nullopt);
    EXPECT_EQ(messageOf(validateEmail(email + "c")), kEmailInvalid);
}

// --- Год рождения ---

TEST(ValidateBirthYearTest, AcceptsRange)
{
    EXPECT_EQ(validateBirthYear(1900, 2026), std::nullopt);
    EXPECT_EQ(validateBirthYear(1998, 2026), std::nullopt);
    EXPECT_EQ(validateBirthYear(2026, 2026), std::nullopt);
}

TEST(ValidateBirthYearTest, RejectsOutOfRange)
{
    for (const int year : {1899, 2027, 0, -1998, 19980})
    {
        SCOPED_TRACE(year);
        const auto error = validateBirthYear(year, 2026);
        ASSERT_TRUE(error);
        EXPECT_EQ(error->code, "AUTH_BIRTH_YEAR_INVALID");
        EXPECT_EQ(error->message, "Введите корректный год рождения");
        EXPECT_EQ(error->field, "birth_year");
    }
}

// --- Пароль ---

TEST(ValidatePasswordTest, AcceptsValid)
{
    for (const char *password : {"Kino#2026", "Aa1!aaaa", "Пароль-Kino1", "Aa1 bbbb"})
    {
        SCOPED_TRACE(password);
        EXPECT_EQ(validatePassword(password), std::nullopt);
    }
}

TEST(ValidatePasswordTest, ErrorHasCodeAndField)
{
    const auto error = validatePassword("");
    ASSERT_TRUE(error);
    EXPECT_EQ(error->code, "AUTH_VALIDATION_ERROR");
    EXPECT_EQ(error->field, "password");
}

TEST(ValidatePasswordTest, RejectsEmpty)
{
    EXPECT_EQ(messageOf(validatePassword("")), "Введите пароль");
}

TEST(ValidatePasswordTest, RejectsShort)
{
    EXPECT_EQ(messageOf(validatePassword("Aa1!aaa")),
              "Пароль должен содержать не менее 8 символов");
}

TEST(ValidatePasswordTest, CountsCharactersNotBytes)
{
    // 7 символов, но 10 байт: «ббб» в UTF-8 по 2 байта
    EXPECT_EQ(messageOf(validatePassword("Aa1!ббб")),
              "Пароль должен содержать не менее 8 символов");
    // 8 символов, кириллица - специальный символ
    EXPECT_EQ(validatePassword("Aa1бббб!"), std::nullopt);
}

TEST(ValidatePasswordTest, LimitsLengthTo128)
{
    EXPECT_EQ(validatePassword("Aa1!" + repeat("a", 124)), std::nullopt);
    EXPECT_EQ(messageOf(validatePassword("Aa1!" + repeat("a", 125))),
              "Пароль должен содержать не более 128 символов");
    // 128 символов, но 252 байта
    EXPECT_EQ(validatePassword("Aa1!" + repeat("ж", 124)), std::nullopt);
}

TEST(ValidatePasswordTest, RequiresUpperLowerAndDigit)
{
    for (const char *password : {"aa1!aaaa", "AA1!AAAA", "Aaa!aaaa", "ПАРОЛЬ1!a"})
    {
        SCOPED_TRACE(password);
        EXPECT_EQ(messageOf(validatePassword(password)), kPasswordClasses);
    }
}

TEST(ValidatePasswordTest, RequiresSpecialCharacter)
{
    EXPECT_EQ(messageOf(validatePassword("Kino2026")), kPasswordSpecial);
}

// --- Логин: имя пользователя или email ---

TEST(ValidateLoginTest, AcceptsUsernameAndEmail)
{
    EXPECT_EQ(validateLogin("movie_fan_42"), std::nullopt);
    EXPECT_EQ(validateLogin("ivan@example.com"), std::nullopt);
}

TEST(ValidateLoginTest, RejectsEmpty)
{
    const auto error = validateLogin("");
    ASSERT_TRUE(error);
    EXPECT_EQ(error->code, "AUTH_VALIDATION_ERROR");
    EXPECT_EQ(error->message, "Введите имя пользователя или email");
    EXPECT_EQ(error->field, "login");
}

TEST(ValidateLoginTest, RejectsInvalid)
{
    // без @ - правила имени пользователя, с @ - правила email; текст ошибки всегда один
    for (const char *login : {"ab", "ivan petrov", "иван", "ivan@", "ivan@example"})
    {
        SCOPED_TRACE(login);
        const auto error = validateLogin(login);
        ASSERT_TRUE(error);
        EXPECT_EQ(error->message, kLoginInvalid);
        EXPECT_EQ(error->field, "login");
    }
}
