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
