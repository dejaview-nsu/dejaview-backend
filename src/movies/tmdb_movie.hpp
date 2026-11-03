#pragma once

#include <json/value.h>

#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <string_view>

// Ответ TMDB /movie/{id}?append_to_response=credits,release_dates,translations -> строка movies
// (contracts/db-schema.md). Те же правила, что у сборки каталога scripts/catalog/movies/parser.py
// в dejaview-ml (#17640): фильм, который не взял бы каталог, не берёт и backend.

// Русские названия стран по коду ISO 3166-1, из /configuration/countries?language=ru-RU
using CountryNames = std::map<std::string, std::string, std::less<>>;

struct TmdbMovie
{
    std::int64_t movieId = 0;
    std::string title;  // русское
    std::string originalTitle;
    std::optional<std::string> releaseDate;  // YYYY-MM-DD
    std::optional<std::string> ageRating;    // 0+, 6+, 12+, 16+, 18+
    std::string posterPath;
    std::optional<int> runtimeMin;
    std::string overview;
    // Списки в формате JSONB-колонок movies, порядок - порядок показа
    Json::Value genres;
    Json::Value countries;
    Json::Value crew;
    Json::Value actors;
};

// Почему фильм не берётся: TMDB прислал другой фильм или каталог такой отсеивает (#17640)
enum class MovieRejection
{
    WrongId,
    Adult,
    NoPoster,
    NoRussianTitle,
    NoRussianOverview
};

std::string_view rejectionReason(MovieRejection rejection);

std::expected<TmdbMovie, MovieRejection> parseTmdbMovie(const Json::Value &details,
                                                        std::int64_t movieId,
                                                        const CountryNames &countryNames);

// Ответ /configuration/countries -> {код: название}. Страны без русского названия пропускаются.
CountryNames parseCountryNames(const Json::Value &countries);
