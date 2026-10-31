#include "movies.hpp"

#include "error_response.hpp"

#include <drogon/drogon.h>

#include <charconv>

using namespace drogon;

namespace
{
HttpResponsePtr movieNotFound()
{
    return errorResponse(k404NotFound, "MOVIE_NOT_FOUND", "Фильм не найден");
}
}  // namespace

std::optional<std::int64_t> parseMovieId(std::string_view text)
{
    std::int64_t id = 0;
    const char *end = text.data() + text.size();
    const auto [last, error] = std::from_chars(text.data(), end, id);
    if (error != std::errc{} || last != end || id <= 0)
    {
        return std::nullopt;
    }
    return id;
}

Task<HttpResponsePtr> movieCardHandler(orm::DbClientPtr db, std::string movieId)
{
    const auto id = parseMovieId(movieId);
    if (!id)
    {
        co_return movieNotFound();
    }
    // Карточку собирает PostgreSQL: NULL становится null, а JSONB-списки уходят как есть - их
    // формат в movies совпадает с контрактом (db-schema.md)
    const auto found = co_await db->execSqlCoro(
        "SELECT json_build_object('movie_id', movie_id, 'title', title, "
        "'original_title', original_title, 'year', EXTRACT(YEAR FROM release_date)::int, "
        "'age_rating', age_rating, 'poster_url', $2 || poster_path, 'runtime_min', runtime_min, "
        "'overview', overview, 'genres', genres, 'countries', countries, 'crew', crew)::text "
        "AS card FROM movies WHERE movie_id = $1",
        *id, std::string(kPosterBaseUrl));
    if (found.empty())
    {
        co_return movieNotFound();  // фильма нет в кеше: получение из TMDB - #18843
    }
    auto response = HttpResponse::newHttpResponse(k200OK, CT_APPLICATION_JSON);
    response->setBody(found[0]["card"].as<std::string>());
    co_return response;
}
