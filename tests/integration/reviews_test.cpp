#include "reviews.hpp"
#include "integration/db_test.hpp"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace drogon;

namespace
{
constexpr std::int64_t kDeletedAuthor = 0;  // user_id NULL: профиль удалён (#17030 п. 3)
}  // namespace

class ReviewsTest : public DbTest
{
  protected:
    void SetUp() override
    {
        DbTest::SetUp();
        if (IsSkipped())
        {
            return;  // нет TEST_DATABASE_URL - db пустой
        }
        db->execSqlSync(
            "INSERT INTO movies (movie_id, title, original_title) "
            "VALUES (157336, 'Интерстеллар', 'Interstellar')");
    }

    // Отзыв на 157336 minutesAgo минут назад. Текст - 300 символов, меньше не пускает CHECK
    std::int64_t addReview(std::int64_t userId, int minutesAgo)
    {
        return db
            ->execSqlSync(
                "INSERT INTO reviews (user_id, movie_id, verdict, body, created_at) "
                "VALUES (NULLIF(" +
                std::to_string(userId) + ", 0), 157336, 'positive', repeat('а', 300), now() - " +
                std::to_string(minutesAgo) +
                " * interval '1 minute') RETURNING review_id")[0]["review_id"]
            .as<std::int64_t>();
    }

    void react(std::int64_t reviewId, std::int64_t userId, int reaction)
    {
        db->execSqlSync("INSERT INTO review_reactions (review_id, user_id, reaction) VALUES (" +
                        std::to_string(reviewId) + ", " + std::to_string(userId) + ", " +
                        std::to_string(reaction) + ")");
    }

    HttpResponsePtr reviews(const std::string &movieId, const std::string &sort = "",
                            const std::string &page = "",
                            const std::map<std::string, std::string> &cookies = {})
    {
        auto req = request(Get, "/api/v1/movies/" + movieId + "/reviews", Json::Value(), cookies);
        if (!sort.empty())
        {
            req->setParameter("sort", sort);
        }
        if (!page.empty())
        {
            req->setParameter("page", page);
        }
        return run(movieReviewsHandler(db, req, movieId));
    }

    // review_id отзывов страницы по порядку
    static std::vector<std::int64_t> ids(const HttpResponsePtr &response)
    {
        // body() возвращает значение: цикл по ссылке внутрь временного объекта повис бы
        const Json::Value json = body(response);
        std::vector<std::int64_t> result;
        for (const auto &review : json["reviews"])
        {
            result.push_back(review["review_id"].asInt64());
        }
        return result;
    }
};

// #17239: при равенстве - от новых к старым, по умолчанию «Сначала лучшие»
TEST_F(ReviewsTest, FourSortsWithNewestFirstOnTies)
{
    std::vector<std::int64_t> users;
    for (int i = 0; i < 3; ++i)
    {
        users.push_back(
            createUser("user" + std::to_string(i), "user" + std::to_string(i) + "@example.com"));
    }
    const auto oldest = addReview(kDeletedAuthor, 40);  // 1 «вверх», 1 «вниз»
    const auto older = addReview(kDeletedAuthor, 30);   // 2 «вверх»
    const auto newer = addReview(kDeletedAuthor, 20);   // 2 «вверх»
    const auto newest = addReview(kDeletedAuthor, 10);  // 3 «вниз»
    for (const auto reviewId : {older, newer})
    {
        react(reviewId, users[0], 1);
        react(reviewId, users[1], 1);
    }
    for (const auto user : users)
    {
        react(newest, user, -1);
    }
    react(oldest, users[0], 1);
    react(oldest, users[1], -1);

    using Ids = std::vector<std::int64_t>;
    EXPECT_EQ(ids(reviews("157336", "best")), (Ids{newer, older, oldest, newest}));
    EXPECT_EQ(ids(reviews("157336")), (Ids{newer, older, oldest, newest}));
    EXPECT_EQ(ids(reviews("157336", "worst")), (Ids{newest, oldest, newer, older}));
    EXPECT_EQ(ids(reviews("157336", "newest")), (Ids{newest, newer, older, oldest}));
    EXPECT_EQ(ids(reviews("157336", "oldest")), (Ids{oldest, older, newer, newest}));
}

// #17239 «Еще отзывы»: по 10, has_more - есть ли следующая страница
TEST_F(ReviewsTest, PagesOfTen)
{
    db->execSqlSync(
        "INSERT INTO reviews (movie_id, verdict, body, created_at) "
        "SELECT 157336, 'neutral', repeat('б', 300), now() - g * interval '1 minute' "
        "FROM generate_series(1, 25) AS g");

    std::set<std::int64_t> seen;
    const std::vector<std::pair<std::size_t, bool>> pages{{10, true}, {10, true}, {5, false}};
    for (std::size_t page = 0; page < pages.size(); ++page)
    {
        const auto response = reviews("157336", "newest", std::to_string(page + 1));
        EXPECT_EQ(response->statusCode(), k200OK);
        EXPECT_EQ(ids(response).size(), pages[page].first) << "страница " << page + 1;
        EXPECT_EQ(body(response)["has_more"].asBool(), pages[page].second) << page + 1;
        for (const auto id : ids(response))
        {
            seen.insert(id);
        }
    }
    EXPECT_EQ(seen.size(), 25U);  // страницы не повторяют друг друга
    const auto beyond = reviews("157336", "newest", "4");
    EXPECT_TRUE(ids(beyond).empty());
    EXPECT_FALSE(body(beyond)["has_more"].asBool());
}

