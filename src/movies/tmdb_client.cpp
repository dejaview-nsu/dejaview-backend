#include "movies/tmdb_client.hpp"

#include <drogon/drogon.h>

#include <chrono>
#include <utility>

using namespace drogon;

namespace
{
using Clock = CircuitBreaker::Clock;

constexpr double kTimeoutSeconds = 3.0;
// Как у ML: 5 сбоев подряд отключают вызовы на settings.openFor (30 с)
constexpr int kFailuresToOpen = 5;

struct TmdbReply
{
    HttpStatusCode status;
    std::shared_ptr<Json::Value> json;  // nullptr - тело не JSON
};

// GET к TMDB через выключатель. nullopt - вызов отключён или TMDB недоступен: нет соединения,
// тайм-аут, 5xx или 429 (лимит запросов, его тоже стоит переждать). Пока выключатель открыт,
// пробой служит сам запрос: TMDB ответил - вызовы снова идут
Task<std::optional<TmdbReply>> tmdbGet(std::shared_ptr<TmdbService> tmdb, std::string path,
                                       bool withAppends)
{
    const auto gate = tmdb->breaker.gate(Clock::now());
    if (gate == CircuitBreaker::Gate::Reject)
    {
        LOG_WARN << "Вызовы TMDB отключены после серии сбоев";
        co_return std::nullopt;
    }
    auto client = HttpClient::newHttpClient(tmdb->settings.url);
    auto request = HttpRequest::newHttpRequest();
    request->setMethod(Get);
    request->setPath(path);
    request->setParameter("language", "ru-RU");
    if (withAppends)
    {
        request->setParameter("append_to_response", "credits,release_dates,translations");
    }
    request->addHeader("Authorization", "Bearer " + tmdb->settings.accessToken);
    request->addHeader("Accept", "application/json");

    std::optional<TmdbReply> reply;
    try
    {
        const auto response = co_await client->sendRequestCoro(request, kTimeoutSeconds);
        const HttpStatusCode status = response->statusCode();
        if (status >= k500InternalServerError || status == k429TooManyRequests)
        {
            LOG_WARN << "TMDB ответил " << status << " на " << path;
        }
        else
        {
            reply = TmdbReply{status, response->getJsonObject()};
        }
    }
    catch (const std::exception &e)
    {
        LOG_WARN << "TMDB недоступен (" << path << "): " << e.what();
    }

    if (gate == CircuitBreaker::Gate::Probe)
    {
        tmdb->breaker.onProbe(reply.has_value(), Clock::now());
    }
    else if (reply)
    {
        tmdb->breaker.onReply();
    }
    else
    {
        tmdb->breaker.onOutage(Clock::now());
    }
    co_return reply;
}

// 401 - ключ неверный или отозван: так TMDB для нас выключен, это должен заметить мониторинг
void logUnexpected(const TmdbReply &reply, const std::string &what)
{
    if (reply.status == k401Unauthorized)
    {
        LOG_ERROR << "TMDB отклонил ключ (401): проверьте TMDB_ACCESS_TOKEN";
        return;
    }
    LOG_WARN << "TMDB: неожиданный ответ " << reply.status << " на " << what;
}

// Названия стран: из памяти, а в первый раз - из /configuration/countries
Task<std::optional<CountryNames>> countryNames(std::shared_ptr<TmdbService> tmdb)
{
    {
        const std::scoped_lock lock(tmdb->mutex);
        if (tmdb->countryNames)
        {
            co_return tmdb->countryNames;
        }
    }
    const auto reply = co_await tmdbGet(tmdb, "/3/configuration/countries", false);
    if (!reply)
    {
        co_return std::nullopt;
    }
    if (reply->status != k200OK || !reply->json || !reply->json->isArray())
    {
        logUnexpected(*reply, "список стран");
        co_return std::nullopt;
    }
    const std::scoped_lock lock(tmdb->mutex);
    tmdb->countryNames = parseCountryNames(*reply->json);
    co_return tmdb->countryNames;
}
}  // namespace

TmdbService::TmdbService(TmdbSettings settings)
    : settings(std::move(settings)), breaker(kFailuresToOpen, this->settings.openFor)
{
}

Task<std::expected<TmdbMovie, TmdbFailure>> fetchTmdbMovie(std::shared_ptr<TmdbService> tmdb,
                                                           std::int64_t movieId)
{
    if (tmdb->settings.accessToken.empty())
    {
        co_return std::unexpected(TmdbFailure::Unavailable);
    }
    const auto reply = co_await tmdbGet(tmdb, "/3/movie/" + std::to_string(movieId), true);
    if (!reply)
    {
        co_return std::unexpected(TmdbFailure::Unavailable);
    }
    if (reply->status == k404NotFound)
    {
        co_return std::unexpected(TmdbFailure::NotFound);
    }
    if (reply->status != k200OK || !reply->json)
    {
        logUnexpected(*reply, "фильм " + std::to_string(movieId));
        co_return std::unexpected(TmdbFailure::Unavailable);
    }
    // Страны нужны только найденному фильму: на 404 второй запрос не тратится
    const auto names = co_await countryNames(tmdb);
    if (!names)
    {
        co_return std::unexpected(TmdbFailure::Unavailable);
    }
    auto movie = parseTmdbMovie(*reply->json, movieId, *names);
    if (!movie)
    {
        LOG_INFO << "TMDB: фильм " << movieId
                 << " не подходит для каталога: " << rejectionReason(movie.error());
        co_return std::unexpected(TmdbFailure::NotFound);
    }
    co_return std::move(*movie);
}
