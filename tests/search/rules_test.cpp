#include "search/rules.hpp"

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <string_view>

using namespace std::string_literals;

namespace
{

const std::string kJpeg = "\xFF\xD8\xFF\xE0\x00\x10JFIF"s;
const std::string kPng = "\x89PNG\r\n\x1a\n"s + "\0\0\0\rIHDR"s;
const std::string kWebp = "RIFF\0\0\0\0WEBP"s;
const std::string kGif = "GIF89a\x01\x00\x01\x00"s;

std::string isoBmff(std::string_view brand)
{
    return "\0\0\0\x18"s + "ftyp" + std::string(brand) + "\0\0\0\0"s + "isom";
}

char byte(std::size_t value) { return static_cast<char>(value); }

// EBML-заголовок: ID, размер вектором в sizeBytes байт (1 или 8), затем дети
std::string ebml(const std::string &children, std::size_t sizeBytes = 1)
{
    std::string size;
    if (sizeBytes == 1)
    {
        size = std::string(1, byte(0x80 | children.size()));
    }
    else
    {
        size = std::string(1, byte(0x01)) + std::string(sizeBytes - 2, '\0') +
               std::string(1, byte(children.size()));
    }
    return "\x1A\x45\xDF\xA3"s + size + children;
}

std::string docType(std::string_view value)
{
    return "\x42\x82"s + std::string(1, byte(0x80 | value.size())) + std::string(value);
}

const std::string kEbmlVersion = "\x42\x86\x81\x01"s;

std::string withBytes(std::string head, std::size_t total)
{
    head.resize(total, 'x');
    return head;
}

std::optional<std::string_view> image(std::string_view content)
{
    return detectMediaType(MediaKind::Image, content);
}

std::optional<std::string_view> video(std::string_view content)
{
    return detectMediaType(MediaKind::Video, content);
}

}  // namespace

TEST(DetectMediaTypeTest, RecognizesJpeg) { EXPECT_EQ(image(kJpeg), "image/jpeg"); }

TEST(DetectMediaTypeTest, RejectsTruncatedJpegSignature)
{
    EXPECT_EQ(image("\xFF\xD8"s), std::nullopt);
}

TEST(DetectMediaTypeTest, RecognizesPng) { EXPECT_EQ(image(kPng), "image/png"); }

TEST(DetectMediaTypeTest, RejectsPngWithAnyChangedSignatureByte)
{
    for (std::size_t i = 0; i < 8; ++i)
    {
        SCOPED_TRACE(i);
        std::string broken = kPng;
        broken[i] = static_cast<char>(broken[i] ^ 0x01);
        EXPECT_EQ(image(broken), std::nullopt);
    }
}

TEST(DetectMediaTypeTest, RecognizesWebp) { EXPECT_EQ(image(kWebp), "image/webp"); }

TEST(DetectMediaTypeTest, RejectsRiffThatIsNotWebp)
{
    EXPECT_EQ(image("RIFF\0\0\0\0WAVE"s), std::nullopt);
}

TEST(DetectMediaTypeTest, RejectsTruncatedRiff)
{
    EXPECT_EQ(image("RIFF"), std::nullopt);
    EXPECT_EQ(image("RIFF\0\0\0\0WEB"s), std::nullopt);
}

TEST(DetectMediaTypeTest, RejectsGifAndEmptyContent)
{
    EXPECT_EQ(image(kGif), std::nullopt);
    EXPECT_EQ(image(""), std::nullopt);
}

TEST(DetectMediaTypeTest, VideoKindRejectsImageBytes)
{
    EXPECT_EQ(video(kJpeg), std::nullopt);
    EXPECT_EQ(video(kPng), std::nullopt);
    EXPECT_EQ(video(kWebp), std::nullopt);
}

TEST(DetectMediaTypeTest, ImageKindRejectsVideoBytes)
{
    EXPECT_EQ(image(isoBmff("isom")), std::nullopt);
    EXPECT_EQ(image(ebml(kEbmlVersion + docType("webm"))), std::nullopt);
}

TEST(DetectMediaTypeTest, RecognizesMp4)
{
    EXPECT_EQ(video(isoBmff("isom")), "video/mp4");
    EXPECT_EQ(video(isoBmff("mp42")), "video/mp4");
}

TEST(DetectMediaTypeTest, RecognizesQuickTimeBrand)
{
    EXPECT_EQ(video(isoBmff("qt  ")), "video/quicktime");
}

TEST(DetectMediaTypeTest, RejectsImageAndAudioIsoBmffBrands)
{
    for (const std::string_view brand : {"avif", "avis", "heic", "heix", "heim", "heis", "hevc",
                                         "hevx", "mif1", "msf1", "M4A ", "M4B ", "M4P "})
    {
        SCOPED_TRACE(brand);
        EXPECT_EQ(video(isoBmff(brand)), std::nullopt);
    }
}

