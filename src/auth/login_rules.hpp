#pragma once

#include <string>
#include <string_view>

// Защита от подбора пароля (#17149 п. 2.5, #17094 п. 3.3). Чистая логика без БД и HTTP:
// состояние учётной записи приходит из users, решения применяет обработчик входа.

constexpr int kCaptchaAfterFailures = 3;  // с 3-й неудачной попытки подряд - CAPTCHA
constexpr int kLockAfterFailures = 5;     // после 5-й - блокировка
constexpr int kLockSeconds = 15 * 60;     // на 15 минут

// Что делать с попыткой входа до проверки пароля.
enum class LoginGate
{
    Allowed,          // проверять пароль
    CaptchaRequired,  // сначала CAPTCHA
    Locked            // пароль не проверять, ответ 429
};

// failedCount - неудачных попыток подряд, lockSeconds - сколько секунд осталось до конца
// блокировки (0 - не заблокирован). Блокировка важнее CAPTCHA.
LoginGate loginGate(int failedCount, int lockSeconds);

// Итог неверного пароля по новому значению счётчика.
struct FailedLogin
{
    bool locked;           // это была 5-я неудача: сразу 429
    bool captchaRequired;  // следующей попытке нужна CAPTCHA (captcha_required в ответе)
};
FailedLogin afterFailedLogin(int failedCount);

// «Слишком много попыток входа. Повторите через {N} минут»: N с округлением вверх, слово
// «минут» согласовано с числом - «1 минуту», «3 минуты», «14 минут».
std::string loginLockedMessage(int lockSeconds);

// Текст неверных учётных данных по виду login (#17149 п. 2.4): с `@` - про email, без - про имя.
std::string invalidCredentialsMessage(std::string_view login);
