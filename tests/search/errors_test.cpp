#include "search/errors.hpp"

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <string_view>

namespace
{

constexpr std::string_view kImagePrefix = "Невозможно выполнить поиск по изображению: ";
constexpr std::string_view kVideoPrefix = "Невозможно выполнить поиск по видеофрагменту: ";
constexpr std::string_view kAudioPrefix = "Невозможно выполнить поиск по аудиофрагменту: ";
constexpr std::string_view kTextPrefix = "Невозможно выполнить поиск по запросу: ";
constexpr std::string_view kUnavailableText = "Поиск временно недоступен. Попробуйте позже";

constexpr std::array<MediaKind, 4> kAllKinds{MediaKind::Image, MediaKind::Video, MediaKind::Audio,
                                             MediaKind::Text};

constexpr std::array<SearchError, 12> kAllErrors{
    SearchError::FileRequired,      SearchError::TooManyFiles,    SearchError::FileTooLarge,
    SearchError::UnsupportedFormat, SearchError::DurationTooLong, SearchError::FileCorrupted,
    SearchError::QueryEmpty,        SearchError::QueryTooShort,   SearchError::QueryTooLong,
    SearchError::InvalidQuery,      SearchError::Unavailable,     SearchError::Busy};

std::string_view prefixOf(MediaKind kind)
{
    switch (kind)
    {
        case MediaKind::Image:
            return kImagePrefix;
        case MediaKind::Video:
            return kVideoPrefix;
        case MediaKind::Audio:
            return kAudioPrefix;
        case MediaKind::Text:
            return kTextPrefix;
    }
    return {};
}

std::string prefixed(MediaKind kind, std::string_view tail)
{
    return std::string(prefixOf(kind)) + std::string(tail);
}

struct FileErrorRow
{
    SearchError error;
    int status;
    std::string_view code;
    std::string_view imageTail;
    std::string_view videoTail;
    std::string_view audioTail;
};

void expectReply(const ErrorReply &reply, int status, std::string_view code,
                 std::string_view message, std::string_view field)
{
    EXPECT_EQ(reply.status, status);
    EXPECT_EQ(reply.code, code);
    EXPECT_EQ(reply.message, message);
    EXPECT_EQ(reply.field, field);
}

bool isFileError(SearchError error)
{
    switch (error)
    {
        case SearchError::FileRequired:
        case SearchError::TooManyFiles:
        case SearchError::FileTooLarge:
        case SearchError::UnsupportedFormat:
        case SearchError::DurationTooLong:
        case SearchError::FileCorrupted:
            return true;
        default:
            return false;
    }
}

bool isQueryError(SearchError error)
{
    return error == SearchError::QueryEmpty || error == SearchError::QueryTooShort ||
           error == SearchError::QueryTooLong;
}

// Ожидаемая таблица forKind: что остаётся у эндпоинта, остальное - Unavailable.
SearchError expectedForKind(MediaKind kind, SearchError error)
{
    if (error == SearchError::Unavailable || error == SearchError::Busy)
    {
        return error;
    }
    switch (kind)
    {
        case MediaKind::Image:
            return (error == SearchError::DurationTooLong || isQueryError(error) ||
                    error == SearchError::InvalidQuery)
                       ? SearchError::Unavailable
                       : error;
        case MediaKind::Video:
        case MediaKind::Audio:
            return (isQueryError(error) || error == SearchError::InvalidQuery)
                       ? SearchError::Unavailable
                       : error;
        case MediaKind::Text:
            return isFileError(error) ? SearchError::Unavailable : error;
    }
    return SearchError::Unavailable;
}

}  // namespace

TEST(ErrorReplyTest, FileErrorsMatchContractTableForImageVideoAudio)
{
    const std::array<FileErrorRow, 5> rows{{
        {SearchError::FileRequired, 400, "FILE_REQUIRED", "изображение не загружено",
         "видеофрагмент не загружен", "аудиофрагмент не загружен и не записан"},
        {SearchError::TooManyFiles, 400, "TOO_MANY_FILES", "загружено больше одного файла",
         "загружено больше одного файла", "загружено больше одного файла"},
        {SearchError::FileTooLarge, 413, "FILE_TOO_LARGE", "размер файла больше 10 МБ",
         "размер файла больше 50 МБ", "размер файла больше 20 МБ"},
        {SearchError::UnsupportedFormat, 415, "UNSUPPORTED_FORMAT",
         "поддерживаются только JPG, JPEG, PNG и WEBP", "поддерживаются только MP4, MOV и WEBM",
         "поддерживаются только MP3, WAV, OGG и M4A"},
        {SearchError::FileCorrupted, 422, "FILE_CORRUPTED",
         "файл поврежден или не может быть обработан", "файл поврежден или не может быть обработан",
         "файл поврежден или не может быть обработан"},
    }};
    for (const MediaKind kind : {MediaKind::Image, MediaKind::Video, MediaKind::Audio})
    {
        for (const FileErrorRow &row : rows)
        {
            SCOPED_TRACE(row.code);
            std::string_view tail = row.imageTail;
            if (kind == MediaKind::Video)
            {
                tail = row.videoTail;
            }
            else if (kind == MediaKind::Audio)
            {
                tail = row.audioTail;
            }
            expectReply(errorReply(kind, row.error), row.status, row.code, prefixed(kind, tail),
                        "file");
        }
    }
}

TEST(ErrorReplyTest, AudioDurationTooLongIsAudioTooLong)
{
    expectReply(errorReply(MediaKind::Audio, SearchError::DurationTooLong), 422, "AUDIO_TOO_LONG",
                prefixed(MediaKind::Audio, "аудиофрагмент длиннее 10 секунд"), "file");
}

