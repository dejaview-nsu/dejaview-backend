#include "lists.hpp"

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
constexpr std::int64_t kPageSize = 24;  // #17030 п. 2.4.2

constexpr std::array<std::pair<std::string_view, ListSort>, 6> kSorts{{
    {"newest", ListSort::Newest},
    {"oldest", ListSort::Oldest},
    {"title_asc", ListSort::TitleAsc},
    {"title_desc", ListSort::TitleDesc},
    {"best", ListSort::Best},
    {"worst", ListSort::Worst},
}};

// При равенстве - от недавно добавленных к ранним (контракт); movie_id - чтобы фильмы с одной
// датой добавления не менялись местами между страницами
std::string_view orderBy(ListSort sort)
{
    switch (sort)
    {
        case ListSort::Newest:
            return "l.added_at DESC, l.movie_id DESC";
        case ListSort::Oldest:
            return "l.added_at, l.movie_id";
        case ListSort::TitleAsc:
            return "m.title, l.added_at DESC, l.movie_id DESC";
        case ListSort::TitleDesc:
            return "m.title DESC, l.added_at DESC, l.movie_id DESC";
        case ListSort::Best:
            return "x.average_rating DESC NULLS LAST, l.added_at DESC, l.movie_id DESC";
        case ListSort::Worst:
            return "x.average_rating NULLS LAST, l.added_at DESC, l.movie_id DESC";
    }
    return "";
}

HttpResponsePtr invalidParameter(std::string_view field)
{
    return errorResponse(k400BadRequest, "INVALID_PARAMETER", "Некорректный запрос", field);
}

// Без listType источник плиток — собственные оценки пользователя.
Task<HttpResponsePtr> listPage(orm::DbClientPtr db, HttpRequestPtr req, std::int64_t userId,
                               std::optional<std::string> listType)
{
    const auto query = parseListQuery(req->getParameter("sort"), req->getParameter("q"),
                                      req->getParameter("page"));
    if (!query)
    {
        co_return invalidParameter(query.error());
    }

    // n - номер в порядке sort: страница - диапазон номеров, total верен и за последней страницей.
    // JSON собирает PostgreSQL: средняя оценка уходит как 8.6, а не 8.5999999999999996 из double
    const std::int64_t first = (query->page - 1) * kPageSize + 1;
    const auto result = co_await db->execSqlCoro(
        std::string(
            "WITH entries AS (SELECT l.movie_id, l.added_at, own.score AS user_rating "
            "FROM user_movie_lists l "
            "LEFT JOIN ratings own ON own.user_id = l.user_id AND own.movie_id = l.movie_id "
            "WHERE l.user_id = $1 AND l.list_type = $2::text "
            "UNION ALL SELECT movie_id, created_at, score FROM ratings "
            "WHERE user_id = $1 AND $2::text IS NULL), "
            "found AS (SELECT m.movie_id, m.title, m.poster_path, x.average_rating, "
            "l.user_rating, row_number() OVER (ORDER BY ") +
            std::string(orderBy(query->sort)) +
            ") AS n FROM entries l JOIN movies m ON m.movie_id = l.movie_id "
            "CROSS JOIN LATERAL (SELECT round(avg(score), 1) AS average_rating "
            "FROM ratings WHERE movie_id = l.movie_id) x "
            "WHERE strpos(lower(m.title), lower($3::text COLLATE \"ru-RU-x-icu\")) > 0) "
            "SELECT json_build_object('movies', coalesce((SELECT json_agg(json_build_object("
            "'movie_id', movie_id, 'title', title, 'poster_url', $6 || poster_path, "
            "'average_rating', average_rating, 'user_rating', user_rating) ORDER BY n) "
            "FROM found WHERE n BETWEEN $4 AND $5), '[]'), "
            "'total', (SELECT count(*) FROM found))::text AS page",
        userId, listType, query->q, first, first + kPageSize - 1, std::string(kPosterBaseUrl));

    auto response = HttpResponse::newHttpResponse(k200OK, CT_APPLICATION_JSON);
    response->setBody(result[0]["page"].as<std::string>());
    co_return response;
}
}  // namespace

bool isListType(std::string_view text) { return text == "want_to_watch" || text == "watched"; }

