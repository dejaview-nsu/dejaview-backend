#include "reviews.hpp"
#include "integration/db_test.hpp"
#include "profile.hpp"

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

namespace
{
using Cookies = std::map<std::string, std::string>;
const Cookies kAuthor{{"dv_session", "author-session"}};
const Cookies kOther{{"dv_session", "other-session"}};

std::string unicodeText(int length)
{
    std::string text;
    for (int i = 0; i < length; ++i)
    {
        text += "🎬";
    }
    return text;
}

Json::Value reviewInput(int textLength = 300)
{
    Json::Value json;
    json["verdict"] = "positive";
    json["body"] = unicodeText(textLength);
    return json;
}
}  // namespace

class ReviewWritesTest : public ReviewsTest
{
  protected:
    void SetUp() override
    {
        ReviewsTest::SetUp();
        if (IsSkipped())
        {
            return;
        }
        for (const auto &[name, cookie] :
             {std::pair{"author", "author-session"}, std::pair{"other", "other-session"}})
        {
            const auto id = createUser(name, std::string(name) + "@example.com");
            db->execSqlSync(
                "INSERT INTO sessions (token_hash, user_id, expires_at) "
                "VALUES (sha256($1::text::bytea), $2, now() + interval '1 hour')",
                std::string(cookie), id);
        }
    }

    HttpResponsePtr post(const Json::Value &json, const Cookies &cookies = kAuthor,
                         const std::string &movieId = "157336")
    {
        return run(createMovieReviewHandler(
            db, request(Post, "/api/v1/movies/" + movieId + "/reviews", json, cookies), movieId));
    }

    HttpResponsePtr edit(const std::string &id, const Json::Value &json,
                         const Cookies &cookies = kAuthor)
    {
        return run(
            updateReviewHandler(db, request(Patch, "/api/v1/reviews/" + id, json, cookies), id));
    }

    HttpResponsePtr remove(const std::string &id, const Cookies &cookies = kAuthor)
    {
        return run(
            deleteReviewHandler(db, request(Delete, "/api/v1/reviews/" + id, {}, cookies), id));
    }
};

TEST_F(ReviewWritesTest, AcceptsUnicodeBoundsAndOptionalTitle)
{
    auto json = reviewInput(10000);
    json["title"] = unicodeText(150);
    const auto response = post(json);
    ASSERT_EQ(response->statusCode(), k201Created);
    const auto review = body(response);
    EXPECT_GT(review["review_id"].asInt64(), 0);
    EXPECT_EQ(review["username"].asString(), "author");
    EXPECT_FALSE(review["created_at"].asString().empty());
    EXPECT_FALSE(review["is_edited"].asBool());
    EXPECT_EQ(review["verdict"], json["verdict"]);
    EXPECT_EQ(review["title"], json["title"]);
    EXPECT_EQ(review["body"], json["body"]);
    EXPECT_EQ(review["likes"].asInt64(), 0);
    EXPECT_EQ(review["dislikes"].asInt64(), 0);

    json = reviewInput(300);
    json["title"] = "";
    const auto other = post(json, kOther);
    ASSERT_EQ(other->statusCode(), k201Created);
    EXPECT_TRUE(body(other)["title"].isNull());
}

TEST_F(ReviewWritesTest, RejectsInvalidReviewFields)
{
    for (const int length : {0, 299, 10001})
    {
        const auto response = post(reviewInput(length));
        EXPECT_EQ(response->statusCode(), k400BadRequest) << length;
        EXPECT_EQ(body(response)["code"].asString(), "VALIDATION_ERROR");
        EXPECT_EQ(body(response)["field"].asString(), "body");
    }
    for (const auto &[field, value] : {std::pair{"title", Json::Value(unicodeText(151))},
                                       {"title", Json::Value(4)},
                                       {"body", Json::Value()},
                                       {"verdict", Json::Value("unknown")}})
    {
        auto json = reviewInput();
        json[field] = value;
        const auto response = post(json);
        EXPECT_EQ(response->statusCode(), k400BadRequest) << field;
        EXPECT_EQ(body(response)["field"].asString(), field);
    }
    auto json = reviewInput();
    json.removeMember("verdict");
    EXPECT_EQ(body(post(json))["field"].asString(), "verdict");
    json = reviewInput();
    json["body"] = unicodeText(300) + std::string(1, '\0');
    EXPECT_EQ(body(post(json))["field"].asString(), "body");
    const auto malformed = post(Json::Value(Json::arrayValue));
    EXPECT_EQ(malformed->statusCode(), k400BadRequest);
    EXPECT_TRUE(body(malformed)["field"].isNull());
    EXPECT_EQ(scalar("SELECT count(*) FROM reviews"), "0");
    EXPECT_EQ(scalar("SELECT count(*) FROM user_actions"), "0");
}

