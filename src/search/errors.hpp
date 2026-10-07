#pragma once

#include <string_view>

// Виды поиска и тексты ошибок поиска из контракта

enum class MediaKind
{
    Image,
    Video
};

enum class SearchError
{
    FileRequired,
    TooManyFiles,
    FileTooLarge,
    UnsupportedFormat,
    VideoTooLong,
    FileCorrupted,
    Unavailable,
    Busy
};

struct ErrorReply
{
    int status;
    std::string_view code;
    std::string_view message;
    bool fileField;
};

// ML может прислать DURATION_TOO_LONG и для картинки; у эндпоинта картинки такой ошибки нет
[[nodiscard]] SearchError forKind(MediaKind kind, SearchError error);

[[nodiscard]] ErrorReply errorReply(MediaKind kind, SearchError error);