TEST(DetectMediaTypeTest, AcceptsFtypBoxOfExactlyTwelveBytes)
{
    EXPECT_EQ(video("\0\0\0\x0c"s + "ftypisom"), "video/mp4");
}

TEST(DetectMediaTypeTest, RejectsFtypBoxShorterThanTwelveBytes)
{
    EXPECT_EQ(video("\0\0\0\x0b"s + "ftypiso"), std::nullopt);
    EXPECT_EQ(video("\0\0\0\x08"s + "ftyp"), std::nullopt);
}

TEST(DetectMediaTypeTest, RejectsLegacyMovWithoutFtyp)
{
    EXPECT_EQ(video("\0\0\0\x18"s + "moov" + "mvhd" + "\0\0\0\0"s), std::nullopt);
}

TEST(DetectMediaTypeTest, RecognizesWebmDocType)
{
    EXPECT_EQ(video(ebml(docType("webm"))), "video/webm");
}

TEST(DetectMediaTypeTest, RecognizesWebmDocTypeAfterEbmlVersion)
{
    EXPECT_EQ(video(ebml(kEbmlVersion + docType("webm"))), "video/webm");
}

TEST(DetectMediaTypeTest, RecognizesWebmWithEightByteHeaderSize)
{
    EXPECT_EQ(video(ebml(kEbmlVersion + docType("webm"), 8)), "video/webm");
}

TEST(DetectMediaTypeTest, TrimsTrailingNulsFromDocType)
{
    EXPECT_EQ(video(ebml("\x42\x82\x86webm\0\0"s)), "video/webm");
}

TEST(DetectMediaTypeTest, RecognizesWebmWhenHeaderSizeIsUnknown)
{
    EXPECT_EQ(video("\x1A\x45\xDF\xA3\xFF"s + kEbmlVersion + docType("webm")), "video/webm");
}

TEST(DetectMediaTypeTest, RejectsMatroskaDocType)
{
    EXPECT_EQ(video(ebml(docType("matroska"))), std::nullopt);
}

TEST(DetectMediaTypeTest, RejectsDocTypeThatOnlyStartsWithWebm)
{
    EXPECT_EQ(video(ebml(docType("webmx"))), std::nullopt);
}

TEST(DetectMediaTypeTest, RejectsEbmlWithoutDocType)
{
    EXPECT_EQ(video(ebml(kEbmlVersion)), std::nullopt);
}

TEST(DetectMediaTypeTest, RejectsHeaderSizeWithoutMarkerBit)
{
    EXPECT_EQ(video("\x1A\x45\xDF\xA3\x00"s + docType("webm")), std::nullopt);
}

TEST(DetectMediaTypeTest, RejectsHeaderTruncatedInsideDocType)
{
    const std::string full = ebml(kEbmlVersion + docType("webm"));
    EXPECT_EQ(video(full.substr(0, full.size() - 2)), std::nullopt);
}

TEST(DetectMediaTypeTest, RejectsEbmlMagicWithoutSize)
{
    EXPECT_EQ(video("\x1A\x45\xDF\xA3"s), std::nullopt);
}

TEST(DetectMediaTypeTest, RejectsOversizedDocTypeSize)
{
    EXPECT_EQ(video(ebml("\x42\x82\x01\xFF\xFF\xFF\xFF\xFF\xFF\xFE"s + "webm")), std::nullopt);
}

TEST(DetectMediaTypeTest, RejectsOversizedEbmlHeaderSize)
{
    EXPECT_EQ(video("\x1A\x45\xDF\xA3\x01\xFF\xFF\xFF\xFF\xFF\xFF\xFE"s + docType("webm")),
              std::nullopt);
}

TEST(CheckUploadTest, RequiresAFile)
{
    const auto result = checkUpload(MediaKind::Image, {});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), SearchError::FileRequired);
}

TEST(CheckUploadTest, TreatsOnlyEmptyPartsAsNoFile)
{
    const std::array<std::string_view, 1> files{""};
    const auto result = checkUpload(MediaKind::Image, files);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), SearchError::FileRequired);
}

TEST(CheckUploadTest, RejectsTwoFiles)
{
    const std::array<std::string_view, 2> files{kPng, kPng};
    const auto result = checkUpload(MediaKind::Image, files);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), SearchError::TooManyFiles);
}

TEST(CheckUploadTest, IgnoresEmptyPartNextToAFile)
{
    const std::array<std::string_view, 2> files{kPng, ""};
    const auto result = checkUpload(MediaKind::Image, files);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->mime, "image/png");
    EXPECT_EQ(result->content, kPng);
}

