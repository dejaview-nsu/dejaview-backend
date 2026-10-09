#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <gtest/gtest.h>
#include <json/value.h>

#include <cstdint>
#include <map>
#include <string>

// Интеграционные тесты: обработчики вызываются напрямую, с настоящим PostgreSQL. База -
// TEST_DATABASE_URL (сервис postgres-test в compose.yaml, в Dev Container переменная уже задана).
// Не задана - тесты пропускаются: модульные тесты и сборка образа работают без БД.
// Перед каждым тестом таблицы пустые, миграции из db/migrations применяются при первом запуске.
class DbTest : public ::testing::Test
{
  protected:
    void SetUp() override;

    drogon::orm::DbClientPtr db;

    // Дождаться корутины: обработчики возвращают Task<...>
    template <typename T>
    static T run(drogon::Task<T> task)
    {
        return drogon::sync_wait(std::move(task));
    }

    // POST/GET с JSON-телом и cookie: {"dv_session", "токен"}
    static drogon::HttpRequestPtr request(drogon::HttpMethod method, const std::string &path,
                                          const Json::Value &body = Json::Value(),
                                          const std::map<std::string, std::string> &cookies = {});

    // Первая ячейка результата строкой ("" - NULL или нет строк): для проверок состояния БД
    std::string scalar(const std::string &sql);

    // Токен из ссылки в последнем письме очереди: ...?token=<токен>
    std::string lastLinkToken();

    // Учётная запись с паролем kPassword, сразу в нужном статусе
    std::int64_t createUser(const std::string &username, const std::string &email,
                            const std::string &status = "active");

    static constexpr const char *kPassword = "Kino#2026";

    // Фейковый внешний сервис вместо SmartCaptcha и OIDC-провайдеров: HTTP-сервер на 127.0.0.1 в
    // этом же процессе, адрес - fakeUrl. На путь отвечает тем, что задал fakeReply (не задано -
    // 404), и запоминает последний пришедший запрос: тест проверяет, что отправил backend
    std::string fakeUrl;
    static void fakeReply(const std::string &path, drogon::HttpStatusCode status,
                          const std::string &json);
    static drogon::HttpRequestPtr fakeReceived(const std::string &path);  // nullptr - не было
};

// Тело ответа как JSON (пустой объект, если тела нет)
Json::Value body(const drogon::HttpResponsePtr &response);
