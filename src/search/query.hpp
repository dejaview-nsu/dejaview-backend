#pragma once

#include <cstddef>
#include <expected>
#include <string_view>

#include "search/errors.hpp"

// Проверка текстового запроса поиска без Drogon

constexpr std::size_t kMinQueryChars = 3;
constexpr std::size_t kMaxQueryChars = 250;

struct Query
{
    std::string_view text;  // без пробелов по краям, view в исходную строку
    std::size_t length;     // в символах Unicode (кодовых точках)
};

// Не UTF-8 или пусто после обрезки -> QueryEmpty, затем
// QueryTooShort, затем QueryTooLong
[[nodiscard]] std::expected<Query, SearchError> checkQuery(std::string_view raw);

// ML сам посчитал запрос неверным: это расхождение в подсчёте длины, берём ближайшую границу
[[nodiscard]] SearchError resolveInvalidQuery(SearchError error, std::size_t length);
