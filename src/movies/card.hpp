#pragma once

#include "movies/tmdb_client.hpp"

#include <drogon/HttpResponse.h>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

// Карточка фильма (#17630, тег Movies в api/openapi.yaml). Источник - таблица movies, кеш TMDB
// (#17393 п. 2, contracts/db-schema.md); фильма там нет - TMDB, и фильм сохраняется (#18843).

// Начало URL постера на CDN TMDB, к нему дописывается movies.poster_path (db-schema.md).
inline constexpr std::string_view kPosterBaseUrl = "https://image.tmdb.org/t/p/w500";

// movie_id из пути: целое положительное число, иначе nullopt.
std::optional<std::int64_t> parseMovieId(std::string_view text);

// 404 MOVIE_NOT_FOUND «Фильм не найден»
drogon::HttpResponsePtr movieNotFound();

// GET /movies/{movie_id}: MovieCard, гостю тоже. movie_id не число или фильма нет и в TMDB -
// 404, фильма нет в кеше, а TMDB недоступен - 503.
drogon::Task<drogon::HttpResponsePtr> movieCardHandler(drogon::orm::DbClientPtr db,
                                                       std::shared_ptr<TmdbService> tmdb,
                                                       std::string movieId);
