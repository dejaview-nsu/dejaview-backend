#include "search/query.hpp"

#include <gtest/gtest.h>

#include <array>
#include <expected>
#include <string>
#include <string_view>

namespace
{

void expectError(std::string_view raw, SearchError error)
{
    const auto result = checkQuery(raw);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), error);
}

void expectOk(std::string_view raw, std::string_view text, std::size_t length)
{
    const auto result = checkQuery(raw);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->text, text);
    EXPECT_EQ(result->length, length);
}

std::string repeat(std::string_view unit, std::size_t count)
{
    std::string result;
    for (std::size_t i = 0; i < count; ++i)
    {
        result += unit;
    }
    return result;
}

// Все символы Unicode White_Space не из ASCII, в UTF-8.
constexpr std::array<std::string_view, 17> kUnicodeSpaces{
    "\xC2\x85",     "\xC2\xA0",     "\xE1\x9A\x80", "\xE2\x80\x80", "\xE2\x80\x81", "\xE2\x80\x82",
    "\xE2\x80\x83", "\xE2\x80\x84", "\xE2\x80\x85", "\xE2\x80\x86", "\xE2\x80\x87", "\xE2\x80\x88",
    "\xE2\x80\x89", "\xE2\x80\x8A", "\xE2\x80\xA8", "\xE2\x80\xA9", "\xE2\x80\xAF"};

constexpr std::string_view kMediumMathSpace = "\xE2\x81\x9F";
constexpr std::string_view kIdeographicSpace = "\xE3\x80\x80";
constexpr std::string_view kZeroWidthSpace = "\xE2\x80\x8B";

}  // namespace

TEST(QueryConstantsTest, BoundsMatchContract)
{
    EXPECT_EQ(kMinQueryChars, 3U);
    EXPECT_EQ(kMaxQueryChars, 250U);
}

TEST(CheckQueryTest, EmptyStringIsEmpty) { expectError("", SearchError::QueryEmpty); }

TEST(CheckQueryTest, AsciiWhitespaceOnlyIsEmpty)
{
    expectError("   ", SearchError::QueryEmpty);
    expectError("\t\n\v\f\r ", SearchError::QueryEmpty);
}

TEST(CheckQueryTest, UnicodeWhitespaceOnlyIsEmpty)
{
    expectError(std::string(" ") + std::string(kIdeographicSpace) + " \xC2\x85",
                SearchError::QueryEmpty);
    expectError(std::string(kMediumMathSpace), SearchError::QueryEmpty);
    for (const std::string_view space : kUnicodeSpaces)
    {
        SCOPED_TRACE(std::string(space));
        expectError(space, SearchError::QueryEmpty);
    }
}

TEST(CheckQueryTest, UnicodeWhitespaceAroundTextIsTrimmed)
{
    for (const std::string_view space : kUnicodeSpaces)
    {
        SCOPED_TRACE(std::string(space));
        const std::string raw = std::string(space) + "abc" + std::string(space);
        expectOk(raw, "abc", 3);
    }
}

TEST(CheckQueryTest, ShortQueriesAreTooShort)
{
    expectError("a", SearchError::QueryTooShort);
    expectError("ab", SearchError::QueryTooShort);
    expectError("  ab  ", SearchError::QueryTooShort);
    expectError("аб", SearchError::QueryTooShort);
}

TEST(CheckQueryTest, ThreeCharactersAreAccepted) { expectOk("abc", "abc", 3); }

TEST(CheckQueryTest, TrimmedTextIsViewIntoRaw)
{
    const std::string raw = "  abc\n";
    const auto result = checkQuery(raw);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->text, "abc");
    EXPECT_EQ(result->text.data(), raw.data() + 2);
}

TEST(CheckQueryTest, LengthCountsCodePointsNotBytes)
{
    expectOk("абв", "абв", 3);
    expectOk("\xF0\x9F\x98\x80\xF0\x9F\x98\x80\xF0\x9F\x98\x80",
             "\xF0\x9F\x98\x80\xF0\x9F\x98\x80\xF0\x9F\x98\x80", 3);
}

TEST(CheckQueryTest, InnerSpacesAreKept) { expectOk("a  b", "a  b", 4); }

