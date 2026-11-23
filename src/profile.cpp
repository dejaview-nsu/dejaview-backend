#include "profile.hpp"

#include "auth/session.hpp"
#include "error_response.hpp"

#include <json/value.h>
#include <trantor/utils/Logger.h>

#include <string>

drogon::Task<drogon::HttpResponsePtr> userProfileHandler(drogon::orm::DbClientPtr db,
                                                         drogon::HttpRequestPtr req)
{
    const auto user = co_await requireSession(db, req);
    if (!user)
    {
        co_return user.error();
    }

    const auto result = co_await db->execSqlCoro(
        R"sql(
        SELECT u.username, u.birth_year,
               to_char(u.created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS registered_at,
               (SELECT count(*) FROM user_movie_lists
                WHERE user_id = u.user_id AND list_type = 'watched') AS watched_count,
               r.ratings_count, r.average_rating,
               (SELECT count(*) FROM reviews WHERE user_id = u.user_id) AS reviews_count
        FROM users u
        CROSS JOIN LATERAL (
            SELECT count(*) AS ratings_count, coalesce(avg(score), 0) AS average_rating
            FROM ratings WHERE user_id = u.user_id
        ) r
        WHERE u.user_id = $1
        )sql",
        user->userId);
    if (result.empty())
    {
        LOG_WARN << "профиль владельца сессии не найден: " << user->userId;
        co_return internalError();
    }

    const auto row = result[0];
    Json::Value body(Json::objectValue);
    body["username"] = row["username"].as<std::string>();
    body["birth_year"] = row["birth_year"].as<int>();
    body["registered_at"] = row["registered_at"].as<std::string>();
    auto &stats = body["stats"];
    stats["watched_count"] = row["watched_count"].as<Json::Int64>();
    stats["ratings_count"] = row["ratings_count"].as<Json::Int64>();
    stats["average_rating"] = row["average_rating"].as<double>();
    stats["reviews_count"] = row["reviews_count"].as<Json::Int64>();

    auto response = drogon::HttpResponse::newHttpJsonResponse(body);
    response->addHeader("cache-control", "no-store");
    co_return response;
}
