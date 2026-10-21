#include "search/errors.hpp"

#include <utility>

namespace
{

constexpr std::string_view kUnavailable = "Поиск временно недоступен. Попробуйте позже";
constexpr std::string_view kBusy = "Сервис перегружен. Попробуйте позже";
constexpr std::string_view kFile = "file";
constexpr std::string_view kQuery = "query";

bool isFileError(SearchError error)
{
    return error >= SearchError::FileRequired && error <= SearchError::FileCorrupted;
}

bool isQueryError(SearchError error)
{
    return error >= SearchError::QueryEmpty && error <= SearchError::InvalidQuery;
}

// Текст ошибки файла: у каждого вида поиска свой префикс и свои пределы
std::string_view forFile(MediaKind kind, std::string_view image, std::string_view video,
                         std::string_view audio)
{
    switch (kind)
    {
        case MediaKind::Image:
            return image;
        case MediaKind::Video:
            return video;
        case MediaKind::Audio:
            return audio;
        case MediaKind::Text:
            break;
    }
    std::unreachable();
}

}  // namespace

SearchError forKind(MediaKind kind, SearchError error)
{
    if (kind == MediaKind::Text)
    {
        return isFileError(error) ? SearchError::Unavailable : error;
    }
    const bool outsideTable =
        isQueryError(error) || (kind == MediaKind::Image && error == SearchError::DurationTooLong);
    return outsideTable ? SearchError::Unavailable : error;
}

ErrorReply errorReply(MediaKind kind, SearchError error)
{
    switch (forKind(kind, error))
    {
        case SearchError::FileRequired:
            return {
                400, "FILE_REQUIRED",
                forFile(kind, "Невозможно выполнить поиск по изображению: изображение не загружено",
                        "Невозможно выполнить поиск по видеофрагменту: видеофрагмент не "
                        "загружен",
                        "Невозможно выполнить поиск по аудиофрагменту: аудиофрагмент не "
                        "загружен и не записан"),
                kFile};
        case SearchError::TooManyFiles:
            return {
                400, "TOO_MANY_FILES",
                forFile(kind,
                        "Невозможно выполнить поиск по изображению: загружено больше одного файла",
                        "Невозможно выполнить поиск по видеофрагменту: загружено больше одного "
                        "файла",
                        "Невозможно выполнить поиск по аудиофрагменту: загружено больше одного "
                        "файла"),
                kFile};
        case SearchError::FileTooLarge:
            return {413, "FILE_TOO_LARGE",
                    forFile(kind,
                            "Невозможно выполнить поиск по изображению: размер файла больше 10 МБ",
                            "Невозможно выполнить поиск по видеофрагменту: размер файла больше "
                            "50 МБ",
                            "Невозможно выполнить поиск по аудиофрагменту: размер файла больше "
                            "20 МБ"),
                    kFile};
        case SearchError::UnsupportedFormat:
            return {415, "UNSUPPORTED_FORMAT",
                    forFile(kind,
                            "Невозможно выполнить поиск по изображению: поддерживаются только "
                            "JPG, JPEG, PNG и WEBP",
                            "Невозможно выполнить поиск по видеофрагменту: поддерживаются только "
                            "MP4, MOV и WEBM",
                            "Невозможно выполнить поиск по аудиофрагменту: поддерживаются только "
                            "MP3, WAV, OGG и M4A"),
                    kFile};
        case SearchError::DurationTooLong:
            if (kind == MediaKind::Audio)
            {
                return {422, "AUDIO_TOO_LONG",
                        "Невозможно выполнить поиск по аудиофрагменту: аудиофрагмент длиннее 10 "
                        "секунд",
                        kFile};
            }
            return {422, "VIDEO_TOO_LONG",
                    "Невозможно выполнить поиск по видеофрагменту: видео длиннее 30 секунд", kFile};
        case SearchError::FileCorrupted:
            return {422, "FILE_CORRUPTED",
                    forFile(kind,
                            "Невозможно выполнить поиск по изображению: файл поврежден или не "
                            "может быть обработан",
                            "Невозможно выполнить поиск по видеофрагменту: файл поврежден или не "
                            "может быть обработан",
                            "Невозможно выполнить поиск по аудиофрагменту: файл поврежден или не "
                            "может быть обработан"),
                    kFile};
        case SearchError::QueryEmpty:
            return {400, "QUERY_EMPTY", "Невозможно выполнить поиск по запросу: запрос пустой",
                    kQuery};
        case SearchError::QueryTooShort:
            return {422, "QUERY_TOO_SHORT",
                    "Невозможно выполнить поиск по запросу: запрос короче 3 символов", kQuery};
        case SearchError::QueryTooLong:
            return {422, "QUERY_TOO_LONG",
                    "Невозможно выполнить поиск по запросу: запрос длиннее 250 символов", kQuery};
        case SearchError::InvalidQuery:
        case SearchError::Unavailable:
            return {503, "SEARCH_UNAVAILABLE", kUnavailable, ""};
        case SearchError::Busy:
            return {503, "SERVER_BUSY", kBusy, ""};
    }
    std::unreachable();
}