TEST_F(ReviewWritesTest, RejectsSecondReviewAndWritesOneAction)
{
    ASSERT_EQ(post(reviewInput())->statusCode(), k201Created);
    const auto duplicate = post(reviewInput());
    EXPECT_EQ(duplicate->statusCode(), k409Conflict);
    EXPECT_EQ(body(duplicate)["code"].asString(), "REVIEW_ALREADY_EXISTS");
    EXPECT_EQ(scalar("SELECT count(*) FROM reviews"), "1");
    EXPECT_EQ(scalar("SELECT action FROM user_actions"), "review_create");
}

TEST_F(ReviewWritesTest, RequiresSessionAndKnownIds)
{
    for (const auto &response :
         {post({}, {}, "invalid"), edit("invalid", {}, {}), remove("invalid", {})})
    {
        EXPECT_EQ(response->statusCode(), k401Unauthorized);
        EXPECT_EQ(body(response)["code"].asString(), "SESSION_REQUIRED");
    }
    const auto expired = post(reviewInput(), {{"dv_session", "stale"}});
    EXPECT_EQ(expired->statusCode(), k401Unauthorized);
    EXPECT_EQ(body(expired)["code"].asString(), "SESSION_EXPIRED");
    for (const auto &response :
         {post(reviewInput(), kAuthor, "999"), post(reviewInput(), kAuthor, "invalid")})
    {
        EXPECT_EQ(response->statusCode(), k404NotFound);
        EXPECT_EQ(body(response)["code"].asString(), "MOVIE_NOT_FOUND");
    }
    for (const auto &response : {edit("999", reviewInput()), remove("999"), remove("invalid")})
    {
        EXPECT_EQ(response->statusCode(), k404NotFound);
        EXPECT_EQ(body(response)["code"].asString(), "REVIEW_NOT_FOUND");
    }
}

TEST_F(ReviewWritesTest, EditPreservesPublicationDateAndUntouchedFields)
{
    auto json = reviewInput();
    json["title"] = "Название";
    const auto response = post(json);
    ASSERT_EQ(response->statusCode(), k201Created);
    const auto created = body(response);
    const auto id = created["review_id"].asString();
    EXPECT_EQ(edit(id, Json::Value(Json::objectValue))->statusCode(), k400BadRequest);
    Json::Value change;
    change["body"] = unicodeText(299);
    EXPECT_EQ(body(edit(id, change))["field"].asString(), "body");
    change = Json::Value(Json::objectValue);
    change["title"] = Json::Value();
    change["verdict"] = "negative";
    const auto updated = edit(id, change);
    ASSERT_EQ(updated->statusCode(), k200OK);
    const auto review = body(updated);
    EXPECT_EQ(review["created_at"], created["created_at"]);
    EXPECT_EQ(review["body"], created["body"]);
    EXPECT_TRUE(review["is_edited"].asBool());
    EXPECT_TRUE(review["title"].isNull());
    EXPECT_EQ(review["verdict"].asString(), "negative");
    EXPECT_EQ(scalar("SELECT action FROM user_actions WHERE action = 'review_edit'"),
              "review_edit");
}

TEST_F(ReviewWritesTest, ForeignAndDeletedAuthorReviewsCannotBeChanged)
{
    const auto created = post(reviewInput());
    ASSERT_EQ(created->statusCode(), k201Created);
    const auto id = body(created)["review_id"].asString();
    const auto deletedAuthorId = std::to_string(addReview(kDeletedAuthor, 5));
    for (const auto &response :
         {edit(id, reviewInput(), kOther), remove(id, kOther),
          edit(deletedAuthorId, reviewInput(), kOther), remove(deletedAuthorId, kOther)})
    {
        EXPECT_EQ(response->statusCode(), k403Forbidden);
        EXPECT_EQ(body(response)["code"].asString(), "REVIEW_NOT_OWNER");
    }
    EXPECT_EQ(scalar("SELECT count(*) FROM reviews WHERE NOT is_edited"), "2");
    EXPECT_EQ(scalar("SELECT count(*) FROM user_actions"), "1");
}

