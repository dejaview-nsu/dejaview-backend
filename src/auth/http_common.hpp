#pragma once

#include "auth/validation.hpp"

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <json/value.h>

#include <string>

// Общее для обработчиков auth: разбор тела запроса и типовые ответы.

// Тело запроса, если это JSON-объект, иначе nullptr. Указатель на константу: у неконстантного
// Json::Value оператор [] молча добавляет отсутствующее поле.
const Json::Value *jsonBody(const drogon::HttpRequestPtr &req);

// Строковое поле тела. Нет поля или не строка - пустая строка: валидация ответит «Введите ...».
std::string stringField(const Json::Value &body, const char *name);

// 400: тело не JSON-объект (тег Auth, раздел «Ошибки»).
drogon::HttpResponsePtr malformedBody();

// 400 с ошибкой поля из validation.hpp.
drogon::HttpResponsePtr fieldErrorResponse(const FieldError &error);

// Адрес клиента. За nginx адрес соединения - это адрес самого nginx, поэтому настоящий берём из
// X-Real-IP: nginx перезаписывает этот заголовок, клиент подделать его не может. Без nginx
// (локально) заголовка нет - берём адрес соединения.
std::string clientIp(const drogon::HttpRequestPtr &req);
