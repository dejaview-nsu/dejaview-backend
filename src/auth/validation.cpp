#include "auth/validation.hpp"

#include <algorithm>
#include <cstddef>
#include <ranges>
#include <string>
#include <string_view>

namespace
{
constexpr std::string_view kValidationError = "AUTH_VALIDATION_ERROR";

constexpr std::string_view kUsernameField = "username";
constexpr std::string_view kEmailField = "email";
constexpr std::string_view kBirthYearField = "birth_year";
constexpr std::string_view kPasswordField = "password";
constexpr std::string_view kLoginField = "login";

FieldError makeError(std::string_view code, std::string_view message, std::string_view field)
{
    return FieldError{
        .code = std::string(code), .message = std::string(message), .field = std::string(field)};
}

FieldError validationError(std::string_view message, std::string_view field)
{
    return makeError(kValidationError, message, field);
}

// Число символов UTF-8: байты продолжения (10xxxxxx) не считаются, у каждого символа ровно
// один начальный байт.
std::size_t utf8Length(std::string_view text)
{
    return std::ranges::count_if(
        text, [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; });
}

bool isUpper(char c) { return c >= 'A' && c <= 'Z'; }

bool isLower(char c) { return c >= 'a' && c <= 'z'; }

bool isDigit(char c) { return c >= '0' && c <= '9'; }

bool isLatinOrDigit(char c) { return isUpper(c) || isLower(c) || isDigit(c); }

bool isUsernameChar(char c) { return isLatinOrDigit(c) || c == '_'; }

bool isEmailLocalChar(char c)
{
    constexpr std::string_view kSpecial = "!#$%&'*+/=?^_`{|}~.-";
    return isLatinOrDigit(c) || kSpecial.contains(c);
}

bool isDomainChar(char c) { return isLatinOrDigit(c) || c == '-'; }

// Часть до @: 1-64 символа, точка не первая, не последняя и не две подряд.
bool isValidLocalPart(std::string_view local)
{
    return !local.empty() && local.size() <= 64 && std::ranges::all_of(local, isEmailLocalChar) &&
           !local.starts_with('.') && !local.ends_with('.') && !local.contains("..");
}

// Часть после @: не меньше двух непустых частей через точку. split отдаёт и пустые части
// («a..b», «a.»), поэтому пустую часть проверяем явно.
bool isValidDomain(std::string_view domain)
{
    int parts = 0;
    for (const auto part : domain | std::views::split('.'))
    {
        if (part.empty() || !std::ranges::all_of(part, isDomainChar))
        {
            return false;
        }
        ++parts;
    }
    return parts >= 2;
}
}  // namespace

std::optional<FieldError> validateUsername(std::string_view username)
{
    if (username.empty())
    {
        return validationError("Введите имя пользователя", kUsernameField);
    }
    const std::size_t length = utf8Length(username);
    if (length < 3)
    {
        return validationError("Имя пользователя должно содержать не менее 3 символов",
                               kUsernameField);
    }
    if (length > 30)
    {
        return validationError("Имя пользователя должно содержать не более 30 символов",
                               kUsernameField);
    }
    if (!std::ranges::all_of(username, isUsernameChar))
    {
        return validationError(
            "Имя пользователя может содержать только латинские буквы, цифры и "
            "нижнее подчёркивание",
            kUsernameField);
    }
    return std::nullopt;
}

std::optional<FieldError> validateEmail(std::string_view email)
{
    if (email.empty())
    {
        return validationError("Введите email", kEmailField);
    }
    // Допустимы только ASCII-символы, поэтому байты здесь равны символам.
    const std::size_t at = email.find('@');
    if (email.size() > 254 || at == std::string_view::npos || email.substr(at + 1).contains('@') ||
        !isValidLocalPart(email.substr(0, at)) || !isValidDomain(email.substr(at + 1)))
    {
        return validationError("Введите корректный email", kEmailField);
    }
    return std::nullopt;
}

std::optional<FieldError> validateBirthYear(int birthYear, int currentYear)
{
    if (birthYear < 1900 || birthYear > currentYear)
    {
        return makeError("AUTH_BIRTH_YEAR_INVALID", "Введите корректный год рождения",
                         kBirthYearField);
    }
    return std::nullopt;
}

std::optional<FieldError> validatePassword(std::string_view password)
{
    if (password.empty())
    {
        return validationError("Введите пароль", kPasswordField);
    }
    const std::size_t length = utf8Length(password);
    if (length < 8)
    {
        return validationError("Пароль должен содержать не менее 8 символов", kPasswordField);
    }
    if (length > 128)
    {
        return validationError("Пароль должен содержать не более 128 символов", kPasswordField);
    }

    bool hasUpper = false;
    bool hasLower = false;
    bool hasDigit = false;
    bool hasSpecial = false;  // всё, кроме латиницы и цифр: пробел, кириллица, знаки
    for (const char c : password)
    {
        hasUpper = hasUpper || isUpper(c);
        hasLower = hasLower || isLower(c);
        hasDigit = hasDigit || isDigit(c);
        hasSpecial = hasSpecial || !isLatinOrDigit(c);
    }

    if (!hasUpper || !hasLower || !hasDigit)
    {
        return validationError(
            "Пароль должен содержать хотя бы одну заглавную букву, одну "
            "строчную букву и одну цифру",
            kPasswordField);
    }
    if (!hasSpecial)
    {
        return validationError(
            "Пароль должен содержать хотя бы одну заглавную букву, одну "
            "строчную букву, одну цифру и один специальный символ",
            kPasswordField);
    }
    return std::nullopt;
}

std::optional<FieldError> validateLogin(std::string_view login)
{
    if (login.empty())
    {
        return validationError("Введите имя пользователя или email", kLoginField);
    }
    const auto error = login.contains('@') ? validateEmail(login) : validateUsername(login);
    if (error)
    {
        return validationError("Введите корректные имя пользователя или email", kLoginField);
    }
    return std::nullopt;
}