TEST_F(ReviewWritesTest, DeleteRemovesReviewAndAllowsNewPublication)
{
    const auto created = post(reviewInput());
    ASSERT_EQ(created->statusCode(), k201Created);
    const auto id = body(created)["review_id"].asString();
    const auto deleted = remove(id);
    ASSERT_EQ(deleted->statusCode(), k204NoContent);
    EXPECT_TRUE(deleted->body().empty());
    EXPECT_EQ(remove(id)->statusCode(), k404NotFound);
    EXPECT_EQ(scalar("SELECT count(*) FROM reviews"), "0");
    EXPECT_EQ(scalar("SELECT action FROM user_actions WHERE action = 'review_delete'"),
              "review_delete");
    EXPECT_EQ(post(reviewInput())->statusCode(), k201Created);
}

TEST_F(ReviewWritesTest, OwnReviewIsReturnedOnlyOnFirstPage)
{
    const auto own =
        addReview(std::stoll(scalar("SELECT user_id FROM users WHERE username = 'author'")), 50);
    const auto other =
        addReview(std::stoll(scalar("SELECT user_id FROM users WHERE username = 'other'")), 20);
    db->execSqlSync(
        "INSERT INTO reviews (movie_id, verdict, body, created_at) "
        "SELECT 157336, 'neutral', repeat('б', 300), now() - g * interval '1 minute' "
        "FROM generate_series(1, 10) AS g");
    for (const char *sort : {"best", "worst", "newest", "oldest"})
    {
        const auto first = reviews("157336", sort, "1", kAuthor);
        ASSERT_EQ(first->statusCode(), k200OK);
        const auto firstJson = body(first);
        EXPECT_EQ(first->getHeader("cache-control"), "no-store");
        ASSERT_TRUE(firstJson.isMember("own_review"));
        EXPECT_EQ(firstJson["own_review"]["review_id"].asInt64(), own);
        EXPECT_EQ(firstJson["own_review"]["username"].asString(), "author");
        EXPECT_EQ(firstJson["reviews"].size(), 10U);
        EXPECT_TRUE(firstJson["has_more"].asBool());
        const auto secondJson = body(reviews("157336", sort, "2", kAuthor));
        EXPECT_FALSE(secondJson.isMember("own_review"));
        EXPECT_EQ(secondJson["reviews"].size(), 1U);
        EXPECT_FALSE(secondJson["has_more"].asBool());
        std::set<std::int64_t> otherIds;
        for (const auto *page : {&firstJson, &secondJson})
        {
            for (const auto &review : (*page)["reviews"])
            {
                otherIds.insert(review["review_id"].asInt64());
            }
        }
        EXPECT_EQ(otherIds.size(), 11U);
        EXPECT_EQ(otherIds.count(own), 0U);
        EXPECT_EQ(otherIds.count(other), 1U);
    }
}

TEST_F(ReviewWritesTest, MovieReviewsHandleGuestStaleSessionAndAbsentOwnReview)
{
    const auto other =
        addReview(std::stoll(scalar("SELECT user_id FROM users WHERE username = 'other'")), 20);
    for (const auto &cookies : {Cookies{}, Cookies{{"dv_session", "stale"}}, kAuthor})
    {
        const auto response = reviews("157336", "newest", "1", cookies);
        ASSERT_EQ(response->statusCode(), k200OK);
        EXPECT_EQ(response->getHeader("cache-control"), "no-store");
        const auto json = body(response);
        EXPECT_FALSE(json.isMember("own_review"));
        ASSERT_EQ(json["reviews"].size(), 1U);
        EXPECT_EQ(json["reviews"][0]["review_id"].asInt64(), other);
        EXPECT_FALSE(json["has_more"].asBool());
    }
}

