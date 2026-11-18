#include "lists.hpp"

#include <gtest/gtest.h>

TEST(IsListTypeTest, AcceptsOnlyTwoLists)
{
    EXPECT_TRUE(isListType("want_to_watch"));
    EXPECT_TRUE(isListType("watched"));
    for (const char *text : {"", "Watched", "watched ", "ratings", "want-to-watch"})
    {
        EXPECT_FALSE(isListType(text)) << text;
    }
}

TEST(ParseListQueryTest, DefaultsToNewestWholeListFirstPage)
{
    const auto query = parseListQuery("", "", "");
    ASSERT_TRUE(query);
    EXPECT_EQ(query->sort, ListSort::Newest);
    EXPECT_EQ(query->q, "");
    EXPECT_EQ(query->page, 1);
}

TEST(ParseListQueryTest, AcceptsEverySortAndPositivePage)
{
    EXPECT_EQ(parseListQuery("newest", "", "")->sort, ListSort::Newest);
    EXPECT_EQ(parseListQuery("oldest", "", "")->sort, ListSort::Oldest);
    EXPECT_EQ(parseListQuery("title_asc", "", "")->sort, ListSort::TitleAsc);
    EXPECT_EQ(parseListQuery("title_desc", "", "")->sort, ListSort::TitleDesc);
    EXPECT_EQ(parseListQuery("best", "", "")->sort, ListSort::Best);
    EXPECT_EQ(parseListQuery("worst", "", "")->sort, ListSort::Worst);
    EXPECT_EQ(parseListQuery("", "", "3")->page, 3);
}

// Контракт: пробелы по краям q не учитываются, пустой q - весь список («Очистить»)
TEST(ParseListQueryTest, TrimsSearchText)
{
    EXPECT_EQ(parseListQuery("", "  матр \t", "")->q, "матр");
    EXPECT_EQ(parseListQuery("", "Бойцовский клуб", "")->q, "Бойцовский клуб");
    EXPECT_EQ(parseListQuery("", " \t\n ", "")->q, "");
}

// Контракт: sort не из списка или page не целое число от 1 - 400 с field
TEST(ParseListQueryTest, NamesInvalidParameter)
{
    for (const char *sort : {"rating", "Newest", "title", "newest ", "1"})
    {
        EXPECT_EQ(parseListQuery(sort, "", "1").error(), "sort") << sort;
    }
    for (const char *page : {"0", "-1", "+1", "1.5", "abc", "2x", "99999999999"})
    {
        EXPECT_EQ(parseListQuery("newest", "", page).error(), "page") << page;
    }
    EXPECT_EQ(parseListQuery("rating", "", "0").error(), "sort");  // первая ошибка - sort
}
