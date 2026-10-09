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

// Смена пароля из профиля (#17150 п. 3.2) в порядке полей формы: текущий пароль, затем новый.
// Без пароля (учётная запись из OIDC) текущий не нужен - операция задаёт пароль (п. 3.1).
std::optional<FieldError> checkPasswordChange(std::string_view currentPassword,
                                              std::string_view newPassword,
                                              const std::optional<std::string> &currentHash);

// «Неверный текущий пароль»: и при неверном вводе, и если пароль успели сменить параллельно.
FieldError currentPasswordInvalid();