TEST_F(ReviewWritesTest, OwnReviewDoesNotCountTowardsHasMore)
{
    const auto own =
        addReview(std::stoll(scalar("SELECT user_id FROM users WHERE username = 'author'")), 20);
    const auto json = body(reviews("157336", "best", "1", kAuthor));
    EXPECT_EQ(json["own_review"]["review_id"].asInt64(), own);
    EXPECT_EQ(json["reviews"], Json::Value(Json::arrayValue));
    EXPECT_FALSE(json["has_more"].asBool());
}

class MyReviewsTest : public ReviewWritesTest
{
  protected:
    HttpResponsePtr myReviews(const std::string &sort = "", const std::string &q = "",
                              const std::string &page = "", const Cookies &cookies = kAuthor)
    {
        const auto req = request(Get, "/api/v1/users/me/reviews", {}, cookies);
        req->setParameter("sort", sort);
        req->setParameter("q", q);
        req->setParameter("page", page);
        return run(myReviewsHandler(db, req));
    }

    static std::vector<std::int64_t> movieIds(const HttpResponsePtr &response)
    {
        const auto json = body(response);
        std::vector<std::int64_t> ids;
        for (const auto &item : json["items"])
        {
            ids.push_back(item["movie"]["movie_id"].asInt64());
        }
        return ids;
    }

    Json::Value profileStats()
    {
        return body(
            run(userProfileHandler(db, request(Get, "/api/v1/users/me", {}, kAuthor))))["stats"];
    }
};

TEST_F(MyReviewsTest, OnlyOwnReviewsWithMovieAndReviewFields)
{
    const auto empty = myReviews();
    ASSERT_EQ(empty->statusCode(), k200OK);
    EXPECT_EQ(body(empty)["items"], Json::Value(Json::arrayValue));
    EXPECT_EQ(body(empty)["total"].asInt64(), 0);

    db->execSqlSync("UPDATE movies SET poster_path = '/interstellar.jpg' WHERE movie_id = 157336");
    const auto created = post(reviewInput());
    ASSERT_EQ(created->statusCode(), k201Created);
    auto foreign = reviewInput();
    foreign["verdict"] = "neutral";
    ASSERT_EQ(post(foreign, kOther)->statusCode(), k201Created);
    db->execSqlSync(
        "INSERT INTO ratings (user_id, movie_id, score) "
        "SELECT user_id, 157336, CASE username WHEN 'author' THEN 9 ELSE 8 END FROM users");
    const auto response = myReviews();
    ASSERT_EQ(response->statusCode(), k200OK);
    EXPECT_EQ(response->getHeader("cache-control"), "no-store");
    const auto json = body(response);
    EXPECT_EQ(json["total"].asInt64(), 1);
    ASSERT_EQ(json["items"].size(), 1U);
    const auto item = json["items"][0];
    EXPECT_EQ(item["review"], body(created));
    EXPECT_EQ(item["movie"]["movie_id"].asInt64(), 157336);
    EXPECT_EQ(item["movie"]["title"].asString(), "Интерстеллар");
    EXPECT_EQ(item["movie"]["poster_url"].asString(),
              "https://image.tmdb.org/t/p/w500/interstellar.jpg");
    EXPECT_DOUBLE_EQ(item["movie"]["average_rating"].asDouble(), 8.5);
    EXPECT_EQ(item["movie"]["user_rating"].asInt(), 9);
}

TEST_F(MyReviewsTest, RequiresSessionAndValidParameters)
{
    const auto anonymous = myReviews("invalid", "", "0", {});
    EXPECT_EQ(anonymous->statusCode(), k401Unauthorized);
    EXPECT_EQ(body(anonymous)["code"].asString(), "SESSION_REQUIRED");
    const auto expired = myReviews("", "", "", {{"dv_session", "stale"}});
    EXPECT_EQ(expired->statusCode(), k401Unauthorized);
    EXPECT_EQ(body(expired)["code"].asString(), "SESSION_EXPIRED");
    for (const auto &[sort, page, field] :
         {std::tuple{"invalid", "", "sort"}, std::tuple{"newest", "0", "page"},
          std::tuple{"newest", "2147483648", "page"}})
    {
        const auto response = myReviews(sort, "", page);
        EXPECT_EQ(response->statusCode(), k400BadRequest);
        EXPECT_EQ(body(response)["code"].asString(), "INVALID_PARAMETER");
        EXPECT_EQ(body(response)["field"].asString(), field);
    }
}

