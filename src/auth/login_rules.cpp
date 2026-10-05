#include "auth/login_rules.hpp"

#include <format>

LoginGate loginGate(int failedCount, int lockSeconds)
{
    if (lockSeconds > 0)
    {
        return LoginGate::Locked;
    }
    return failedCount >= kCaptchaAfterFailures ? LoginGate::CaptchaRequired : LoginGate::Allowed;
}

FailedLogin afterFailedLogin(int failedCount)
{
    return {.locked = failedCount >= kLockAfterFailures,
            .captchaRequired = failedCount >= kCaptchaAfterFailures};
}

std::string loginLockedMessage(int lockSeconds)
{
    const int minutes = (lockSeconds + 59) / 60;  // вверх: осталось 61 с - «через 2 минуты»
    const int last = minutes % 10;
    const int lastTwo = minutes % 100;
    const char *word = "минут";  // 5-20: «через 15 минут»
    if (last == 1 && lastTwo != 11)
    {
        word = "минуту";  // «через 1 минуту», «через 21 минуту»
    }
    else if (last >= 2 && last <= 4 && (lastTwo < 12 || lastTwo > 14))
    {
        word = "минуты";  // «через 3 минуты»
    }
    return std::format("Слишком много попыток входа. Повторите через {} {}", minutes, word);
}

std::string invalidCredentialsMessage(std::string_view login)
{
    return login.contains('@') ? "Неверные email или пароль"
                               : "Неверные имя пользователя или пароль";
}
