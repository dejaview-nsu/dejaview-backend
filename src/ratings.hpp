#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>

#include <string>

drogon::Task<drogon::HttpResponsePtr> setRatingHandler(drogon::orm::DbClientPtr db,
                                                       drogon::HttpRequestPtr req,
                                                       std::string movieId);

drogon::Task<drogon::HttpResponsePtr> removeRatingHandler(drogon::orm::DbClientPtr db,
                                                          drogon::HttpRequestPtr req,
                                                          std::string movieId);
