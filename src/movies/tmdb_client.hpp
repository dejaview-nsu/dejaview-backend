#pragma once

#include "movies/tmdb_movie.hpp"
#include "search/circuit_breaker.hpp"

#include <drogon/utils/coroutine.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

// Фильм из TMDB API для карточки (#17241, #18843): запрос как у сборки каталога (#17640),
// language=ru-RU и append_to_response=credits,release_dates,translations.

struct TmdbSettings
{
    std::string accessToken;  // TMDB_ACCESS_TOKEN; пустой - фильм не из кеша получает 503
    // Меняют только тесты: фейковый сервер в том же процессе (tests/integration/db_test.hpp) и
    // короткое отключение вызовов, чтобы проверить их возобновление без ожидания
    std::string url = "https://api.themoviedb.org";
    std::chrono::milliseconds openFor{30'000};  // на сколько сбои отключают вызовы
};

// Общее для всех запросов экземпляра backend: выключатель (#17296, «автоматическое отключение
// вызовов») и русские названия стран - их TMDB отдаёт отдельным запросом, они почти не меняются
struct TmdbService
{
    explicit TmdbService(TmdbSettings settings);

    TmdbSettings settings;
    CircuitBreaker breaker;
    std::mutex mutex;
    std::optional<CountryNames> countryNames;  // под mutex, после первого успешного ответа
};

enum class TmdbFailure
{
    NotFound,    // TMDB не знает movie_id или каталог такой фильм не взял бы
    Unavailable  // нет ключа, вызовы отключены, TMDB не ответил или ответил ошибкой
};

drogon::Task<std::expected<TmdbMovie, TmdbFailure>> fetchTmdbMovie(
    std::shared_ptr<TmdbService> tmdb, std::int64_t movieId);
