#include "search/errors.hpp"

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <string_view>

namespace
{

constexpr std::string_view kImagePrefix = "Невозможно выполнить поиск по изображению: ";
constexpr std::string_view kVideoPrefix = "Невозможно выполнить поиск по видеофрагменту: ";

std::string prefixed(MediaKind kind, std::string_view tail)
{
    return std::string(kind == MediaKind::Image ? kImagePrefix : kVideoPrefix) + std::string(tail);
}

struct FileErrorRow
{
    SearchError error;
    int status;
    std::string_view code;
    std::string_view imageTail;
    std::string_view videoTail;
};

}  // namespace

TEST(ErrorReplyTest, FileErrorsMatchContractTableForBothKinds)
{
    const std::array<FileErrorRow, 5> rows{{
        {SearchError::FileRequired, 400, "FILE_REQUIRED", "изображение не загружено",
         "видеофрагмент не загружен"},
        {SearchError::TooManyFiles, 400, "TOO_MANY_FILES", "загружено больше одного файла",
         "загружено больше одного файла"},
        {SearchError::FileTooLarge, 413, "FILE_TOO_LARGE", "размер файла больше 10 МБ",
         "размер файла больше 50 МБ"},
        {SearchError::UnsupportedFormat, 415, "UNSUPPORTED_FORMAT",
         "поддерживаются только JPG, JPEG, PNG и WEBP", "поддерживаются только MP4, MOV и WEBM"},
        {SearchError::FileCorrupted, 422, "FILE_CORRUPTED",
         "файл поврежден или не может быть обработан",
         "файл поврежден или не может быть обработан"},
    }};
    for (const MediaKind kind : {MediaKind::Image, MediaKind::Video})
    {
        for (const FileErrorRow &row : rows)
        {
            SCOPED_TRACE(row.code);
            const ErrorReply reply = errorReply(kind, row.error);
            EXPECT_EQ(reply.status, row.status);
            EXPECT_EQ(reply.code, row.code);
            EXPECT_EQ(reply.message,
                      prefixed(kind, kind == MediaKind::Image ? row.imageTail : row.videoTail));
            EXPECT_TRUE(reply.fileField);
        }
    }
}

TEST(ErrorReplyTest, VideoTooLongIsReportedForVideo)
{
    const ErrorReply reply = errorReply(MediaKind::Video, SearchError::VideoTooLong);
    EXPECT_EQ(reply.status, 422);
    EXPECT_EQ(reply.code, "VIDEO_TOO_LONG");
    EXPECT_EQ(reply.message, prefixed(MediaKind::Video, "видео длиннее 30 секунд"));
    EXPECT_TRUE(reply.fileField);
}

TEST(ErrorReplyTest, VideoTooLongForImageFallsBackToUnavailable)
{
    const ErrorReply reply = errorReply(MediaKind::Image, SearchError::VideoTooLong);
    EXPECT_EQ(reply.status, 503);
    EXPECT_EQ(reply.code, "SEARCH_UNAVAILABLE");
    EXPECT_EQ(reply.message, "Поиск временно недоступен. Попробуйте позже");
    EXPECT_FALSE(reply.fileField);
}

TEST(ErrorReplyTest, UnavailableIsTheSameForBothKinds)
{
    for (const MediaKind kind : {MediaKind::Image, MediaKind::Video})
    {
        const ErrorReply reply = errorReply(kind, SearchError::Unavailable);
        EXPECT_EQ(reply.status, 503);
        EXPECT_EQ(reply.code, "SEARCH_UNAVAILABLE");
        EXPECT_EQ(reply.message, "Поиск временно недоступен. Попробуйте позже");
        EXPECT_FALSE(reply.fileField);
    }
}

TEST(ErrorReplyTest, BusyIsTheSameForBothKinds)
{
    for (const MediaKind kind : {MediaKind::Image, MediaKind::Video})
    {
        const ErrorReply reply = errorReply(kind, SearchError::Busy);
        EXPECT_EQ(reply.status, 503);
        EXPECT_EQ(reply.code, "SERVER_BUSY");
        EXPECT_EQ(reply.message, "Сервис перегружен. Попробуйте позже");
        EXPECT_FALSE(reply.fileField);
    }
}

TEST(ForKindTest, ImageVideoTooLongBecomesUnavailable)
{
    EXPECT_EQ(forKind(MediaKind::Image, SearchError::VideoTooLong), SearchError::Unavailable);
}

TEST(ForKindTest, VideoKeepsVideoTooLong)
{
    EXPECT_EQ(forKind(MediaKind::Video, SearchError::VideoTooLong), SearchError::VideoTooLong);
}

TEST(ForKindTest, EveryOtherErrorIsUnchangedForBothKinds)
{
    for (const SearchError error :
         {SearchError::FileRequired, SearchError::TooManyFiles, SearchError::FileTooLarge,
          SearchError::UnsupportedFormat, SearchError::FileCorrupted, SearchError::Unavailable,
          SearchError::Busy})
    {
        for (const MediaKind kind : {MediaKind::Image, MediaKind::Video})
        {
            SCOPED_TRACE(static_cast<int>(error));
            EXPECT_EQ(forKind(kind, error), error);
        }
    }
}
