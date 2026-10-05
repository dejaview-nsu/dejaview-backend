#include "auth/login_rules.hpp"

#include <gtest/gtest.h>

// Пороги #17149 п. 2.5: с 3-й неудачи подряд - CAPTCHA, после 5-й - блокировка на 15 минут.

TEST(LoginGateTest, AllowsFirstAttempts)
{
    for (const int failed : {0, 1, 2})
    {
        SCOPED_TRACE(failed);
        EXPECT_EQ(loginGate(failed, 0), LoginGate::Allowed);
    }
}

TEST(LoginGateTest, RequiresCaptchaFromThirdFailure)
{
    for (const int failed : {3, 4})
    {
        SCOPED_TRACE(failed);
        EXPECT_EQ(loginGate(failed, 0), LoginGate::CaptchaRequired);
    }
}

TEST(LoginGateTest, LockWinsOverEverything)
{
    // пока идёт блокировка, пароль не проверяется и CAPTCHA не предлагается
    EXPECT_EQ(loginGate(5, 900), LoginGate::Locked);
    EXPECT_EQ(loginGate(0, 1), LoginGate::Locked);
}

TEST(AfterFailedLoginTest, CountsUpToLock)
{
    EXPECT_FALSE(afterFailedLogin(1).captchaRequired);
    EXPECT_FALSE(afterFailedLogin(2).captchaRequired);
    // 3-я неудача: в ответе captcha_required, следующая попытка - с CAPTCHA
    EXPECT_TRUE(afterFailedLogin(3).captchaRequired);
    EXPECT_FALSE(afterFailedLogin(4).locked);
    // 5-я неудача сразу блокирует вход (описание POST /auth/login, шаг 3)
    EXPECT_TRUE(afterFailedLogin(5).locked);
}

TEST(LoginLockedMessageTest, RoundsMinutesUp)
{
    EXPECT_EQ(loginLockedMessage(900), "Слишком много попыток входа. Повторите через 15 минут");
    EXPECT_EQ(loginLockedMessage(840), "Слишком много попыток входа. Повторите через 14 минут");
    EXPECT_EQ(loginLockedMessage(61), "Слишком много попыток входа. Повторите через 2 минуты");
    EXPECT_EQ(loginLockedMessage(1), "Слишком много попыток входа. Повторите через 1 минуту");
}

TEST(LoginLockedMessageTest, AgreesWordWithNumber)
{
    const std::string prefix = "Слишком много попыток входа. Повторите через ";
    EXPECT_EQ(loginLockedMessage(60), prefix + "1 минуту");
    EXPECT_EQ(loginLockedMessage(3 * 60), prefix + "3 минуты");
    EXPECT_EQ(loginLockedMessage(5 * 60), prefix + "5 минут");
    EXPECT_EQ(loginLockedMessage(11 * 60), prefix + "11 минут");
    EXPECT_EQ(loginLockedMessage(12 * 60), prefix + "12 минут");
    EXPECT_EQ(loginLockedMessage(21 * 60), prefix + "21 минуту");
    EXPECT_EQ(loginLockedMessage(22 * 60), prefix + "22 минуты");
}

TEST(InvalidCredentialsMessageTest, DependsOnLoginKind)
{
    EXPECT_EQ(invalidCredentialsMessage("ivan@example.com"), "Неверные email или пароль");
    EXPECT_EQ(invalidCredentialsMessage("movie_fan_42"), "Неверные имя пользователя или пароль");
}
