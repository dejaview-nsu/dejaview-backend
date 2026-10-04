#pragma once

#include <json/value.h>

#include <optional>
#include <string>
#include <string_view>

// Данные пользователя от OIDC-провайдера и что из них следует. Чистая логика без сети и БД.

struct OidcProfile
{
    std::string subject;      // id пользователя у провайдера, не меняется (claim sub)
    std::string email;        // пустой - провайдер email не дал
    bool emailVerified;       // провайдер сам подтвердил, что адрес принадлежит пользователю
    std::string displayName;  // имя профиля: из него предлагается имя пользователя
};

// Профиль из ответа Яндекс ID (login.yandex.ru/info). nullopt - в ответе нет id.
std::optional<OidcProfile> parseYandexProfile(const Json::Value &info);

// Профиль из стандартного OIDC userinfo Google (openidconnect.googleapis.com/v1/userinfo):
// sub, email, email_verified, name. nullopt - в ответе нет sub.
std::optional<OidcProfile> parseGoogleProfile(const Json::Value &info);

// Название провайдера для текстов ошибок: «Не удалось выполнить вход через Яндекс».
std::string_view providerTitle(std::string_view provider);

// Имя пользователя из имени профиля (#17148 п. 3.2 шаг 7, схема AuthOidcPending): кириллица
// транслитерируется, пробелы и знаки становятся `_`, остальное отбрасывается, длина - до 30.
// Не вышло 3 символов - берётся начало email, не вышло и так - «user». Занято ли имя, решает
// обработчик: он добавит случайный числовой суффикс.
std::string suggestUsername(std::string_view displayName, std::string_view email);
