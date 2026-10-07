#include "search/rules.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <fstream>
#include <iterator>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace std::string_literals;

namespace
{

// Отсутствующий образец - сбой теста, а не пропуск
std::string readSample(const std::string &name)
{
    const std::string path = std::string(DEJAVIEW_TEST_DATA_DIR) + "/search/" + name;
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        throw std::runtime_error("sample not found: " + path);
    }
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string padded(std::string head, std::size_t total)
{
    head.resize(total, 'x');
    return head;
}

// Фиксированное зерно: тест детерминирован
std::string randomBytes(std::size_t size)
{
    std::mt19937 engine(12345);
    std::string bytes(size, 'x');
    for (char &byte : bytes)
    {
        byte = static_cast<char>(engine() & 0xFF);
    }
    return bytes;
}

std::optional<std::string_view> detect(MediaKind kind, const std::string &sample)
{
    return detectMediaType(kind, sample);
}

struct SampleRow
{
    const char *file;
    MediaKind kind;
    std::string_view mime;
};

constexpr std::array<SampleRow, 6> kAccepted{{
    {"image.jpg", MediaKind::Image, "image/jpeg"},
    {"image.png", MediaKind::Image, "image/png"},
    {"image.webp", MediaKind::Image, "image/webp"},
    {"video.mp4", MediaKind::Video, "video/mp4"},
    {"video.mov", MediaKind::Video, "video/quicktime"},
    {"video.webm", MediaKind::Video, "video/webm"},
}};

}  // namespace

TEST(DetectMediaTypeTest, AcceptsEverySupportedSampleWithExactMime)
{
    for (const SampleRow &row : kAccepted)
    {
        SCOPED_TRACE(row.file);
        EXPECT_EQ(detect(row.kind, readSample(row.file)), row.mime);
    }
}

TEST(DetectMediaTypeTest, RejectsUnlistedImageFormats)
{
    EXPECT_EQ(detect(MediaKind::Image, readSample("reject.gif")), std::nullopt);
}

TEST(DetectMediaTypeTest, RejectsUnlistedVideoContainers)
{
    for (const char *file : {"reject.mkv", "reject.3gp"})
    {
        SCOPED_TRACE(file);
        EXPECT_EQ(detect(MediaKind::Video, readSample(file)), std::nullopt);
    }
}

TEST(DetectMediaTypeTest, RejectsVideoSamplesUnderImageKind)
{
    for (const char *file : {"video.mp4", "video.mov", "video.webm"})
    {
        SCOPED_TRACE(file);
        EXPECT_EQ(detect(MediaKind::Image, readSample(file)), std::nullopt);
    }
}

TEST(DetectMediaTypeTest, RejectsImageSamplesUnderVideoKind)
{
    for (const char *file : {"image.jpg", "image.png", "image.webp"})
    {
        SCOPED_TRACE(file);
        EXPECT_EQ(detect(MediaKind::Video, readSample(file)), std::nullopt);
    }
}

TEST(DetectMediaTypeTest, RejectsPlainTextUnderBothKinds)
{
    const std::string text = "hello, this is plain text"s;
    EXPECT_EQ(detect(MediaKind::Image, text), std::nullopt);
    EXPECT_EQ(detect(MediaKind::Video, text), std::nullopt);
}

TEST(DetectMediaTypeTest, RejectsRandomBytesUnderBothKinds)
{
    const std::string bytes = randomBytes(4096);
    EXPECT_EQ(detect(MediaKind::Image, bytes), std::nullopt);
    EXPECT_EQ(detect(MediaKind::Video, bytes), std::nullopt);
}

TEST(DetectMediaTypeTest, RejectsEmptyContentUnderBothKinds)
{
    const std::string empty = std::string();
    EXPECT_EQ(detect(MediaKind::Image, empty), std::nullopt);
    EXPECT_EQ(detect(MediaKind::Video, empty), std::nullopt);
}

TEST(DetectMediaTypeTest, ReturnedMimeOutlivesTheContent)
{
    std::optional<std::string_view> mime;
    {
        const std::string png = readSample("image.png");
        mime = detect(MediaKind::Image, png);
    }
    EXPECT_EQ(mime, "image/png");
}

TEST(DetectMediaTypeTest, WorksConcurrentlyOnDifferentSamples)
{
    std::vector<std::string> contents;
    for (const SampleRow &row : kAccepted)
    {
        contents.push_back(readSample(row.file));
    }
    constexpr int kRounds = 50;
    std::atomic<int> wrong{0};
    {
        std::vector<std::jthread> threads;
        for (std::size_t i = 0; i < kAccepted.size(); ++i)
        {
            threads.emplace_back(
                [&, i]
                {
                    for (int round = 0; round < kRounds; ++round)
                    {
                        if (detectMediaType(kAccepted[i].kind, contents[i]) != kAccepted[i].mime)
                        {
                            ++wrong;
                        }
                    }
                });
        }
    }
    EXPECT_EQ(wrong.load(), 0);
}

TEST(CheckUploadTest, RequiresAFile)
{
    const auto result = checkUpload(MediaKind::Image, {});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), SearchError::FileRequired);
}

