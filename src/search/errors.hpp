#pragma once

#include <string_view>

// Виды поиска и тексты ошибок поиска из контракта

enum class MediaKind
{
    Image,
    Video,
    Audio,
    Text
};

enum class SearchError
{
    FileRequired,
    TooManyFiles,
    FileTooLarge,
    UnsupportedFormat,
    DurationTooLong,
    FileCorrupted,
    QueryEmpty,
    QueryTooShort,
    QueryTooLong,
    InvalidQuery,
    Unavailable,
    Busy
};

struct ErrorReply
{
    int status;
    std::string_view code;
    std::string_view message;
    std::string_view field;
};

// ML может прислать ошибку, которой нет в таблице эндпоинта (DURATION_TOO_LONG для картинки,
// файловую для текста): это не авария, а 503. InvalidQuery остаётся только у текста - её
// превращает в QueryTooShort/QueryTooLong обработчик текста
[[nodiscard]] SearchError forKind(MediaKind kind, SearchError error);

[[nodiscard]] ErrorReply errorReply(MediaKind kind, SearchError error);
