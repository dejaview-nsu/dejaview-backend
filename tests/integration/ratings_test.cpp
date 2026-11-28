#include "ratings.hpp"
#include "integration/db_test.hpp"

#include <cstdint>
#include <map>
#include <string>

using namespace drogon;

namespace
{
using Cookies = std::map<std::string, std::string>;

const Cookies kIvan{{"dv_session", "ivan-session"}};
constexpr char kMovieId[] = "157336";

Json::Value rating(const Json::Value &score)
{
    Json::Value json(Json::objectValue);
    json["score"] = score;
    return json;
}
}  // namespace

class RatingsTest : public DbTest
{
  protected:
    void SetUp() override
    {
        DbTest::SetUp();
        if (IsSkipped())
        {
            return;
        }
        ivan = createUser("ivan", "ivan@example.com");
        db->execSqlSync(
            "INSERT INTO sessions (token_hash, user_id, expires_at) "
            "VALUES (sha256('ivan-session'), $1, now() + interval '1 hour')",
            ivan);
        db->execSqlSync(
            "INSERT INTO movies (movie_id, title, original_title) "
            "VALUES (157336, 'Интерстеллар', 'Interstellar')");
    }

    HttpResponsePtr put(const Json::Value &json, const std::string &movieId = kMovieId,
                        const Cookies &cookies = kIvan)
    {
        return run(setRatingHandler(
            db, request(Put, "/api/v1/movies/" + movieId + "/rating", json, cookies), movieId));
    }

    HttpResponsePtr remove(const std::string &movieId = kMovieId, const Cookies &cookies = kIvan)
    {
        return run(removeRatingHandler(
            db, request(Delete, "/api/v1/movies/" + movieId + "/rating", {}, cookies), movieId));
    }

    std::int64_t ivan = 0;
};

TEST_F(RatingsTest, SavesRatingAndLogsAction)
{
    const auto response = put(rating(1));
    ASSERT_EQ(response->statusCode(), k204NoContent);
    EXPECT_TRUE(response->body().empty());
    EXPECT_EQ(scalar("SELECT score FROM ratings WHERE user_id = (SELECT user_id FROM users) "
                     "AND movie_id = 157336"),
              "1");
    EXPECT_EQ(scalar("SELECT action || ' ' || movie_id FROM user_actions"), "rating_set 157336");
}

TEST_F(RatingsTest, RepeatedPutAndChangeKeepOneRatingAndOriginalDate)
{
    ASSERT_EQ(put(rating(1))->statusCode(), k204NoContent);
    db->execSqlSync("UPDATE ratings SET created_at = '2026-01-01 00:00:00+00'");
    ASSERT_EQ(put(rating(1.0))->statusCode(), k204NoContent);
    EXPECT_EQ(scalar("SELECT count(*) FROM user_actions"), "1");

    ASSERT_EQ(put(rating(10))->statusCode(), k204NoContent);
    EXPECT_EQ(scalar("SELECT count(*) FROM ratings"), "1");
    EXPECT_EQ(scalar("SELECT score FROM ratings"), "10");
    EXPECT_EQ(scalar("SELECT created_at = '2026-01-01 00:00:00+00' FROM ratings"), "t");
    EXPECT_EQ(scalar("SELECT string_agg(action, ' ' ORDER BY action_id) FROM user_actions"),
              "rating_set rating_set");
}

TEST_F(RatingsTest, DeleteRemovesOnlyOwnRatingAndIsIdempotent)
{
    const auto petr = createUser("petr", "petr@example.com");
    db->execSqlSync("INSERT INTO ratings (user_id, movie_id, score) VALUES ($1, 157336, 8)", petr);
    ASSERT_EQ(put(rating(6))->statusCode(), k204NoContent);
    const auto response = remove();
    ASSERT_EQ(response->statusCode(), k204NoContent);
    EXPECT_TRUE(response->body().empty());
    EXPECT_EQ(remove()->statusCode(), k204NoContent);
    EXPECT_EQ(scalar("SELECT string_agg(score::text, ',') FROM ratings"), "8");
    EXPECT_EQ(scalar("SELECT string_agg(action, ' ' ORDER BY action_id) FROM user_actions"),
              "rating_set rating_remove");
}

TEST_F(RatingsTest, RejectsMissingAndInvalidScore)
{
    for (const auto &json :
         {Json::Value(), Json::Value(Json::objectValue), Json::Value(Json::arrayValue), rating(0),
          rating(11), rating(5.5), rating("8"), rating(true), rating(Json::Value())})
    {
        const auto response = put(json);
        EXPECT_EQ(response->statusCode(), k400BadRequest) << json;
        EXPECT_EQ(body(response)["code"].asString(), "INVALID_PARAMETER") << json;
        EXPECT_EQ(body(response)["field"].asString(), "score") << json;
    }
    EXPECT_EQ(scalar("SELECT count(*) FROM ratings"), "0");
    EXPECT_EQ(scalar("SELECT count(*) FROM user_actions"), "0");
}

TEST_F(RatingsTest, ChecksSessionBeforeMovieAndScore)
{
    for (const auto &response : {put(rating(0), "invalid", {}), remove("invalid", {})})
    {
        EXPECT_EQ(response->statusCode(), k401Unauthorized);
        EXPECT_EQ(body(response)["code"].asString(), "SESSION_REQUIRED");
    }
    const auto expired = put(rating(8), kMovieId, {{"dv_session", "stale"}});
    EXPECT_EQ(expired->statusCode(), k401Unauthorized);
    EXPECT_EQ(body(expired)["code"].asString(), "SESSION_EXPIRED");
    EXPECT_EQ(scalar("SELECT count(*) FROM ratings"), "0");
}

TEST_F(RatingsTest, InvalidIdOrMissingMovieGivesNotFound)
{
    for (const auto &response :
         {put(rating(0), "invalid"), put(rating(7), "603"), remove("0"), remove("603")})
    {
        EXPECT_EQ(response->statusCode(), k404NotFound);
        EXPECT_EQ(body(response)["code"].asString(), "MOVIE_NOT_FOUND");
    }
    EXPECT_EQ(put(rating(0), "603")->statusCode(), k400BadRequest);
}
