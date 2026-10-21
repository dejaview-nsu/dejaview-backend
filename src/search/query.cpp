#include "search/query.hpp"

#include <cstdint>

#include <unicode/uchar.h>
#include <unicode/utf8.h>

std::expected<Query, SearchError> checkQuery(std::string_view raw)
{
    // Весь запрос проверяется на UTF-8: ML не должен получить невалидные байты
    std::size_t begin = 0;
    std::size_t end = 0;
    std::size_t length = 0;
    std::size_t seen = 0;  // символы с первого непробельного
    bool started = false;
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(raw.data());
    for (std::size_t pos = 0; pos < raw.size();)
    {
        const std::size_t first = pos;
        UChar32 point = 0;
        // Строгий UTF-8 по RFC 3629: без «коротких» форм, суррогатов и значений выше U+10FFFF
        U8_NEXT(bytes, pos, raw.size(), point);
        if (point < 0)
        {
            return std::unexpected(SearchError::QueryEmpty);
        }
        // Свойство Unicode White_Space
        if (!u_isUWhiteSpace(point))
        {
            if (!started)
            {
                started = true;
                begin = first;
            }
            end = pos;
            length = seen + 1;
        }
        if (started)
        {
            ++seen;
        }
    }
    if (!started)
    {
        return std::unexpected(SearchError::QueryEmpty);
    }
    if (length < kMinQueryChars)
    {
        return std::unexpected(SearchError::QueryTooShort);
    }
    if (length > kMaxQueryChars)
    {
        return std::unexpected(SearchError::QueryTooLong);
    }
    return Query{raw.substr(begin, end - begin), length};
}

SearchError resolveInvalidQuery(SearchError error, std::size_t length)
{
    if (error != SearchError::InvalidQuery)
    {
        return error;
    }
    return length * 2 <= kMinQueryChars + kMaxQueryChars ? SearchError::QueryTooShort
                                                         : SearchError::QueryTooLong;
}
