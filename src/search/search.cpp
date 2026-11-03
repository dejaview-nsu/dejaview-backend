#include "search/search.hpp"

#include "auth/http_common.hpp"
#include "auth/session.hpp"
#include "error_response.hpp"
#include "movies/card.hpp"
#include "search/query.hpp"

#include <drogon/MultiPart.h>
#include <drogon/drogon.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace drogon;

namespace
{

HttpResponsePtr searchErrorResponse(MediaKind kind, SearchError error)
{
    const ErrorReply reply = errorReply(kind, error);
    return errorResponse(static_cast<HttpStatusCode>(reply.status), std::string(reply.code),
                         std::string(reply.message), std::string(reply.field));
}

HttpResponsePtr resultsResponse(Json::Value results)
{
    Json::Value body;
    body["results"] = std::move(results);
    return HttpResponse::newHttpJsonResponse(body);
}

// Сколько карточек отдаёт поиск: по тексту до 10 (openapi.yaml), по файлу - самую релевантную
std::size_t maxResults(MediaKind kind)
{
    constexpr std::size_t kTextResults = 10;
    return kind == MediaKind::Text ? kTextResults : 1;
}

// Карточки фильмов из movies одним запросом в порядке ids; фильмы, которых нет в базе,
// отбрасываются (контракт)
Task<Json::Value> movieCards(orm::DbClientPtr db, std::vector<std::int64_t> ids,
                             std::string requestId)
{
    std::string idArray = "{";
    for (const std::int64_t id : ids)
    {
        idArray += (idArray.size() > 1 ? "," : "") + std::to_string(id);
    }
    idArray += "}";
    const auto rows = co_await db->execSqlCoro(
        "SELECT m.movie_id, m.title, EXTRACT(YEAR FROM m.release_date)::int AS year, "
        "m.poster_path, m.overview "
        "FROM unnest($1::bigint[]) WITH ORDINALITY AS found(movie_id, position) "
        "JOIN movies m ON m.movie_id = found.movie_id ORDER BY found.position",
        idArray);
    if (rows.size() < ids.size())
    {
        LOG_WARN << "поиск: в movies нет " << ids.size() - rows.size() << " из " << ids.size()
                 << " найденных фильмов [X-Request-Id " << requestId << "]";
    }
    Json::Value results(Json::arrayValue);
    for (const auto &row : rows)
    {
        Json::Value card;
        card["movie_id"] = Json::Int64(row["movie_id"].as<std::int64_t>());
        card["title"] = row["title"].as<std::string>();
        card["year"] = row["year"].isNull() ? Json::Value() : Json::Value(row["year"].as<int>());
        card["poster_url"] =
            row["poster_path"].isNull()
                ? Json::Value()
                : Json::Value(std::string(kPosterBaseUrl) + row["poster_path"].as<std::string>());
        card["overview"] = row["overview"].isNull()
                               ? Json::Value()
                               : Json::Value(row["overview"].as<std::string>());
        results.append(std::move(card));
    }
    co_return results;
}

// Ответ 200 по id от ML: не больше maxResults(kind), пусто - results: []
Task<HttpResponsePtr> cardsResponse(orm::DbClientPtr db, MediaKind kind,
                                    std::vector<std::int64_t> ids, std::string requestId)
{
    ids.resize(std::min(ids.size(), maxResults(kind)));
    if (ids.empty())
    {
        co_return resultsResponse(Json::Value(Json::arrayValue));
    }
    co_return resultsResponse(co_await movieCards(db, std::move(ids), requestId));
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
    co_return co_await cardsResponse(db, kind, *ids, requestId);
}

Task<HttpResponsePtr> textSearchHandler(orm::DbClientPtr db, std::shared_ptr<MlService> ml,
                                        HttpRequestPtr req)
{
    // Нет тела, не JSON-объект или нет строки query - запрос не получен
    const Json::Value *json = jsonBody(req);
    const std::string raw = json == nullptr ? "" : stringField(*json, "query");
    const auto query = checkQuery(raw);
    if (!query)
    {
        co_return searchErrorResponse(MediaKind::Text, query.error());
    }

    // В ML уходит обрезанный запрос: его длину ML и проверяет
    Json::Value mlBody;
    mlBody["query"] = std::string(query->text);
    const std::string requestId = utils::getUuid();
    const auto ids = co_await searchMl(ml, MediaKind::Text, "application/json",
                                       mlBody.toStyledString(), requestId);
    if (!ids)
    {
        co_return searchErrorResponse(MediaKind::Text,
                                      resolveInvalidQuery(ids.error(), query->length));
    }
    co_return co_await cardsResponse(db, MediaKind::Text, *ids, requestId);
}
