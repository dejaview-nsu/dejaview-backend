#include "auth/http_common.hpp"

#include "error_response.hpp"

#include <chrono>

using namespace drogon;

const Json::Value *jsonBody(const HttpRequestPtr &req)
{
    const auto &body = req->getJsonObject();
    return body && body->isObject() ? body.get() : nullptr;
}

std::string stringField(const Json::Value &body, const char *name)
{
    const Json::Value &value = body[name];
    return value.isString() ? value.asString() : "";
}

int birthYearField(const Json::Value &body)
{
    const Json::Value &value = body["birth_year"];
    return value.isInt() ? value.asInt() : 0;
}

int currentYear()
{
    using namespace std::chrono;
    return static_cast<int>(year_month_day{floor<days>(system_clock::now())}.year());
}

HttpResponsePtr malformedBody()
{
    return errorResponse(k400BadRequest, "AUTH_VALIDATION_ERROR",
                         "Произошла ошибка. Попробуйте позже");
}

HttpResponsePtr fieldErrorResponse(const FieldError &error)
{
    return errorResponse(k400BadRequest, error.code, error.message, error.field);
}

std::string clientIp(const HttpRequestPtr &req)
{
    const std::string &realIp = req->getHeader("x-real-ip");
    return realIp.empty() ? req->peerAddr().toIp() : realIp;
}