TEST_F(ReviewsTest, MovieWithoutReviewsGivesEmptyList)
{
    // 603 нет и в movies: отзывов у него быть не может, тот же пустой список
    for (const char *movieId : {"157336", "603"})
    {
        const auto response = reviews(movieId);
        EXPECT_EQ(response->statusCode(), k200OK) << movieId;
        EXPECT_EQ(body(response)["reviews"], Json::Value(Json::arrayValue)) << movieId;
        EXPECT_FALSE(body(response)["has_more"].asBool()) << movieId;
    }
}

TEST_F(ReviewsTest, ReviewHasContractFields)
{
    const auto ivan = createUser("ivan", "ivan@example.com");
    const auto petr = createUser("petr", "petr@example.com");
    const auto review =
        db->execSqlSync(
              "INSERT INTO reviews (user_id, movie_id, verdict, title, body, is_edited, "
              "created_at) VALUES ($1, 157336, 'negative', 'Космос, время и отцовство', "
              "repeat('в', 300), true, '2024-11-12 12:37:00+00') RETURNING review_id",
              ivan)[0]["review_id"]
            .as<std::int64_t>();
    react(review, petr, 1);
    react(review, createUser("anna", "anna@example.com"), -1);
    react(review, createUser("oleg", "oleg@example.com"), -1);
    addReview(kDeletedAuthor, 60);

    // «12 ноября 2024, 15:37» по Москве из #17239 - в UTC 12:37; отзыв Ивана старше
    const auto response = reviews("157336", "oldest");
    EXPECT_EQ(response->statusCode(), k200OK);
    const Json::Value list = body(response)["reviews"];
    ASSERT_EQ(list.size(), 2U);
    EXPECT_EQ(list[0]["review_id"].asInt64(), review);
    EXPECT_EQ(list[0]["username"].asString(), "ivan");
    EXPECT_EQ(list[0]["created_at"].asString(), "2024-11-12T12:37:00Z");
    EXPECT_TRUE(list[0]["is_edited"].asBool());
    EXPECT_EQ(list[0]["verdict"].asString(), "negative");
    EXPECT_EQ(list[0]["title"].asString(), "Космос, время и отцовство");
    EXPECT_EQ(list[0]["body"].asString().size(), 600U);  // 300 кириллических символов
    EXPECT_EQ(list[0]["likes"].asInt64(), 1);
    EXPECT_EQ(list[0]["dislikes"].asInt64(), 2);
    EXPECT_EQ(list[0].size(), 9U);  // ровно поля MovieReview

    // автор удалён, заголовка нет, реакций нет
    EXPECT_TRUE(list[1]["username"].isNull());
    EXPECT_TRUE(list[1]["title"].isNull());
    EXPECT_FALSE(list[1]["is_edited"].asBool());
    EXPECT_EQ(list[1]["likes"].asInt64(), 0);
    EXPECT_EQ(list[1]["dislikes"].asInt64(), 0);
}

// #17029 п. 3: свой отзыв показывается первым отдельно, в списке его нет
TEST_F(ReviewsTest, OwnReviewIsNotInListWithSession)
{
    const auto ivan = createUser("ivan", "ivan@example.com");
    const auto own = addReview(ivan, 30);
    const auto other = addReview(createUser("petr", "petr@example.com"), 20);
    const auto deleted = addReview(kDeletedAuthor, 10);
    db->execSqlSync(
        "INSERT INTO sessions (token_hash, user_id, expires_at) "
        "VALUES (sha256('ivan-session'), " +
        std::to_string(ivan) + ", now() + interval '1 hour')");

    using Ids = std::vector<std::int64_t>;
    EXPECT_EQ(ids(reviews("157336", "newest")), (Ids{deleted, other, own}));
    EXPECT_EQ(ids(reviews("157336", "newest", "", {{"dv_session", "ivan-session"}})),
              (Ids{deleted, other}));
    // недействительная cookie - как гость
    EXPECT_EQ(ids(reviews("157336", "newest", "", {{"dv_session", "stale"}})),
              (Ids{deleted, other, own}));
}

TEST_F(ReviewsTest, InvalidMovieIdOrParameters)
{
    const auto notFound = reviews("abc");
    EXPECT_EQ(notFound->statusCode(), k404NotFound);
    EXPECT_EQ(body(notFound)["code"].asString(), "MOVIE_NOT_FOUND");

    for (const auto &[sort, page, field] :
         {std::tuple{"rating", "", "sort"}, std::tuple{"best", "0", "page"}})
    {
        const auto response = reviews("157336", sort, page);
        EXPECT_EQ(response->statusCode(), k400BadRequest) << field;
        EXPECT_EQ(body(response)["code"].asString(), "INVALID_PARAMETER") << field;
        EXPECT_EQ(body(response)["message"].asString(), "Некорректный запрос") << field;
        EXPECT_EQ(body(response)["field"].asString(), field);
    }
}