TEST(CheckUploadTest, TreatsOnlyEmptyPartsAsNoFile)
{
    const std::array<std::string_view, 2> files{"", ""};
    const auto result = checkUpload(MediaKind::Image, files);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), SearchError::FileRequired);
}

TEST(CheckUploadTest, RejectsTwoFiles)
{
    const std::string png = readSample("image.png");
    const std::array<std::string_view, 2> files{png, png};
    const auto result = checkUpload(MediaKind::Image, files);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), SearchError::TooManyFiles);
}

TEST(CheckUploadTest, IgnoresEmptyPartsAroundAFile)
{
    const std::string png = readSample("image.png");
    const std::array<std::string_view, 3> files{"", png, ""};
    const auto result = checkUpload(MediaKind::Image, files);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->mime, "image/png");
    EXPECT_EQ(result->content, png);
}

TEST(CheckUploadTest, ReturnsViewOfTheSameBytes)
{
    const std::string png = readSample("image.png");
    const std::array<std::string_view, 1> files{png};
    const auto result = checkUpload(MediaKind::Image, files);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->content.data(), png.data());
    EXPECT_EQ(result->content.size(), png.size());
}

TEST(CheckUploadTest, AcceptsVideoSample)
{
    const std::string mp4 = readSample("video.mp4");
    const std::array<std::string_view, 1> files{mp4};
    const auto result = checkUpload(MediaKind::Video, files);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->mime, "video/mp4");
}

TEST(CheckUploadTest, AcceptsImageOfExactlyTenMiB)
{
    const std::string big = padded(readSample("image.png"), kMaxImageBytes);
    const std::array<std::string_view, 1> files{big};
    const auto result = checkUpload(MediaKind::Image, files);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->mime, "image/png");
    EXPECT_EQ(result->content.data(), big.data());
}

TEST(CheckUploadTest, AcceptsImageOneByteUnderTenMiB)
{
    const std::string big = padded(readSample("image.png"), kMaxImageBytes - 1);
    const std::array<std::string_view, 1> files{big};
    EXPECT_TRUE(checkUpload(MediaKind::Image, files).has_value());
}

TEST(CheckUploadTest, RejectsImageOneByteOverTenMiB)
{
    const std::string big = padded(readSample("image.png"), kMaxImageBytes + 1);
    const std::array<std::string_view, 1> files{big};
    const auto result = checkUpload(MediaKind::Image, files);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), SearchError::FileTooLarge);
}

TEST(CheckUploadTest, AcceptsVideoOfExactlyFiftyMiB)
{
    const std::string big = padded(readSample("video.mp4"), kMaxVideoBytes);
    const std::array<std::string_view, 1> files{big};
    const auto result = checkUpload(MediaKind::Video, files);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->mime, "video/mp4");
}

TEST(CheckUploadTest, AcceptsVideoOneByteUnderFiftyMiB)
{
    const std::string big = padded(readSample("video.mp4"), kMaxVideoBytes - 1);
    const std::array<std::string_view, 1> files{big};
    EXPECT_TRUE(checkUpload(MediaKind::Video, files).has_value());
}

TEST(CheckUploadTest, RejectsVideoOneByteOverFiftyMiB)
{
    const std::string big = padded(readSample("video.mp4"), kMaxVideoBytes + 1);
    const std::array<std::string_view, 1> files{big};
    const auto result = checkUpload(MediaKind::Video, files);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), SearchError::FileTooLarge);
}

TEST(CheckUploadTest, VideoKindAcceptsMoreThanImageLimit)
{
    const std::string big = padded(readSample("video.mp4"), kMaxImageBytes + 1);
    const std::array<std::string_view, 1> files{big};
    EXPECT_TRUE(checkUpload(MediaKind::Video, files).has_value());
}

TEST(CheckUploadTest, ChecksCountBeforeSize)
{
    const std::string big = padded(readSample("reject.gif"), kMaxImageBytes + 1);
    const std::array<std::string_view, 2> files{big, big};
    EXPECT_EQ(checkUpload(MediaKind::Image, files).error(), SearchError::TooManyFiles);
}

TEST(CheckUploadTest, ChecksSizeBeforeFormat)
{
    const std::string big = padded(readSample("reject.gif"), kMaxImageBytes + 1);
    const std::array<std::string_view, 1> files{big};
    EXPECT_EQ(checkUpload(MediaKind::Image, files).error(), SearchError::FileTooLarge);
}

TEST(CheckUploadTest, RejectsUnsupportedFormat)
{
    const std::string gif = readSample("reject.gif");
    const std::array<std::string_view, 1> files{gif};
    EXPECT_EQ(checkUpload(MediaKind::Image, files).error(), SearchError::UnsupportedFormat);
}

TEST(CheckUploadTest, RejectsImageWhenVideoExpected)
{
    const std::string png = readSample("image.png");
    const std::array<std::string_view, 1> files{png};
    EXPECT_EQ(checkUpload(MediaKind::Video, files).error(), SearchError::UnsupportedFormat);
}

TEST(SizeLimitsTest, ConstantsMatchContract)
{
    EXPECT_EQ(kMiB, 1048576u);
    EXPECT_EQ(kMaxImageBytes, 10u * 1048576u);
    EXPECT_EQ(kMaxVideoBytes, 50u * 1048576u);
}
