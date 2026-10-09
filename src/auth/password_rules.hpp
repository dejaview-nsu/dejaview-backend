#pragma once

#include "auth/validation.hpp"

#include <optional>
#include <string>
#include <string_view>

// Правила смены пароля (#17150) без БД и HTTP. currentHash - users.password_hash, nullopt - пароля
// нет (учётная запись из OIDC). Результат - первая ошибка или nullopt.

// Новый пароль по ссылке из письма (#17150 п. 2.3-2.4): правила AuthPassword, затем совпадение с
// текущим.
std::optional<FieldError> checkResetPassword(std::string_view newPassword,
                                             const std::optional<std::string> &currentHash);
