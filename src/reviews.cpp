#include "reviews.hpp"

#include "auth/http_common.hpp"
#include "auth/session.hpp"
#include "error_response.hpp"
#include "lists.hpp"
#include "movies/card.hpp"

#include <drogon/drogon.h>
#include <unicode/utf8.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

using namespace drogon;

namespace
{
constexpr std::int64_t kMovieReviewsPageSize = 10;
constexpr std::int64_t kProfileReviewsPageSize = 24;  // #17030 п. 2.4.2

constexpr char kReviewColumns[] =
    "r.review_id, u.username, "
    "to_char(r.created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') AS created_at, "
    "r.is_edited, r.verdict, r.title, r.body, x.likes, x.dislikes ";

constexpr char kReactionCounts[] =
    "CROSS JOIN LATERAL (SELECT count(*) FILTER (WHERE reaction = 1) AS likes, "
    "count(*) FILTER (WHERE reaction = -1) AS dislikes "
    "FROM review_reactions WHERE review_id = r.review_id) x ";

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

std::string_view orderBy(ListSort sort)
{
    switch (sort)
    {
        case ListSort::Newest:
            return "r.created_at DESC, r.review_id DESC";
        case ListSort::Oldest:
            return "r.created_at, r.review_id";
        case ListSort::TitleAsc:
            return "m.title, r.created_at DESC, r.review_id DESC";
        case ListSort::TitleDesc:
            return "m.title DESC, r.created_at DESC, r.review_id DESC";
        case ListSort::Best:
            return "rating.average_rating DESC NULLS LAST, r.created_at DESC, r.review_id DESC";
        case ListSort::Worst:
            return "rating.average_rating NULLS LAST, r.created_at DESC, r.review_id DESC";
    }
    return "";
}

Json::Value nullableText(const orm::Field &field)
{
    return field.isNull() ? Json::Value() : Json::Value(field.as<std::string>());
}

Json::Value reviewJson(const orm::Row &row)
{
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
    return review;
}

enum class ReviewWrite
{
    Create,
    Edit
};

struct ReviewFields
{
    std::optional<std::string> verdict;
    bool hasTitle = false;
    std::optional<std::string> title;
    std::optional<std::string> body;
};

bool validLength(std::string_view text, std::size_t min, std::size_t max)
{
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(text.data());
    std::size_t length = 0;
    for (std::size_t pos = 0; pos < text.size();)
    {
        UChar32 point = 0;
        U8_NEXT(bytes, pos, text.size(), point);
        // PostgreSQL text не хранит U+0000.
        if (point <= 0 || ++length > max)
        {
            return false;
        }
    }
    return length >= min;
}

std::expected<ReviewFields, std::string_view> parseReviewFields(const HttpRequestPtr &req,
                                                                ReviewWrite operation)
{
    // Drogon ищет application/json и в параметрах Content-Type. Для записи нужен сам тип.
    const auto *json = req->contentType() == CT_APPLICATION_JSON ? jsonBody(req) : nullptr;
    if (!json)
    {
        return std::unexpected(std::string_view{});
    }
    constexpr std::array<std::string_view, 3> kVerdicts{"positive", "neutral", "negative"};
    constexpr std::size_t kMaxTitleChars = 150;
    constexpr std::size_t kMinBodyChars = 300;
    constexpr std::size_t kMaxBodyChars = 10000;
    ReviewFields fields;
    if (operation == ReviewWrite::Create || json->isMember("verdict"))
    {
        auto value = stringField(*json, "verdict");
        if (std::ranges::find(kVerdicts, value) == kVerdicts.end())
        {
            return std::unexpected("verdict");
        }
        fields.verdict = std::move(value);
    }
    fields.hasTitle = json->isMember("title");
    if (const auto &title = (*json)["title"]; !title.isNull())
    {
        if (!title.isString())
        {
            return std::unexpected("title");
        }
        auto value = title.asString();
        if (!validLength(value, 0, kMaxTitleChars))
        {
            return std::unexpected("title");
        }
        if (!value.empty())
        {
            fields.title = std::move(value);
        }
    }
    if (operation == ReviewWrite::Create || json->isMember("body"))
    {
        auto value = stringField(*json, "body");
        if (!validLength(value, kMinBodyChars, kMaxBodyChars))
        {
            return std::unexpected("body");
        }
        fields.body = std::move(value);
    }
    if (!fields.verdict && !fields.hasTitle && !fields.body)
    {
        return std::unexpected(std::string_view{});
    }
    return fields;
}

HttpResponsePtr invalidReview(std::string_view field)
{
    return errorResponse(k400BadRequest, "VALIDATION_ERROR", "Некорректные данные отзыва", field);
}

HttpResponsePtr reviewNotFound()
{
    return errorResponse(k404NotFound, "REVIEW_NOT_FOUND", "Отзыв не найден");
}

HttpResponsePtr reviewNotOwner()
{
    return errorResponse(k403Forbidden, "REVIEW_NOT_OWNER",
                         "Отзыв доступен для изменения только автору");
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

    const bool includeOwn = user && query->page == 1;
    const std::int64_t offset = (query->page - 1) * kMovieReviewsPageSize;
    // На первой странице свой отзыв берётся в том же снимке БД. Строка для него и строка
    // для has_more идут сверх лимита массива других отзывов.
    const auto rows =
        co_await db->execSqlCoro(std::string("SELECT ") + kReviewColumns +
                                     ", coalesce(r.user_id = $2::bigint, false) AS is_own "
                                     "FROM reviews r LEFT JOIN users u ON u.user_id = r.user_id " +
                                     kReactionCounts +
                                     "WHERE r.movie_id = $1 AND ($5::boolean OR $2 IS NULL OR "
                                     "r.user_id IS DISTINCT FROM $2) "
                                     "ORDER BY " +
                                     (includeOwn ? "is_own DESC, " : "") +
                                     std::string(orderBy(query->sort)) + " LIMIT $3 OFFSET $4",
                                 *id, user ? std::optional(user->userId) : std::nullopt,
                                 kMovieReviewsPageSize + 1 + includeOwn, offset, includeOwn);

    Json::Value body;
    Json::Value reviews(Json::arrayValue);
    bool hasMore = false;
    for (const auto &row : rows)
    {
        if (row["is_own"].as<bool>())
        {
            body["own_review"] = reviewJson(row);
        }
        else if (reviews.size() == kMovieReviewsPageSize)
        {
            hasMore = true;
            break;
        }
        else
        {
            reviews.append(reviewJson(row));
        }
    }
    body["reviews"] = std::move(reviews);
    body["has_more"] = hasMore;
    auto response = HttpResponse::newHttpJsonResponse(body);
    response->addHeader("cache-control", "no-store");
    co_return response;
}

Task<HttpResponsePtr> myReviewsHandler(orm::DbClientPtr db, HttpRequestPtr req)
{
    const auto user = co_await requireSession(db, req);
    if (!user)
    {
        co_return user.error();
    }
    const auto query = parseListQuery(req->getParameter("sort"), req->getParameter("q"),
                                      req->getParameter("page"));
    if (!query)
    {
        co_return errorResponse(k400BadRequest, "INVALID_PARAMETER", "Некорректный запрос",
                                query.error());
    }
    if (!validLength(query->q, 0, query->q.size()))
    {
        co_return errorResponse(k400BadRequest, "INVALID_PARAMETER", "Некорректный запрос", "q");
    }

    // total считается до пагинации, реакции — только для отзывов страницы. Как в списках
    // профиля, числовое значение средней оценки форматирует PostgreSQL.
    const std::int64_t first = (query->page - 1) * kProfileReviewsPageSize + 1;
    const auto rows = co_await db->execSqlCoro(
        std::string(
            "WITH found AS ("
            "SELECT r.review_id, m.movie_id, m.title, m.poster_path, rating.average_rating, "
            "row_number() OVER (ORDER BY ") +
            std::string(orderBy(query->sort)) +
            ") AS n FROM reviews r JOIN movies m ON m.movie_id = r.movie_id "
            "CROSS JOIN LATERAL (SELECT round(avg(score), 1) AS average_rating "
            "FROM ratings WHERE movie_id = m.movie_id) rating "
            "WHERE r.user_id = $1 AND strpos(lower(m.title), lower($2::text COLLATE "
            "\"ru-RU-x-icu\")) > 0), "
            "page AS (SELECT * FROM found WHERE n BETWEEN $3 AND $4), "
            "items AS (SELECT p.n, json_build_object("
            "'movie', json_build_object('movie_id', p.movie_id, 'title', p.title, "
            "'poster_url', $5 || p.poster_path, 'average_rating', p.average_rating, "
            "'user_rating', own.score), 'review', review) AS item "
            "FROM page p JOIN reviews r ON r.review_id = p.review_id "
            "JOIN users u ON u.user_id = r.user_id "
            "LEFT JOIN ratings own ON own.user_id = $1 AND own.movie_id = p.movie_id " +
            kReactionCounts + "CROSS JOIN LATERAL (SELECT " + kReviewColumns +
            ") review) "
            "SELECT json_build_object('items', coalesce((SELECT json_agg(item ORDER BY n) "
            "FROM items), '[]'), 'total', (SELECT count(*) FROM found))::text AS page",
        user->userId, query->q, first, first + kProfileReviewsPageSize - 1,
        std::string(kPosterBaseUrl));

    auto response = HttpResponse::newHttpResponse(k200OK, CT_APPLICATION_JSON);
    response->setBody(rows[0]["page"].as<std::string>());
    response->addHeader("cache-control", "no-store");
    co_return response;
}

Task<HttpResponsePtr> createMovieReviewHandler(orm::DbClientPtr db, HttpRequestPtr req,
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
    const auto fields = parseReviewFields(req, ReviewWrite::Create);
    if (!fields)
    {
        co_return invalidReview(fields.error());
    }

    // UNIQUE (user_id, movie_id) разрешает параллельные публикации без отдельной проверки.
    // История пишется из RETURNING той же операции, конфликт её не меняет.
    const auto rows = co_await db->execSqlCoro(
        std::string(
            "WITH movie AS (SELECT movie_id FROM movies WHERE movie_id = $2 FOR KEY SHARE), "
            "created AS (INSERT INTO reviews (user_id, movie_id, verdict, title, body) "
            "SELECT $1::bigint, movie_id, $3::text, $4::text, $5::text FROM movie "
            "ON CONFLICT (user_id, movie_id) DO NOTHING RETURNING *), "
            "logged AS (INSERT INTO user_actions (user_id, action, movie_id) "
            "SELECT user_id, 'review_create', movie_id FROM created) SELECT ") +
            kReviewColumns +
            "FROM movie LEFT JOIN created r ON true "
            "LEFT JOIN users u ON u.user_id = r.user_id " +
            kReactionCounts,
        user->userId, *id, fields->verdict, fields->title, fields->body);
    if (rows.empty())
    {
        co_return movieNotFound();
    }
    if (rows[0]["review_id"].isNull())
    {
        co_return errorResponse(k409Conflict, "REVIEW_ALREADY_EXISTS",
                                "У вас уже есть отзыв об этом фильме");
    }
    auto response = HttpResponse::newHttpJsonResponse(reviewJson(rows[0]));
    response->setStatusCode(k201Created);
    co_return response;
}

Task<HttpResponsePtr> updateReviewHandler(orm::DbClientPtr db, HttpRequestPtr req,
                                          std::string reviewId)
{
    const auto user = co_await requireSession(db, req);
    if (!user)
    {
        co_return user.error();
    }
    const auto id = parseMovieId(reviewId);
    if (!id)
    {
        co_return reviewNotFound();
    }
    const auto fields = parseReviewFields(req, ReviewWrite::Edit);
    if (!fields)
    {
        co_return invalidReview(fields.error());
    }

    const auto rows = co_await db->execSqlCoro(
        std::string(
            "WITH target AS (SELECT review_id, user_id FROM reviews WHERE review_id = $1 "
            "FOR UPDATE), "
            "edited AS (UPDATE reviews SET verdict = coalesce($3::text, reviews.verdict), "
            "title = CASE WHEN $4::boolean THEN $5::text ELSE reviews.title END, "
            "body = coalesce($6::text, reviews.body), is_edited = true "
            "FROM target WHERE reviews.review_id = target.review_id AND target.user_id = $2 "
            "RETURNING reviews.*), "
            "logged AS (INSERT INTO user_actions (user_id, action, movie_id) "
            "SELECT user_id, 'review_edit', movie_id FROM edited) SELECT ") +
            kReviewColumns +
            "FROM target LEFT JOIN edited r ON true "
            "LEFT JOIN users u ON u.user_id = r.user_id " +
            kReactionCounts,
        *id, user->userId, fields->verdict, fields->hasTitle, fields->title, fields->body);
    if (rows.empty())
    {
        co_return reviewNotFound();
    }
    if (rows[0]["review_id"].isNull())
    {
        co_return reviewNotOwner();
    }
    co_return HttpResponse::newHttpJsonResponse(reviewJson(rows[0]));
}

Task<HttpResponsePtr> deleteReviewHandler(orm::DbClientPtr db, HttpRequestPtr req,
                                          std::string reviewId)
{
    const auto user = co_await requireSession(db, req);
    if (!user)
    {
        co_return user.error();
    }
    const auto id = parseMovieId(reviewId);
    if (!id)
    {
        co_return reviewNotFound();
    }

    const auto rows = co_await db->execSqlCoro(
        "WITH target AS (SELECT review_id, user_id FROM reviews WHERE review_id = $1 FOR UPDATE), "
        "removed AS (DELETE FROM reviews USING target WHERE reviews.review_id = target.review_id "
        "AND target.user_id = $2 RETURNING reviews.review_id, reviews.user_id, reviews.movie_id), "
        "logged AS (INSERT INTO user_actions (user_id, action, movie_id) "
        "SELECT user_id, 'review_delete', movie_id FROM removed) "
        "SELECT removed.review_id FROM target LEFT JOIN removed ON true",
        *id, user->userId);
    if (rows.empty())
    {
        co_return reviewNotFound();
    }
    if (rows[0]["review_id"].isNull())
    {
        co_return reviewNotOwner();
    }
    co_return HttpResponse::newHttpResponse(k204NoContent, CT_NONE);
}