TEST(CheckQueryTest, ZeroWidthSpaceIsNotWhitespace)
{
    const std::string raw = repeat(kZeroWidthSpace, 3);
    expectOk(raw, raw, 3);
}

TEST(CheckQueryTest, EmbeddedNulIsCountedAsCharacter)
{
    const std::string raw("ab\0c", 4);
    expectOk(raw, raw, 4);
}

TEST(CheckQueryTest, UpperBoundIsInclusive)
{
    expectOk(repeat("a", 249), repeat("a", 249), 249);
    expectOk(repeat("a", 250), repeat("a", 250), 250);
    expectOk(repeat("я", 250), repeat("я", 250), 250);
}

TEST(CheckQueryTest, AboveUpperBoundIsTooLong)
{
    expectError(repeat("a", 251), SearchError::QueryTooLong);
    expectError(repeat("я", 251), SearchError::QueryTooLong);
    expectError(repeat("a", 10000), SearchError::QueryTooLong);
}

TEST(CheckQueryTest, TrimmedWhitespaceDoesNotCountTowardLimit)
{
    const std::string body = repeat("a", 250);
    expectOk(" " + body + " ", body, 250);
}

TEST(CheckQueryTest, InvalidUtf8IsEmpty)
{
    const std::array<std::string_view, 8> bad{
        "\xFF"
        "abc",
        "abc\x80",
        "\xC0\xAF"
        "abc",
        "\xE0\x80\xAF"
        "abc",
        "\xED\xA0\x80"
        "abc",
        "\xF4\x90\x80\x80"
        "abc",
        "abc\xE2\x82",
        "\xF8\x88\x80\x80\x80"};
    for (const std::string_view raw : bad)
    {
        SCOPED_TRACE(std::string(raw));
        expectError(raw, SearchError::QueryEmpty);
    }
}

TEST(CheckQueryTest, OtherOverlongAndOutOfRangeSequencesAreEmpty)
{
    expectError(
        "\xC1\xBF"
        "abc",
        SearchError::QueryEmpty);
    expectError(
        "\xF0\x80\x80\x80"
        "abc",
        SearchError::QueryEmpty);
    expectError(
        "\xF5\x80\x80\x80"
        "abc",
        SearchError::QueryEmpty);
    expectError(
        "\xED\xBF\xBF"
        "abc",
        SearchError::QueryEmpty);
    expectError(
        "\xE2\x28\xA1"
        "abc",
        SearchError::QueryEmpty);
}

TEST(CheckQueryTest, BoundaryValidUtf8IsAccepted)
{
    expectOk("\xED\x9F\xBF\xEE\x80\x80\xF4\x8F\xBF\xBF", "\xED\x9F\xBF\xEE\x80\x80\xF4\x8F\xBF\xBF",
             3);
}

TEST(CheckQueryTest, InvalidUtf8WinsOverTooShort) { expectError("\xFF", SearchError::QueryEmpty); }

TEST(CheckQueryTest, HtmlIsNotRejected)
{
    expectOk("<script>alert(1)</script>", "<script>alert(1)</script>", 25);
}

TEST(ResolveInvalidQueryTest, ShortLengthsResolveToTooShort)
{
    EXPECT_EQ(resolveInvalidQuery(SearchError::InvalidQuery, 3), SearchError::QueryTooShort);
    EXPECT_EQ(resolveInvalidQuery(SearchError::InvalidQuery, 126), SearchError::QueryTooShort);
}

TEST(ResolveInvalidQueryTest, LongLengthsResolveToTooLong)
{
    EXPECT_EQ(resolveInvalidQuery(SearchError::InvalidQuery, 127), SearchError::QueryTooLong);
    EXPECT_EQ(resolveInvalidQuery(SearchError::InvalidQuery, 250), SearchError::QueryTooLong);
}

TEST(ResolveInvalidQueryTest, OtherErrorsAreUnchanged)
{
    EXPECT_EQ(resolveInvalidQuery(SearchError::Unavailable, 3), SearchError::Unavailable);
    EXPECT_EQ(resolveInvalidQuery(SearchError::Busy, 250), SearchError::Busy);
}
