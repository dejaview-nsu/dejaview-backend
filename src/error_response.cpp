#include "error_response.hpp"

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