TEST(CheckUploadTest, ReturnsViewOfTheSameBytes)
{
    const std::array<std::string_view, 1> files{kPng};
    const auto result = checkUpload(MediaKind::Image, files);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->content.data(), kPng.data());
    EXPECT_EQ(result->content.size(), kPng.size());
}

TEST(CheckUploadTest, AcceptsImageOfExactlyTenMiB)
{
    const std::string big = withBytes(kPng, kMaxImageBytes);
    const std::array<std::string_view, 1> files{big};
    const auto result = checkUpload(MediaKind::Image, files);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->mime, "image/png");
    EXPECT_EQ(result->content.data(), big.data());
}

TEST(CheckUploadTest, RejectsImageOneByteOverTenMiB)
{
    const std::string big = withBytes(kPng, kMaxImageBytes + 1);
    const std::array<std::string_view, 1> files{big};
    const auto result = checkUpload(MediaKind::Image, files);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), SearchError::FileTooLarge);
}

TEST(CheckUploadTest, AcceptsImageOneByteUnderTenMiB)
{
    const std::string big = withBytes(kPng, kMaxImageBytes - 1);
    const std::array<std::string_view, 1> files{big};
    EXPECT_TRUE(checkUpload(MediaKind::Image, files).has_value());
}

TEST(CheckUploadTest, AcceptsVideoOfExactlyFiftyMiB)
{
    const std::string big = withBytes(isoBmff("isom"), kMaxVideoBytes);
    const std::array<std::string_view, 1> files{big};
    const auto result = checkUpload(MediaKind::Video, files);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->mime, "video/mp4");
}

TEST(CheckUploadTest, RejectsVideoOneByteOverFiftyMiB)
{
    const std::string big = withBytes(isoBmff("isom"), kMaxVideoBytes + 1);
    const std::array<std::string_view, 1> files{big};
    const auto result = checkUpload(MediaKind::Video, files);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), SearchError::FileTooLarge);
}

TEST(CheckUploadTest, VideoKindAcceptsMoreThanImageLimit)
{
    const std::string big = withBytes(isoBmff("isom"), kMaxImageBytes + 1);
    const std::array<std::string_view, 1> files{big};
    EXPECT_TRUE(checkUpload(MediaKind::Video, files).has_value());
}

TEST(CheckUploadTest, ChecksCountBeforeSize)
{
    const std::string big = withBytes(kGif, kMaxImageBytes + 1);
    const std::array<std::string_view, 2> files{big, big};
    EXPECT_EQ(checkUpload(MediaKind::Image, files).error(), SearchError::TooManyFiles);
}

TEST(CheckUploadTest, ChecksSizeBeforeFormat)
{
    const std::string big = withBytes(kGif, kMaxImageBytes + 1);
    const std::array<std::string_view, 1> files{big};
    EXPECT_EQ(checkUpload(MediaKind::Image, files).error(), SearchError::FileTooLarge);
}

TEST(CheckUploadTest, RejectsUnsupportedFormat)
{
    const std::array<std::string_view, 1> files{kGif};
    EXPECT_EQ(checkUpload(MediaKind::Image, files).error(), SearchError::UnsupportedFormat);
}

TEST(CheckUploadTest, RejectsImageWhenVideoExpected)
{
    const std::array<std::string_view, 1> files{kPng};
    EXPECT_EQ(checkUpload(MediaKind::Video, files).error(), SearchError::UnsupportedFormat);
}

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

TEST(ErrorReplyTest, UnavailableAndBusyAreTheSameForBothKinds)
{
    for (const MediaKind kind : {MediaKind::Image, MediaKind::Video})
    {
        const ErrorReply unavailable = errorReply(kind, SearchError::Unavailable);
        EXPECT_EQ(unavailable.status, 503);
        EXPECT_EQ(unavailable.code, "SEARCH_UNAVAILABLE");
        EXPECT_EQ(unavailable.message, "Поиск временно недоступен. Попробуйте позже");
        EXPECT_FALSE(unavailable.fileField);

        const ErrorReply busy = errorReply(kind, SearchError::Busy);
        EXPECT_EQ(busy.status, 503);
        EXPECT_EQ(busy.code, "SERVER_BUSY");
        EXPECT_EQ(busy.message, "Сервис перегружен. Попробуйте позже");
        EXPECT_FALSE(busy.fileField);
    }
}

TEST(SizeLimitsTest, ConstantsMatchContract)
{
    EXPECT_EQ(kMiB, 1048576u);
    EXPECT_EQ(kMaxImageBytes, 10u * 1048576u);
    EXPECT_EQ(kMaxVideoBytes, 50u * 1048576u);
}
