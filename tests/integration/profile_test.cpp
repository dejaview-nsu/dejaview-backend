#include "profile.hpp"

#include "auth/session.hpp"
#include "integration/db_test.hpp"

#include <cstdint>
#include <map>
#include <string>

using namespace drogon;

class ProfileTest : public DbTest
{
  protected:
    HttpResponsePtr profile(const std::map<std::string, std::string> &cookies = {})
    {
        return run(userProfileHandler(db, request(Get, "/api/v1/users/me", {}, cookies)));
    }

    std::int64_t createViewer()
    {
        const auto user = createUser("viewer", "viewer@example.com");
        db->execSqlSync(
            "INSERT INTO sessions (token_hash, user_id, expires_at) "
            "VALUES (sha256('viewer-session'), $1, now() + interval '1 hour')",
            user);
        return user;
    }

    const std::map<std::string, std::string> viewerCookie{{"dv_session", "viewer-session"}};
};

TEST_F(ProfileTest, ReturnsRegistrationDataAndZeroStatsForEmptyProfile)
{
    const auto viewer = createViewer();
    db->execSqlSync(
        "UPDATE users SET birth_year = 1998, created_at = '2026-09-28 19:00:00+07' "
        "WHERE user_id = $1",
        viewer);

    const auto response = profile(viewerCookie);
    ASSERT_EQ(response->statusCode(), k200OK);
    EXPECT_EQ(response->getHeader("cache-control"), "no-store");
    const auto json = body(response);
    EXPECT_EQ(json["username"], "viewer");
    EXPECT_EQ(json["birth_year"], 1998);
    EXPECT_EQ(json["registered_at"], "2026-09-28T12:00:00Z");
    const auto &stats = json["stats"];
    ASSERT_TRUE(stats["watched_count"].isIntegral());
    ASSERT_TRUE(stats["ratings_count"].isIntegral());
    ASSERT_TRUE(stats["average_rating"].isNumeric());
    ASSERT_TRUE(stats["reviews_count"].isIntegral());
    EXPECT_EQ(stats["watched_count"].asInt64(), 0);
    EXPECT_EQ(stats["ratings_count"].asInt64(), 0);
    EXPECT_DOUBLE_EQ(stats["average_rating"].asDouble(), 0.0);
    EXPECT_EQ(stats["reviews_count"].asInt64(), 0);
}

TEST_F(ProfileTest, CountsWatchedMoviesAndOnlyTheOwnersRatingsAndReviews)
{
    const auto viewer = createViewer();
    const auto other = createUser("other", "other@example.com");
    db->execSqlSync(
        "INSERT INTO movies (movie_id, title, original_title) "
        "VALUES (1, 'First', 'First'), (2, 'Second', 'Second'), (3, 'Third', 'Third')");
    db->execSqlSync(
        "INSERT INTO user_movie_lists (user_id, list_type, movie_id) "
        "VALUES ($1, 'watched', 1), ($1, 'want_to_watch', 2), ($2, 'watched', 1)",
        viewer, other);
    db->execSqlSync(
        "INSERT INTO ratings (user_id, movie_id, score) "
        "VALUES ($1, 1, 8), ($1, 3, 9), ($2, 1, 2)",
        viewer, other);
    db->execSqlSync(
        "INSERT INTO reviews (user_id, movie_id, verdict, body) "
        "VALUES ($1, 3, 'positive', repeat('a', 300)), "
        "($2, 1, 'negative', repeat('b', 300))",
        viewer, other);

    const auto response = profile(viewerCookie);
    ASSERT_EQ(response->statusCode(), k200OK);
    const auto stats = body(response)["stats"];
    EXPECT_EQ(stats["watched_count"].asInt64(), 1);
    EXPECT_EQ(stats["ratings_count"].asInt64(), 2);
    EXPECT_DOUBLE_EQ(stats["average_rating"].asDouble(), 8.5);
    EXPECT_EQ(stats["reviews_count"].asInt64(), 1);
}

TEST_F(ProfileTest, RequiresSessionForGuest)
{
    const auto response = profile();
    ASSERT_EQ(response->statusCode(), k401Unauthorized);
    EXPECT_EQ(body(response)["code"], "SESSION_REQUIRED");
}

TEST_F(ProfileTest, ClearsInvalidSessionCookie)
{
    const auto response = profile({{"dv_session", "missing-session"}});
    ASSERT_EQ(response->statusCode(), k401Unauthorized);
    EXPECT_EQ(body(response)["code"], "SESSION_EXPIRED");
    EXPECT_EQ(response->getCookie("dv_session").maxAge(), 0);
}

TEST_F(ProfileTest, RejectsExpiredSessionStillStoredInDatabase)
{
    const auto viewer = createViewer();
    db->execSqlSync(
        "UPDATE sessions SET created_at = now() - interval '2 hours', "
        "expires_at = now() - interval '1 hour' WHERE user_id = $1",
        viewer);

    const auto response = profile(viewerCookie);
    ASSERT_EQ(response->statusCode(), k401Unauthorized);
    EXPECT_EQ(body(response)["code"], "SESSION_EXPIRED");
    EXPECT_EQ(response->getCookie("dv_session").maxAge(), 0);
    EXPECT_FALSE(body(response).isMember("username"));
}

TEST_F(ProfileTest, ReturnsGenericErrorIfAccountDisappearsAfterSessionResolution)
{
    const auto viewer = createViewer();
    auto req = request(Get, "/api/v1/users/me", {}, viewerCookie);
    run(resolveSession(db, req));
    db->execSqlSync("DELETE FROM users WHERE user_id = $1", viewer);

    const auto response = run(userProfileHandler(db, req));
    ASSERT_EQ(response->statusCode(), k500InternalServerError);
    const auto json = body(response);
    EXPECT_EQ(json["code"], "INTERNAL_ERROR");
    EXPECT_FALSE(json.isMember("username"));
    EXPECT_FALSE(json.isMember("stats"));
}
