#include "auth/password_rules.hpp"

#include "auth/crypto.hpp"

namespace
{
FieldError sameAsCurrent()
{
    return {.code = "AUTH_PASSWORD_SAME_AS_CURRENT",
            .message = "Новый пароль должен отличаться от текущего",
            .field = "new_password"};
}
}  // namespace

FieldError currentPasswordInvalid()
{
    return {.code = "AUTH_CURRENT_PASSWORD_INVALID",
            .message = "Неверный текущий пароль",
            .field = "current_password"};
}

std::optional<FieldError> checkResetPassword(std::string_view newPassword,
                                             const std::optional<std::string> &currentHash)
{
    if (auto error = validateNewPassword(newPassword))
    {
        return error;
    }
    // Текущий пароль хранится только хешем: сравнить с ним - значит проверить новый по этому хешу
    if (currentHash && verifyPassword(newPassword, *currentHash))
    {
        return sameAsCurrent();
    }
    return std::nullopt;
}

std::optional<FieldError> checkPasswordChange(std::string_view currentPassword,
                                              std::string_view newPassword,
                                              const std::optional<std::string> &currentHash)
{
    if (currentHash)
    {
        if (currentPassword.empty())
        {
            return FieldError{.code = "AUTH_VALIDATION_ERROR",
                              .message = "Введите текущий пароль",
                              .field = "current_password"};
        }
        // Длиннее 128 символов паролей не бывает: такую строку не хешируем
        if (utf8Length(currentPassword) > 128 || !verifyPassword(currentPassword, *currentHash))
        {
            return currentPasswordInvalid();
        }
    }
    if (auto error = validateNewPassword(newPassword))
    {
        return error;
    }
    // Текущий пароль уже проверен: совпадение видно сравнением строк, без второго Argon2
    if (currentHash && newPassword == currentPassword)
    {
        return sameAsCurrent();
    }
    return std::nullopt;
}
