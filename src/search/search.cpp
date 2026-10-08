#include "search/search.hpp"

#include "auth/session.hpp"
#include "error_response.hpp"

#include <drogon/MultiPart.h>
#include <drogon/drogon.h>

#include <string>
#include <vector>

using namespace drogon;

namespace
{

const std::string kPosterBaseUrl = "https://image.tmdb.org/t/p/w500";

HttpResponsePtr searchErrorResponse(MediaKind kind, SearchError error)
{
    const ErrorReply reply = errorReply(kind, error);
    return errorResponse(static_cast<HttpStatusCode>(reply.status), std::string(reply.code),
                         std::string(reply.message), reply.fileField ? "file" : "");
}

HttpResponsePtr resultsResponse(Json::Value results)
{
    Json::Value body;
    body["results"] = std::move(results);
    return HttpResponse::newHttpJsonResponse(body);
}

// Карточка фильма из movies; пустой массив, если строки нет (контракт: такой фильм отбрасывается)
Task<Json::Value> movieCard(orm::DbClientPtr db, std::int64_t movieId, std::string requestId)
{
    const auto rows = co_await db->execSqlCoro(
        "SELECT movie_id, title, EXTRACT(YEAR FROM release_date)::int AS year, poster_path, "
        "overview FROM movies WHERE movie_id = $1",
        movieId);
    Json::Value results(Json::arrayValue);
    if (rows.empty())
    {
        LOG_WARN << "поиск: фильма " << movieId << " нет в movies, X-Request-Id " << requestId;
        co_return results;
    }
    const auto &row = rows.front();
    Json::Value card;
    card["movie_id"] = Json::Int64(row["movie_id"].as<std::int64_t>());
    card["title"] = row["title"].as<std::string>();
    card["year"] = row["year"].isNull() ? Json::Value() : Json::Value(row["year"].as<int>());
    card["poster_url"] = row["poster_path"].isNull()
                             ? Json::Value()
                             : Json::Value(kPosterBaseUrl + row["poster_path"].as<std::string>());
    card["overview"] =
        row["overview"].isNull() ? Json::Value() : Json::Value(row["overview"].as<std::string>());
    results.append(std::move(card));
    co_return results;
}

}  // namespace

Task<HttpResponsePtr> mediaSearchHandler(orm::DbClientPtr db, std::shared_ptr<MlService> ml,
                                         MediaKind kind, HttpRequestPtr req)
{
    const auto user = co_await requireSession(db, req);
    if (!user)
    {
        co_return user.error();
    }

    // Тело не multipart или не разбирается - файла нет
    MultiPartParser parser;
    std::vector<std::string_view> files;
    if (parser.parse(req) == 0)
    {
        for (const auto &file : parser.getFiles())
        {
            files.push_back(file.fileContent());
        }
    }
    const auto upload = checkUpload(kind, files);
    if (!upload)
    {
        co_return searchErrorResponse(kind, upload.error());
    }

    const std::string requestId = utils::getUuid();
    const auto ids = co_await searchMl(ml, kind, std::string(upload->mime),
                                       std::string(upload->content), requestId);
    if (!ids)
    {
        co_return searchErrorResponse(kind, ids.error());
    }
    if (ids->empty())
    {
        co_return resultsResponse(Json::Value(Json::arrayValue));
    }
    co_return resultsResponse(co_await movieCard(db, ids->front(), requestId));
}
