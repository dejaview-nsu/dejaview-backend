#include "reviews.hpp"

#include "auth/session.hpp"
#include "error_response.hpp"
#include "movies/card.hpp"

#include <drogon/drogon.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <optional>
#include <utility>

using namespace drogon;

namespace
{
constexpr std::int64_t kPageSize = 10;

constexpr std::array<std::pair<std::string_view, ReviewsSort>, 4> kSorts{{
    {"best", ReviewsSort::Best},
    {"worst", ReviewsSort::Worst},
    {"newest", ReviewsSort::Newest},
    {"oldest", ReviewsSort::Oldest},
}};

// При равенстве - от новых к старым (#17239); review_id - чтобы равные по времени отзывы не
// менялись местами между страницами
std::string_view orderBy(ReviewsSort sort)
{
    switch (sort)
    {
        case ReviewsSort::Best:
            return "x.likes DESC, r.created_at DESC, r.review_id DESC";
        case ReviewsSort::Worst:
            return "x.dislikes DESC, r.created_at DESC, r.review_id DESC";
        case ReviewsSort::Newest:
            return "r.created_at DESC, r.review_id DESC";
        case ReviewsSort::Oldest:
            return "r.created_at, r.review_id";
    }
    return "";
}

Json::Value nullableText(const orm::Field &field)
{
    return field.isNull() ? Json::Value() : Json::Value(field.as<std::string>());
}
}  // namespace

std::expected<ReviewsQuery, std::string_view> parseReviewsQuery(std::string_view sort,
                                                                std::string_view page)
{
    ReviewsQuery query;
    if (!sort.empty())
    {
        const auto found = std::ranges::find(kSorts, sort, &decltype(kSorts)::value_type::first);
        if (found == kSorts.end())
        {
            return std::unexpected("sort");
        }
        query.sort = found->second;
    }
    if (!page.empty())
    {
        const char *end = page.data() + page.size();
        const auto [last, error] = std::from_chars(page.data(), end, query.page);
        if (error != std::errc{} || last != end || query.page < 1)
        {
            return std::unexpected("page");
        }
    }
    return query;
}

Task<HttpResponsePtr> movieReviewsHandler(orm::DbClientPtr db, HttpRequestPtr req,
                                          std::string movieId)
{
    const auto id = parseMovieId(movieId);
    if (!id)
    {
        co_return movieNotFound();
    }
    const auto query = parseReviewsQuery(req->getParameter("sort"), req->getParameter("page"));
    if (!query)
    {
        co_return errorResponse(k400BadRequest, "INVALID_PARAMETER", "Некорректный запрос",
                                query.error());
    }
    const auto user = co_await currentUser(db, req);

    // Реакции считаются для каждого отзыва фильма по первичному ключу review_reactions. Строка
    // сверх страницы - признак has_more. Удалённый автор (user_id NULL) виден всем
    const auto rows = co_await db->execSqlCoro(
        std::string(
            "SELECT r.review_id, u.username, "
            "to_char(r.created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') "
            "AS created_at, r.is_edited, r.verdict, r.title, r.body, x.likes, x.dislikes "
            "FROM reviews r LEFT JOIN users u ON u.user_id = r.user_id "
            "CROSS JOIN LATERAL (SELECT count(*) FILTER (WHERE reaction = 1) AS likes, "
            "count(*) FILTER (WHERE reaction = -1) AS dislikes "
            "FROM review_reactions WHERE review_id = r.review_id) x "
            "WHERE r.movie_id = $1 AND ($2::bigint IS NULL OR r.user_id IS DISTINCT FROM $2) "
            "ORDER BY ") +
            std::string(orderBy(query->sort)) + " LIMIT $3 OFFSET $4",
        *id, user ? std::optional(user->userId) : std::nullopt, kPageSize + 1,
        (query->page - 1) * kPageSize);

    Json::Value reviews(Json::arrayValue);
    for (std::size_t i = 0; i < std::min<std::size_t>(rows.size(), kPageSize); ++i)
    {
        const auto &row = rows[i];
        Json::Value review;
        review["review_id"] = Json::Int64(row["review_id"].as<std::int64_t>());
        review["username"] = nullableText(row["username"]);
        review["created_at"] = row["created_at"].as<std::string>();
        review["is_edited"] = row["is_edited"].as<bool>();
        review["verdict"] = row["verdict"].as<std::string>();
        review["title"] = nullableText(row["title"]);
        review["body"] = row["body"].as<std::string>();
        review["likes"] = Json::Int64(row["likes"].as<std::int64_t>());
        review["dislikes"] = Json::Int64(row["dislikes"].as<std::int64_t>());
        reviews.append(std::move(review));
    }
    Json::Value body;
    body["reviews"] = std::move(reviews);
    body["has_more"] = rows.size() > static_cast<std::size_t>(kPageSize);
    co_return HttpResponse::newHttpJsonResponse(body);
}
