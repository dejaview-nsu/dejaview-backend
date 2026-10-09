#include "auth/password_rules.hpp"

#include "auth/crypto.hpp"

#include <gtest/gtest.h>

TEST(CheckResetPasswordTest, AcceptsNewPassword)
{
    EXPECT_EQ(checkResetPassword("Kino#2027", hashPassword("Kino#2026")), std::nullopt);
}

TEST(CheckResetPasswordTest, RejectsCurrentPassword)
{
    const auto error = checkResetPassword("Kino#2026", hashPassword("Kino#2026"));
    ASSERT_TRUE(error);
    EXPECT_EQ(error->code, "AUTH_PASSWORD_SAME_AS_CURRENT");
    EXPECT_EQ(error->message, "Новый пароль должен отличаться от текущего");
    EXPECT_EQ(error->field, "new_password");
}

TEST(CheckResetPasswordTest, RulesComeBeforeComparison)
{
    const auto error = checkResetPassword("", hashPassword("Kino#2026"));
    ASSERT_TRUE(error);
    EXPECT_EQ(error->message, "Введите новый пароль");
}

TEST(CheckResetPasswordTest, AccountWithoutPasswordHasNothingToCompare)
{
    // учётная запись из OIDC: сброс по ссылке задаёт ей первый пароль
    EXPECT_EQ(checkResetPassword("Kino#2026", std::nullopt), std::nullopt);
}

// Порядок ответов - порядок полей формы #17150 п. 3.1: сначала текущий пароль, потом новый
TEST(CheckPasswordChangeTest, ChecksCurrentPasswordFirst)
{
    const std::string hash = hashPassword("Kino#2026");
    const auto empty = checkPasswordChange("", "short", hash);
    ASSERT_TRUE(empty);
    EXPECT_EQ(empty->message, "Введите текущий пароль");
    EXPECT_EQ(empty->field, "current_password");

    for (const std::string wrong : {std::string("Kino#2025"), std::string(200, 'a')})
    {
        const auto error = checkPasswordChange(wrong, "short", hash);
        ASSERT_TRUE(error);
        EXPECT_EQ(error->code, "AUTH_CURRENT_PASSWORD_INVALID");
        EXPECT_EQ(error->field, "current_password");
    }
}

TEST(CheckPasswordChangeTest, ThenNewPassword)
{
    const std::string hash = hashPassword("Kino#2026");
    const auto weak = checkPasswordChange("Kino#2026", "short", hash);
    ASSERT_TRUE(weak);
    EXPECT_EQ(weak->code, "AUTH_VALIDATION_ERROR");
    EXPECT_EQ(weak->field, "new_password");

    const auto same = checkPasswordChange("Kino#2026", "Kino#2026", hash);
    ASSERT_TRUE(same);
    EXPECT_EQ(same->code, "AUTH_PASSWORD_SAME_AS_CURRENT");

    EXPECT_EQ(checkPasswordChange("Kino#2026", "Kino#2027", hash), std::nullopt);
}

TEST(CheckPasswordChangeTest, AccountWithoutPasswordNeedsOnlyNewOne)
{
    // учётная запись из OIDC: поля «Текущий пароль» нет, операция задаёт пароль (#17150 п. 3.1)
    EXPECT_EQ(checkPasswordChange("", "Kino#2027", std::nullopt), std::nullopt);
    const auto weak = checkPasswordChange("", "short", std::nullopt);
    ASSERT_TRUE(weak);
    EXPECT_EQ(weak->field, "new_password");
}