TEST(ErrorReplyTest, VideoDurationTooLongIsVideoTooLong)
{
    expectReply(errorReply(MediaKind::Video, SearchError::DurationTooLong), 422, "VIDEO_TOO_LONG",
                prefixed(MediaKind::Video, "видео длиннее 30 секунд"), "file");
}

TEST(ErrorReplyTest, ImageDurationTooLongFallsBackToUnavailable)
{
    expectReply(errorReply(MediaKind::Image, SearchError::DurationTooLong), 503,
                "SEARCH_UNAVAILABLE", kUnavailableText, "");
}

TEST(ErrorReplyTest, TextQueryEmpty)
{
    expectReply(errorReply(MediaKind::Text, SearchError::QueryEmpty), 400, "QUERY_EMPTY",
                prefixed(MediaKind::Text, "запрос пустой"), "query");
}

TEST(ErrorReplyTest, TextQueryTooShort)
{
    expectReply(errorReply(MediaKind::Text, SearchError::QueryTooShort), 422, "QUERY_TOO_SHORT",
                prefixed(MediaKind::Text, "запрос короче 3 символов"), "query");
}

TEST(ErrorReplyTest, TextQueryTooLong)
{
    expectReply(errorReply(MediaKind::Text, SearchError::QueryTooLong), 422, "QUERY_TOO_LONG",
                prefixed(MediaKind::Text, "запрос длиннее 250 символов"), "query");
}

TEST(ErrorReplyTest, TextInvalidQueryIsUnavailableWithoutField)
{
    expectReply(errorReply(MediaKind::Text, SearchError::InvalidQuery), 503, "SEARCH_UNAVAILABLE",
                kUnavailableText, "");
}

TEST(ErrorReplyTest, FileErrorsForTextFallBackToUnavailable)
{
    for (const SearchError error : kAllErrors)
    {
        if (!isFileError(error))
        {
            continue;
        }
        SCOPED_TRACE(static_cast<int>(error));
        expectReply(errorReply(MediaKind::Text, error), 503, "SEARCH_UNAVAILABLE", kUnavailableText,
                    "");
    }
}

TEST(ErrorReplyTest, QueryErrorsForFileKindsFallBackToUnavailable)
{
    for (const MediaKind kind : {MediaKind::Image, MediaKind::Video, MediaKind::Audio})
    {
        for (const SearchError error : {SearchError::QueryEmpty, SearchError::QueryTooShort,
                                        SearchError::QueryTooLong, SearchError::InvalidQuery})
        {
            SCOPED_TRACE(static_cast<int>(kind) * 100 + static_cast<int>(error));
            expectReply(errorReply(kind, error), 503, "SEARCH_UNAVAILABLE", kUnavailableText, "");
        }
    }
}

TEST(ErrorReplyTest, UnavailableIsTheSameForAllKinds)
{
    for (const MediaKind kind : kAllKinds)
    {
        SCOPED_TRACE(static_cast<int>(kind));
        expectReply(errorReply(kind, SearchError::Unavailable), 503, "SEARCH_UNAVAILABLE",
                    kUnavailableText, "");
    }
}

TEST(ErrorReplyTest, BusyIsTheSameForAllKinds)
{
    for (const MediaKind kind : kAllKinds)
    {
        SCOPED_TRACE(static_cast<int>(kind));
        expectReply(errorReply(kind, SearchError::Busy), 503, "SERVER_BUSY",
                    "Сервис перегружен. Попробуйте позже", "");
    }
}

TEST(ErrorReplyTest, NoReplyOfAnyKindUsesQueryNotAllowedCode)
{
    for (const MediaKind kind : kAllKinds)
    {
        for (const SearchError error : kAllErrors)
        {
            SCOPED_TRACE(static_cast<int>(kind) * 100 + static_cast<int>(error));
            EXPECT_NE(errorReply(kind, error).code, "QUERY_NOT_ALLOWED");
        }
    }
}

TEST(ForKindTest, MatrixOfAllKindsAndErrorsFollowsEndpointTables)
{
    for (const MediaKind kind : kAllKinds)
    {
        for (const SearchError error : kAllErrors)
        {
            SCOPED_TRACE(static_cast<int>(kind) * 100 + static_cast<int>(error));
            EXPECT_EQ(forKind(kind, error), expectedForKind(kind, error));
        }
    }
}

TEST(ForKindTest, VideoAndAudioKeepDurationTooLong)
{
    EXPECT_EQ(forKind(MediaKind::Video, SearchError::DurationTooLong),
              SearchError::DurationTooLong);
    EXPECT_EQ(forKind(MediaKind::Audio, SearchError::DurationTooLong),
              SearchError::DurationTooLong);
}

TEST(ForKindTest, ImageDurationTooLongBecomesUnavailable)
{
    EXPECT_EQ(forKind(MediaKind::Image, SearchError::DurationTooLong), SearchError::Unavailable);
}

TEST(ForKindTest, TextKeepsQueryErrorsAndInvalidQuery)
{
    for (const SearchError error : {SearchError::QueryEmpty, SearchError::QueryTooShort,
                                    SearchError::QueryTooLong, SearchError::InvalidQuery})
    {
        EXPECT_EQ(forKind(MediaKind::Text, error), error);
    }
}

TEST(ForKindTest, UnavailableAndBusyUnchangedForAllKinds)
{
    for (const MediaKind kind : kAllKinds)
    {
        EXPECT_EQ(forKind(kind, SearchError::Unavailable), SearchError::Unavailable);
        EXPECT_EQ(forKind(kind, SearchError::Busy), SearchError::Busy);
    }
}
