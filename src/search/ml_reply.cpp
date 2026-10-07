#include "search/ml_reply.hpp"

#include <string>

namespace
{

constexpr int kStatusOk = 200;
constexpr int kFirstClientError = 400;
constexpr int kFirstServerError = 500;
constexpr int kEndOfServerErrors = 600;

// Любое отклонение от контракта ответа 200 - не авария: сервис ответил
std::expected<std::vector<std::int64_t>, MlFailure> parseIds(const Json::Value *body)
{
    const MlFailure malformed{SearchError::Unavailable, false};
    if (body == nullptr || !body->isObject() || !(*body)["movie_ids"].isArray())
    {
        return std::unexpected(malformed);
    }
    std::vector<std::int64_t> ids;
    for (const Json::Value &id : (*body)["movie_ids"])
    {
        if (!id.isInt64() || id.asInt64() <= 0)
        {
            return std::unexpected(malformed);
        }
        ids.push_back(id.asInt64());
    }
    return ids;
}

SearchError clientError(const Json::Value *body)
{
    if (body == nullptr || !body->isObject() || !(*body)["code"].isString())
    {
        return SearchError::Unavailable;
    }
    const std::string code = (*body)["code"].asString();
    if (code == "FILE_TOO_LARGE")
    {
        return SearchError::FileTooLarge;
    }
    if (code == "UNSUPPORTED_MEDIA_TYPE")
    {
        return SearchError::UnsupportedFormat;
    }
    if (code == "FILE_CORRUPTED")
    {
        return SearchError::FileCorrupted;
    }
    if (code == "DURATION_TOO_LONG")
    {
        return SearchError::VideoTooLong;
    }
    return SearchError::Unavailable;
}

}  // namespace

std::expected<std::vector<std::int64_t>, MlFailure> parseMlReply(MediaKind kind, int status,
                                                                 const Json::Value *body)
{
    if (status == kStatusOk)
    {
        return parseIds(body);
    }
    if (status >= kFirstClientError && status < kFirstServerError)
    {
        return std::unexpected(MlFailure{forKind(kind, clientError(body)), false});
    }
    const bool outage = status >= kFirstServerError && status < kEndOfServerErrors;
    return std::unexpected(MlFailure{SearchError::Unavailable, outage});
}
