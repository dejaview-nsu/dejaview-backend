#include "search/errors.hpp"

#include <utility>

namespace
{

constexpr std::string_view kUnavailable = "Поиск временно недоступен. Попробуйте позже";
constexpr std::string_view kBusy = "Сервис перегружен. Попробуйте позже";

}  // namespace

SearchError forKind(MediaKind kind, SearchError error)
{
    if (kind == MediaKind::Image && error == SearchError::VideoTooLong)
    {
        return SearchError::Unavailable;
    }
    return error;
}

ErrorReply errorReply(MediaKind kind, SearchError error)
{
    const bool image = kind == MediaKind::Image;

    switch (forKind(kind, error))
    {
        case SearchError::FileRequired:
            return {400, "FILE_REQUIRED",
                    image
                        ? "Невозможно выполнить поиск по изображению: изображение не загружено"
                        : "Невозможно выполнить поиск по видеофрагменту: видеофрагмент не загружен",
                    true};
        case SearchError::TooManyFiles:
            return {
                400, "TOO_MANY_FILES",
                image
                    ? "Невозможно выполнить поиск по изображению: загружено больше одного файла"
                    : "Невозможно выполнить поиск по видеофрагменту: загружено больше одного файла",
                true};
        case SearchError::FileTooLarge:
            return {413, "FILE_TOO_LARGE",
                    image
                        ? "Невозможно выполнить поиск по изображению: размер файла больше 10 МБ"
                        : "Невозможно выполнить поиск по видеофрагменту: размер файла больше 50 МБ",
                    true};
        case SearchError::UnsupportedFormat:
            return {415, "UNSUPPORTED_FORMAT",
                    image ? "Невозможно выполнить поиск по изображению: поддерживаются только JPG, "
                            "JPEG, PNG и WEBP"
                          : "Невозможно выполнить поиск по видеофрагменту: поддерживаются только "
                            "MP4, MOV и WEBM",
                    true};
        case SearchError::VideoTooLong:
            return {422, "VIDEO_TOO_LONG",
                    "Невозможно выполнить поиск по видеофрагменту: видео длиннее 30 секунд", true};
        case SearchError::FileCorrupted:
            return {422, "FILE_CORRUPTED",
                    image ? "Невозможно выполнить поиск по изображению: файл поврежден или не "
                            "может быть обработан"
                          : "Невозможно выполнить поиск по видеофрагменту: файл поврежден или не "
                            "может быть обработан",
                    true};
        case SearchError::Unavailable:
            return {503, "SEARCH_UNAVAILABLE", kUnavailable, false};
        case SearchError::Busy:
            return {503, "SERVER_BUSY", kBusy, false};
    }
    std::unreachable();
}
