#include "movies/card.hpp"

#include <gtest/gtest.h>

TEST(ParseMovieIdTest, AcceptsPositiveIntegers)
{
    EXPECT_EQ(parseMovieId("603"), 603);
    EXPECT_EQ(parseMovieId("1"), 1);
    EXPECT_EQ(parseMovieId("9223372036854775807"), 9223372036854775807);
}

// Контракт: movie_id не целое положительное число - 404
TEST(ParseMovieIdTest, RejectsEverythingElse)
{
    for (const char *text :
         {"", "0", "-1", "+1", "1.5", "abc", "12abc", " 12", "9223372036854775808"})
    {
        EXPECT_EQ(parseMovieId(text), std::nullopt) << text;
    }
}
