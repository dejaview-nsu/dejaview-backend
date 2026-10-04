#include "error_response.hpp"

#include <drogon/drogon.h>

using namespace drogon;

Json::Value errorBody(std::string_view code, std::string_view message, std::string_view field)
{
    Json::Value body;
    body["code"] = std::string(code);
    body["message"] = std::string(message);
    body["field"] = field.empty() ? Json::Value() : Json::Value(std::string(field));
    return body;
}

HttpResponsePtr errorResponse(HttpStatusCode status, std::string_view code,
                              std::string_view message, std::string_view field)
{
    auto response = HttpResponse::newHttpJsonResponse(errorBody(code, message, field));
    response->setStatusCode(status);
    return response;
}

HttpResponsePtr internalError()
{
    return errorResponse(k500InternalServerError, "INTERNAL_ERROR",
                         "Произошла ошибка. Попробуйте позже");
}

void exceptionHandler(const std::exception &e, const HttpRequestPtr &req,
                      std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_ERROR << req->getMethodString() << ' ' << req->path() << ": " << e.what();
    callback(internalError());
}

HttpResponsePtr frameworkError(HttpStatusCode status)
{
    if (status == k404NotFound)
    {
        return errorResponse(status, "NOT_FOUND", "Не найдено");
    }
    if (status == k405MethodNotAllowed)
    {
        return errorResponse(status, "METHOD_NOT_ALLOWED", "Метод не поддерживается");
    }
    if (status >= k500InternalServerError)
    {
        auto response = internalError();
        response->setStatusCode(status);
        return response;
    }
    return errorResponse(status, "BAD_REQUEST", "Некорректный запрос");
}
