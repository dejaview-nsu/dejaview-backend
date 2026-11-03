#include "movies/card.hpp"

#include "error_response.hpp"

#include <drogon/drogon.h>

#include <charconv>

using namespace drogon;

namespace
{
// Фото актёра на CDN TMDB: к адресу дописывается profile_path (контракт, MovieActor)
constexpr std::string_view kPhotoBaseUrl = "https://image.tmdb.org/t/p/w185";

HttpResponsePtr movieUnavailable()
{
    return errorResponse(k503ServiceUnavailable, "MOVIE_UNAVAILABLE",
                         "Не удалось загрузить информацию о фильме. Попробуйте позже");
}

HttpResponsePtr cardResponse(const std::string &card)
{
    auto response = HttpResponse::newHttpResponse(k200OK, CT_APPLICATION_JSON);
    response->setBody(card);
    return response;
}

// Карточка MovieCard из movies JSON-текстом или nullopt, если фильма в кеше нет.
// Карточку собирает PostgreSQL: NULL становится null, жанры, страны и группа уходят как есть -
// их формат в movies совпадает с контрактом. У актёров profile_path становится photo_url
Task<std::optional<std::string>> selectCard(orm::DbClientPtr db, std::int64_t movieId)
{
    const auto found = co_await db->execSqlCoro(
        "SELECT json_build_object('movie_id', movie_id, 'title', title, "
        "'original_title', original_title, 'year', EXTRACT(YEAR FROM release_date)::int, "
        "'age_rating', age_rating, 'poster_url', $2 || poster_path, 'runtime_min', runtime_min, "
        "'overview', overview, 'genres', genres, 'countries', countries, 'crew', crew, "
        "'actors', (SELECT coalesce(json_agg(json_build_object('person_id', a -> 'person_id', "
        "'name', a -> 'name', 'character', a -> 'character', "
        "'photo_url', $3 || (a ->> 'profile_path')) ORDER BY n), '[]') "
        "FROM jsonb_array_elements(actors) WITH ORDINALITY AS list(a, n)))::text "
        "AS card FROM movies WHERE movie_id = $1",
        movieId, std::string(kPosterBaseUrl), std::string(kPhotoBaseUrl));
    if (found.empty())
    {
        co_return std::nullopt;
    }
    co_return found[0]["card"].as<std::string>();
}

// Тем же INSERT ... ON CONFLICT, что и скрипты каталога и индексации (db-schema.md): гонка двух
// запросов одного фильма даёт одну строку. indexed_at не трогаем - его ставит индексация.
// runtime_min строкой: Drogon шлёт int как 4 байта, а колонка smallint
Task<> saveMovie(orm::DbClientPtr db, const TmdbMovie &movie)
{
    co_await db->execSqlCoro(
        "INSERT INTO movies (movie_id, title, original_title, release_date, age_rating, "
        "poster_path, runtime_min, overview, genres, countries, crew, actors, cached_at) "
        "VALUES ($1, $2, $3, $4::date, $5, $6, $7::smallint, $8, $9::jsonb, $10::jsonb, "
        "$11::jsonb, $12::jsonb, now()) "
        "ON CONFLICT (movie_id) DO UPDATE SET title = EXCLUDED.title, "
        "original_title = EXCLUDED.original_title, release_date = EXCLUDED.release_date, "
        "age_rating = EXCLUDED.age_rating, poster_path = EXCLUDED.poster_path, "
        "runtime_min = EXCLUDED.runtime_min, overview = EXCLUDED.overview, "
        "genres = EXCLUDED.genres, countries = EXCLUDED.countries, crew = EXCLUDED.crew, "
        "actors = EXCLUDED.actors, cached_at = EXCLUDED.cached_at",
        movie.movieId, movie.title, movie.originalTitle, movie.releaseDate, movie.ageRating,
        movie.posterPath,
        movie.runtimeMin ? std::optional(std::to_string(*movie.runtimeMin)) : std::nullopt,
        movie.overview, movie.genres.toStyledString(), movie.countries.toStyledString(),
        movie.crew.toStyledString(), movie.actors.toStyledString());
}
}  // namespace

HttpResponsePtr movieNotFound()
{
    return errorResponse(k404NotFound, "MOVIE_NOT_FOUND", "Фильм не найден");
}

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

Task<HttpResponsePtr> movieCardHandler(orm::DbClientPtr db, std::shared_ptr<TmdbService> tmdb,
                                       std::string movieId)
{
    const auto id = parseMovieId(movieId);
    if (!id)
    {
        co_return movieNotFound();
    }
    if (const auto card = co_await selectCard(db, *id))
    {
        co_return cardResponse(*card);
    }
    // Фильма нет в кеше: из TMDB и в movies, следующий запрос пойдёт уже из кеша (#17393 п. 2)
    const auto movie = co_await fetchTmdbMovie(tmdb, *id);
    if (!movie)
    {
        co_return movie.error() == TmdbFailure::NotFound ? movieNotFound() : movieUnavailable();
    }
    co_await saveMovie(db, *movie);
    co_return cardResponse((co_await selectCard(db, *id)).value());
}