std::expected<ListQuery, std::string_view> parseListQuery(std::string_view sort, std::string_view q,
                                                          std::string_view page)
{
    ListQuery query;
    if (!sort.empty())
    {
        const auto found = std::ranges::find(kSorts, sort, &decltype(kSorts)::value_type::first);
        if (found == kSorts.end())
        {
            return std::unexpected("sort");
        }
        query.sort = found->second;
    }
    constexpr std::string_view kSpaces = " \t\n\v\f\r";
    if (const auto first = q.find_first_not_of(kSpaces); first != std::string_view::npos)
    {
        query.q = q.substr(first, q.find_last_not_of(kSpaces) - first + 1);
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

Task<HttpResponsePtr> listMoviesHandler(orm::DbClientPtr db, HttpRequestPtr req,
                                        std::string listType)
{
    const auto user = co_await requireSession(db, req);
    if (!user)
    {
        co_return user.error();
    }
    if (!isListType(listType))
    {
        co_return invalidParameter("list_type");
    }
    co_return co_await listPage(db, req, user->userId, std::move(listType));
}

Task<HttpResponsePtr> listRatingsHandler(orm::DbClientPtr db, HttpRequestPtr req)
{
    const auto user = co_await requireSession(db, req);
    if (!user)
    {
        co_return user.error();
    }
    co_return co_await listPage(db, req, user->userId, std::nullopt);
}

Task<HttpResponsePtr> addToListHandler(orm::DbClientPtr db, HttpRequestPtr req,
                                       std::string listType, std::string movieId)
{
    const auto user = co_await requireSession(db, req);
    if (!user)
    {
        co_return user.error();
    }
    if (!isListType(listType))
    {
        co_return invalidParameter("list_type");
    }
    const auto id = parseMovieId(movieId);
    if (!id)
    {
        co_return movieNotFound();
    }

    // Один запрос - одна транзакция: строка списка и запись в историю вместе. Фильм уже в
    // списке - ON CONFLICT ничего не вставляет: дата добавления прежняя, в историю не пишется
    const auto movie = co_await db->execSqlCoro(
        "WITH movie AS (SELECT movie_id FROM movies WHERE movie_id = $3), "
        "added AS (INSERT INTO user_movie_lists (user_id, list_type, movie_id) "
        "SELECT $1::bigint, $2::text, movie_id FROM movie ON CONFLICT DO NOTHING "
        "RETURNING movie_id), "
        "logged AS (INSERT INTO user_actions (user_id, action, movie_id) "
        "SELECT $1::bigint, $2::text || '_add', movie_id FROM added) "
        "SELECT movie_id FROM movie",
        user->userId, listType, *id);
    if (movie.empty())
    {
        co_return movieNotFound();
    }
    co_return HttpResponse::newHttpResponse(k204NoContent, CT_NONE);
}

Task<HttpResponsePtr> removeFromListHandler(orm::DbClientPtr db, HttpRequestPtr req,
                                            std::string listType, std::string movieId)
{
    const auto user = co_await requireSession(db, req);
    if (!user)
    {
        co_return user.error();
    }
    if (!isListType(listType))
    {
        co_return invalidParameter("list_type");
    }
    const auto id = parseMovieId(movieId);
    if (!id)
    {
        co_return movieNotFound();
    }

    // В историю - только если строка правда удалена
    co_await db->execSqlCoro(
        "WITH removed AS (DELETE FROM user_movie_lists "
        "WHERE user_id = $1 AND list_type = $2 AND movie_id = $3 RETURNING movie_id) "
        "INSERT INTO user_actions (user_id, action, movie_id) "
        "SELECT $1, $2 || '_remove', movie_id FROM removed",
        user->userId, listType, *id);
    co_return HttpResponse::newHttpResponse(k204NoContent, CT_NONE);
}

Task<HttpResponsePtr> movieListsStateHandler(orm::DbClientPtr db, HttpRequestPtr req,
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

    const auto rows = co_await db->execSqlCoro(
        "SELECT count(*) FILTER (WHERE list_type = 'want_to_watch') > 0 AS want_to_watch, "
        "count(*) FILTER (WHERE list_type = 'watched') > 0 AS watched, "
        "(SELECT score FROM ratings WHERE user_id = $1 AND movie_id = $2) AS user_rating "
        "FROM user_movie_lists WHERE user_id = $1 AND movie_id = $2",
        user->userId, *id);
    const auto row = rows[0];
    const auto score = row["user_rating"];
    Json::Value state;
    state["want_to_watch"] = row["want_to_watch"].as<bool>();
    state["watched"] = row["watched"].as<bool>();
    state["user_rating"] = score.isNull() ? Json::Value() : Json::Value(score.as<int>());
    co_return HttpResponse::newHttpJsonResponse(state);
}
