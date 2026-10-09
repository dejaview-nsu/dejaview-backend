#include "auth/password_rules.hpp"

#include "auth/crypto.hpp"

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
        return FieldError{.code = "AUTH_PASSWORD_SAME_AS_CURRENT",
                          .message = "Новый пароль должен отличаться от текущего",
                          .field = "new_password"};
    }
    return std::nullopt;
}
