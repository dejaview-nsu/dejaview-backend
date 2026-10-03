#pragma once

#include <drogon/HttpResponse.h>
#include <json/value.h>

#include <string_view>

Json::Value errorBody(std::string_view code, std::string_view message, std::string_view field = {});

drogon::HttpResponsePtr errorResponse(drogon::HttpStatusCode status, std::string_view code,
                                      std::string_view message, std::string_view field = {});

// 500 INTERNAL_ERROR
drogon::HttpResponsePtr internalError();
