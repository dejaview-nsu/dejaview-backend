#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

// Ошибка поля формы: из неё обработчик собирает тело Error (api/openapi.yaml).
struct FieldError
{
    std::string code;     // AUTH_VALIDATION_ERROR, для года рождения - AUTH_BIRTH_YEAR_INVALID
    std::string message;  // текст для пользователя, дословно из контракта
    std::string field;    // username, email, birth_year, password, login, new_password
};

// Серверные проверки полей (#17148 п. 2.3-2.4, #17149 п. 2.3). Правила и тексты - в схемах
// AuthUsername, AuthEmail, AuthBirthYear, AuthPassword, AuthLogin в api/openapi.yaml.
// Каждая функция возвращает первую найденную ошибку или std::nullopt, если значение верное.
// Длина считается в символах, а не в байтах: «аб» - 2 символа, хотя в UTF-8 это 4 байта.

// 3-30 символов: латинские буквы, цифры и `_`.
std::optional<FieldError> validateUsername(std::string_view username);

// Адрес вида local@domain, не длиннее 254 символов. Полный RFC 5322 не нужен, достаточно:
// - ровно один `@`, слева от него от 1 до 64 символов, справа - домен;
// - слева: латинские буквы, цифры и символы !#$%&'*+/=?^_`{|}~.- ; точка не первая, не
//   последняя и не две подряд;
// - домен: части через точку, частей не меньше двух, часть - латинские буквы, цифры и `-`,
//   не пустая.
std::optional<FieldError> validateEmail(std::string_view email);

// От 1900 до текущего года. currentYear - параметр, а не системное время: так тест не
// сломается 1 января.
std::optional<FieldError> validateBirthYear(int birthYear, int currentYear);

// 8-128 символов, хотя бы одна заглавная и одна строчная латинская буква, цифра и специальный
// символ - любой, кроме латинской буквы и цифры (в том числе кириллица).
std::optional<FieldError> validatePassword(std::string_view password);

// Новый пароль в формах #17150: правила те же, поле new_password, пустой - «Введите новый пароль».
std::optional<FieldError> validateNewPassword(std::string_view password);

// Имя пользователя или email для входа: строка с `@` проверяется как email, без `@` - как имя.
std::optional<FieldError> validateLogin(std::string_view login);

// Число символов в строке UTF-8, а не байтов: в «аб» 2 символа и 4 байта.
std::size_t utf8Length(std::string_view text);
