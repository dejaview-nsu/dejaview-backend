#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <json/value.h>

#include <exception>
#include <functional>
#include <string_view>

Json::Value errorBody(std::string_view code, std::string_view message, std::string_view field = {});

drogon::HttpResponsePtr errorResponse(drogon::HttpStatusCode status, std::string_view code,
                                      std::string_view message, std::string_view field = {});

// 500 INTERNAL_ERROR
drogon::HttpResponsePtr internalError();

// Для app().setExceptionHandler: непредвиденная ошибка любого обработчика (БД недоступна,
// ошибка в SQL) - 500 INTERNAL_ERROR. Подробности только в лог: наружу не уходят (#17094 п. 4.3)
void exceptionHandler(const std::exception &e, const drogon::HttpRequestPtr &req,
                      std::function<void(const drogon::HttpResponsePtr &)> &&callback);

// Ошибка самого фреймворка (нет такого пути, неверный метод) - в формате Error, а не HTML-страница
// Drogon с его версией (#17094 п. 4.3). Для app().setCustomErrorHandler.
drogon::HttpResponsePtr frameworkError(drogon::HttpStatusCode status);
