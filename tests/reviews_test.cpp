#include "reviews.hpp"

#include <gtest/gtest.h>

TEST(ParseReviewsQueryTest, DefaultsToBestFirstPage)
{
    const auto query = parseReviewsQuery("", "");
    ASSERT_TRUE(query);
    EXPECT_EQ(query->sort, ReviewsSort::Best);
    EXPECT_EQ(query->page, 1);
}

TEST(ParseReviewsQueryTest, AcceptsEverySortAndPositivePage)
{
    EXPECT_EQ(parseReviewsQuery("best", "")->sort, ReviewsSort::Best);
    EXPECT_EQ(parseReviewsQuery("worst", "")->sort, ReviewsSort::Worst);
    EXPECT_EQ(parseReviewsQuery("newest", "")->sort, ReviewsSort::Newest);
    EXPECT_EQ(parseReviewsQuery("oldest", "")->sort, ReviewsSort::Oldest);
    EXPECT_EQ(parseReviewsQuery("", "3")->page, 3);
}

// Контракт: sort не из списка или page не целое число от 1 - 400 с field
TEST(ParseReviewsQueryTest, NamesInvalidParameter)
{
    for (const char *sort : {"rating", "Best", "best ", "1"})
    {
        EXPECT_EQ(parseReviewsQuery(sort, "1").error(), "sort") << sort;
    }
    for (const char *page : {"0", "-1", "+1", "1.5", "abc", "2x", "99999999999"})
    {
        EXPECT_EQ(parseReviewsQuery("best", page).error(), "page") << page;
    }
    EXPECT_EQ(parseReviewsQuery("rating", "0").error(), "sort");  // первая ошибка - sort
}
