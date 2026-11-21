#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>

drogon::Task<drogon::HttpResponsePtr> userProfileHandler(drogon::orm::DbClientPtr db,
                                                         drogon::HttpRequestPtr req);
