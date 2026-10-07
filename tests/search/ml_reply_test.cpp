#include "search/ml_reply.hpp"

#include <gtest/gtest.h>

#include <json/reader.h>
#include <json/value.h>

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace
{

Json::Value parseJson(const std::string &text)
{
    Json::Value value;
    Json::Reader reader;
    EXPECT_TRUE(reader.parse(text, value)) << text;
    return value;
}

std::expected<std::vector<std::int64_t>, MlFailure> parse(MediaKind kind, int status,
                                                          const std::string &json)
{
    const Json::Value body = parseJson(json);
    return parseMlReply(kind, status, &body);
}

void expectFailure(const std::expected<std::vector<std::int64_t>, MlFailure> &result,
                   SearchError error, bool outage)
{
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().error, error);
    EXPECT_EQ(result.error().outage, outage);
}

}  // namespace

TEST(ParseMlReplyTest, ReturnsSingleId)
{
    const auto result = parse(MediaKind::Image, 200, R"({"movie_ids":[603]})");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, std::vector<std::int64_t>{603});
}

TEST(ParseMlReplyTest, ReturnsEmptyListForEmptyResult)
{
    const auto result = parse(MediaKind::Image, 200, R"({"movie_ids":[]})");
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->empty());
}

TEST(ParseMlReplyTest, KeepsMlOrder)
{
    const auto result = parse(MediaKind::Video, 200, R"({"movie_ids":[605,603,604]})");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, (std::vector<std::int64_t>{605, 603, 604}));
}

TEST(ParseMlReplyTest, KeepsIdsAbove2Pow53Exactly)
{
    const auto result = parse(MediaKind::Image, 200, R"({"movie_ids":[9007199254740993]})");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, std::vector<std::int64_t>{9007199254740993});
}

TEST(ParseMlReplyTest, TreatsNonJsonOkReplyAsUnavailableWithoutOutage)
{
    expectFailure(parseMlReply(MediaKind::Image, 200, nullptr), SearchError::Unavailable, false);
}

TEST(ParseMlReplyTest, TreatsMalformedOkBodyAsUnavailableWithoutOutage)
{
    for (const char *json :
         {"{}", R"({"movie_ids":603})", R"({"movie_ids":["603"]})", R"({"movie_ids":[1.5]})",
          R"({"movie_ids":[0]})", R"({"movie_ids":[-1]})", R"({"movie_ids":[603,0]})", "[603]"})
    {
        SCOPED_TRACE(json);
        expectFailure(parse(MediaKind::Image, 200, json), SearchError::Unavailable, false);
    }
}

TEST(ParseMlReplyTest, MapsFileTooLarge)
{
    expectFailure(parse(MediaKind::Image, 413, R"({"code":"FILE_TOO_LARGE"})"),
                  SearchError::FileTooLarge, false);
}

TEST(ParseMlReplyTest, MapsUnsupportedMediaType)
{
    expectFailure(parse(MediaKind::Video, 415, R"({"code":"UNSUPPORTED_MEDIA_TYPE"})"),
                  SearchError::UnsupportedFormat, false);
}

TEST(ParseMlReplyTest, MapsFileCorruptedForBothKinds)
{
    for (const MediaKind kind : {MediaKind::Image, MediaKind::Video})
    {
        expectFailure(
            parse(kind, 422, R"({"code":"FILE_CORRUPTED","message":"cannot decode image"})"),
            SearchError::FileCorrupted, false);
    }
}

TEST(ParseMlReplyTest, MapsDurationTooLongForVideo)
{
    expectFailure(parse(MediaKind::Video, 422, R"({"code":"DURATION_TOO_LONG"})"),
                  SearchError::VideoTooLong, false);
}

TEST(ParseMlReplyTest, MapsDurationTooLongForImageToUnavailable)
{
    expectFailure(parse(MediaKind::Image, 422, R"({"code":"DURATION_TOO_LONG"})"),
                  SearchError::Unavailable, false);
}

TEST(ParseMlReplyTest, MapsUnknownClientErrorsToUnavailableWithoutOutage)
{
    expectFailure(parse(MediaKind::Image, 422, R"({"code":"INVALID_QUERY"})"),
                  SearchError::Unavailable, false);
    expectFailure(parse(MediaKind::Image, 400, "{}"), SearchError::Unavailable, false);
    expectFailure(parse(MediaKind::Image, 400, R"({"code":7})"), SearchError::Unavailable, false);
    expectFailure(parseMlReply(MediaKind::Image, 404, nullptr), SearchError::Unavailable, false);
}

TEST(ParseMlReplyTest, TreatsServerErrorsAsOutage)
{
    expectFailure(parse(MediaKind::Image, 500, R"({"code":"INTERNAL_ERROR"})"),
                  SearchError::Unavailable, true);
    expectFailure(parse(MediaKind::Image, 503, R"({"code":"NOT_READY"})"), SearchError::Unavailable,
                  true);
    expectFailure(parse(MediaKind::Video, 503, R"({"code":"QDRANT_UNAVAILABLE"})"),
                  SearchError::Unavailable, true);
    expectFailure(parseMlReply(MediaKind::Image, 502, nullptr), SearchError::Unavailable, true);
}

TEST(ParseMlReplyTest, ServerErrorIgnoresClientErrorCodeInBody)
{
    expectFailure(parse(MediaKind::Image, 503, R"({"code":"FILE_TOO_LARGE"})"),
                  SearchError::Unavailable, true);
}

TEST(ParseMlReplyTest, OutageBoundaries)
{
    expectFailure(parseMlReply(MediaKind::Image, 399, nullptr), SearchError::Unavailable, false);
    expectFailure(parseMlReply(MediaKind::Image, 400, nullptr), SearchError::Unavailable, false);
    expectFailure(parse(MediaKind::Image, 499, R"({"code":"FILE_TOO_LARGE"})"),
                  SearchError::FileTooLarge, false);
    expectFailure(parseMlReply(MediaKind::Image, 500, nullptr), SearchError::Unavailable, true);
    expectFailure(parseMlReply(MediaKind::Image, 599, nullptr), SearchError::Unavailable, true);
    expectFailure(parseMlReply(MediaKind::Image, 600, nullptr), SearchError::Unavailable, false);
}

TEST(ParseMlReplyTest, TreatsOtherStatusesAsUnavailableWithoutOutage)
{
    for (const int status : {100, 201, 204, 301, 399})
    {
        SCOPED_TRACE(status);
        expectFailure(parse(MediaKind::Image, status, R"({"movie_ids":[603]})"),
                      SearchError::Unavailable, false);
    }
}
