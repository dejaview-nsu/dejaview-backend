#include "ratings.hpp"

#include "auth/http_common.hpp"
#include "auth/session.hpp"
#include "error_response.hpp"
#include "movies/card.hpp"

#include <optional>

using namespace drogon;

namespace
{
constexpr int kMinRating = 1;
constexpr int kMaxRating = 10;

std::optional<int> ratingScore(const HttpRequestPtr &req)
{
    const auto *json = jsonBody(req);
    if (!json || !(*json)["score"].isInt())
    {
        return std::nullopt;
    }
    const int score = (*json)["score"].asInt();
    return score >= kMinRating && score <= kMaxRating ? std::optional(score) : std::nullopt;
}
}  // namespace

Task<HttpResponsePtr> setRatingHandler(orm::DbClientPtr db, HttpRequestPtr req, std::string movieId)
{
    const auto user = co_await requireSession(db, req);
    if (!user)
    {
        co_return user.error();
    }
    const auto id = parseMovieId(movieId);
    if (!id)
    {
        co_return movieNotFound();
    }
    const auto score = ratingScore(req);
    if (!score)
    {
        co_return errorResponse(k400BadRequest, "INVALID_PARAMETER",
                                "Оценка должна быть целым числом от 1 до 10", "score");
    }

    // История и оценка пишутся атомарно. Повтор того же балла не добавляет запись в историю.
    const auto movie = co_await db->execSqlCoro(
        "WITH movie AS (SELECT movie_id FROM movies WHERE movie_id = $2 FOR KEY SHARE), "
        "changed AS (INSERT INTO ratings (user_id, movie_id, score) "
        "SELECT $1::bigint, movie_id, $3::integer FROM movie "
        "ON CONFLICT (user_id, movie_id) DO UPDATE SET score = EXCLUDED.score "
        "WHERE ratings.score <> EXCLUDED.score RETURNING user_id, movie_id), "
        "logged AS (INSERT INTO user_actions (user_id, action, movie_id) "
        "SELECT user_id, 'rating_set', movie_id FROM changed) SELECT movie_id FROM movie",
        user->userId, *id, *score);
    if (movie.empty())
    {
        co_return movieNotFound();
    }
    co_return HttpResponse::newHttpResponse(k204NoContent, CT_NONE);
}

Task<HttpResponsePtr> removeRatingHandler(orm::DbClientPtr db, HttpRequestPtr req,
                                          std::string movieId)
{
    const auto user = co_await requireSession(db, req);
    if (!user)
    {
        co_return user.error();
    }
    const auto id = parseMovieId(movieId);
    if (!id)
    {
        co_return movieNotFound();
    }

    const auto movie = co_await db->execSqlCoro(
        "WITH movie AS (SELECT movie_id FROM movies WHERE movie_id = $2 FOR KEY SHARE), "
        "removed AS (DELETE FROM ratings USING movie WHERE ratings.movie_id = movie.movie_id "
        "AND ratings.user_id = $1 RETURNING ratings.user_id, ratings.movie_id), "
        "logged AS (INSERT INTO user_actions (user_id, action, movie_id) "
        "SELECT user_id, 'rating_remove', movie_id FROM removed) SELECT movie_id FROM movie",
        user->userId, *id);
    if (movie.empty())
    {
        co_return movieNotFound();
    }
    co_return HttpResponse::newHttpResponse(k204NoContent, CT_NONE);
}