TEST_F(MyReviewsTest, SortsAndSearchesByMovieTitle)
{
    db->execSqlSync(
        "INSERT INTO movies (movie_id, title, original_title) VALUES "
        "(11, 'Атлас', 'Atlas'), (12, 'Север', 'North'), (13, 'Север', 'North II')");
    db->execSqlSync(
        "INSERT INTO reviews (user_id, movie_id, verdict, body, created_at) "
        "SELECT user_id, x.movie_id, 'negative', repeat('а', 300), now() - x.age * interval '1 "
        "minute' "
        "FROM users CROSS JOIN (VALUES (11, 5), (12, 10), (13, 15)) AS x(movie_id, age) "
        "WHERE username = 'author'");
    db->execSqlSync(
        "INSERT INTO ratings (user_id, movie_id, score) "
        "SELECT user_id, x.movie_id, x.score FROM users "
        "CROSS JOIN (VALUES (11, 9), (12, 7)) AS x(movie_id, score) WHERE username = 'other'");
    using Ids = std::vector<std::int64_t>;
    EXPECT_EQ(movieIds(myReviews()), (Ids{11, 12, 13}));
    EXPECT_EQ(movieIds(myReviews("oldest")), (Ids{13, 12, 11}));
    EXPECT_EQ(movieIds(myReviews("title_asc")), (Ids{11, 12, 13}));
    EXPECT_EQ(movieIds(myReviews("title_desc")), (Ids{12, 13, 11}));
    EXPECT_EQ(movieIds(myReviews("best")), (Ids{11, 12, 13}));
    EXPECT_EQ(movieIds(myReviews("worst")), (Ids{12, 11, 13}));
    const auto filtered = myReviews("oldest", "  еВЕ  ");
    EXPECT_EQ(movieIds(filtered), (Ids{13, 12}));
    EXPECT_EQ(body(filtered)["total"].asInt64(), 2);
    const auto unknown = myReviews("", "нет совпадений");
    EXPECT_TRUE(movieIds(unknown).empty());
    EXPECT_EQ(body(unknown)["total"].asInt64(), 0);
}

TEST_F(MyReviewsTest, RejectsSearchTextUnsupportedByPostgres)
{
    for (const auto &query : {std::string("Атлас\0другой", sizeof("Атлас\0другой") - 1),
                              std::string(1, static_cast<char>(0xff))})
    {
        const auto response = myReviews("", query);
        EXPECT_EQ(response->statusCode(), k400BadRequest);
        EXPECT_EQ(body(response)["code"].asString(), "INVALID_PARAMETER");
        EXPECT_EQ(body(response)["field"].asString(), "q");
    }
}

TEST_F(MyReviewsTest, StablePagesOfTwentyFourAndTotalPastLastPage)
{
    db->execSqlSync(
        "INSERT INTO movies (movie_id, title, original_title) "
        "SELECT g, 'Атлас', 'Atlas' FROM generate_series(101, 125) AS g");
    db->execSqlSync(
        "INSERT INTO reviews (user_id, movie_id, verdict, body, created_at) "
        "SELECT user_id, g, 'neutral', repeat('а', 300), '2024-11-12 12:37:00+00' "
        "FROM users CROSS JOIN generate_series(101, 125) AS g WHERE username = 'author' ORDER BY "
        "g");
    const auto first = myReviews("title_asc", "", "1");
    const auto firstIds = movieIds(first);
    ASSERT_EQ(firstIds.size(), 24U);
    EXPECT_EQ(firstIds.front(), 125);
    EXPECT_EQ(firstIds.back(), 102);
    const auto second = myReviews("title_asc", "", "2");
    EXPECT_EQ(movieIds(second), std::vector<std::int64_t>{101});
    const auto beyond = myReviews("title_asc", "", "3");
    EXPECT_TRUE(movieIds(beyond).empty());
    for (const auto &page : {first, second, beyond})
    {
        EXPECT_EQ(page->statusCode(), k200OK);
        EXPECT_EQ(body(page)["total"].asInt64(), 25);
    }
}

TEST_F(MyReviewsTest, ReviewCountFollowsCreationAndDeletion)
{
    EXPECT_EQ(profileStats()["reviews_count"].asInt64(), 0);
    const auto created = post(reviewInput());
    ASSERT_EQ(created->statusCode(), k201Created);
    EXPECT_EQ(profileStats()["reviews_count"].asInt64(), 1);
    const auto removed = remove(body(created)["review_id"].asString());
    ASSERT_EQ(removed->statusCode(), k204NoContent);
    EXPECT_EQ(profileStats()["reviews_count"].asInt64(), 0);
    EXPECT_TRUE(movieIds(myReviews()).empty());
}

TEST_F(MyReviewsTest, EditedBodyAppearsWithOriginalDateAndTitle)
{
    auto initial = reviewInput();
    initial["title"] = "Заголовок";
    const auto created = post(initial);
    ASSERT_EQ(created->statusCode(), k201Created);
    const auto review = body(created);
    Json::Value change;
    change["body"] = unicodeText(301);
    const auto updated = edit(review["review_id"].asString(), change);
    ASSERT_EQ(updated->statusCode(), k200OK);
    const auto listed = body(myReviews())["items"];
    ASSERT_EQ(listed.size(), 1U);
    const auto edited = listed[0]["review"];
    EXPECT_EQ(edited, body(updated));
    EXPECT_EQ(edited["created_at"], review["created_at"]);
    EXPECT_EQ(edited["title"], initial["title"]);
    EXPECT_EQ(edited["body"], change["body"]);
    EXPECT_TRUE(edited["is_edited"].asBool());
}

TEST_F(ReviewWritesTest, RequiresJsonMediaTypeForWrites)
{
    constexpr auto kMoviePath = "/api/v1/movies/157336/reviews";
    const auto withMediaType = [&](HttpMethod method, const std::string &path,
                                   const Json::Value &json, const std::string &mediaType)
    {
        auto req = request(method, path, Json::Value(), kAuthor);
        req->setBody(json.toStyledString());
        req->addHeader("content-type", mediaType);
        return req;
    };

    for (const std::string mediaType : {"text/plain", "application/json-seq"})
    {
        const auto rejected = run(createMovieReviewHandler(
            db, withMediaType(Post, kMoviePath, reviewInput(), mediaType), "157336"));
        EXPECT_EQ(rejected->statusCode(), k400BadRequest);
        EXPECT_EQ(body(rejected)["code"].asString(), "VALIDATION_ERROR");
    }
    EXPECT_EQ(scalar("SELECT count(*) FROM reviews"), "0");

    const auto created = run(createMovieReviewHandler(
        db, withMediaType(Post, kMoviePath, reviewInput(), "application/json; charset=utf-8"),
        "157336"));
    ASSERT_EQ(created->statusCode(), k201Created);
    const auto id = body(created)["review_id"].asString();
    Json::Value change;
    change["title"] = "Новый заголовок";
    const auto rejectedEdit = run(updateReviewHandler(
        db, withMediaType(Patch, "/api/v1/reviews/" + id, change, "application/json-seq"), id));
    EXPECT_EQ(rejectedEdit->statusCode(), k400BadRequest);
    EXPECT_EQ(body(rejectedEdit)["code"].asString(), "VALIDATION_ERROR");
    EXPECT_EQ(scalar("SELECT is_edited FROM reviews"), "f");
    EXPECT_EQ(scalar("SELECT count(*) FROM user_actions"), "1");
}

TEST_F(MyReviewsTest, PreservesPunctuationInReviewAndMovieSearch)
{
    const std::string title = "L'été & «Кадр»";
    db->execSqlSync("UPDATE movies SET title = $1 WHERE movie_id = 157336", title);
    auto input = reviewInput();
    input["title"] = title;
    input["body"] =
        std::string{"Текст с \"кавычками\", <угловыми скобками> и &."} + unicodeText(300);

    const auto created = post(input);
    ASSERT_EQ(created->statusCode(), k201Created);
    const auto response = myReviews("", "été &");
    ASSERT_EQ(response->statusCode(), k200OK);
    const auto items = body(response)["items"];
    ASSERT_EQ(items.size(), 1U);
    EXPECT_EQ(items[0]["movie"]["title"].asString(), title);
    EXPECT_EQ(items[0]["review"]["title"], input["title"]);
    EXPECT_EQ(items[0]["review"]["body"], input["body"]);
}
