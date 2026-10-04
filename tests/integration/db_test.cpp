#include "integration/db_test.hpp"

#include "auth/crypto.hpp"

#include <drogon/drogon.h>
#include <libpq-fe.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace drogon;

namespace
{
void exec(PGconn *conn, const std::string &sql)
{
    const std::unique_ptr<PGresult, decltype(&PQclear)> result(PQexec(conn, sql.c_str()), PQclear);
    if (const auto status = PQresultStatus(result.get());
        status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK)
    {
        throw std::runtime_error(PQerrorMessage(conn));
    }
}

// Применяет ещё не применённые миграции, как dbmate: версия - начало имени файла, учёт в
// schema_migrations, выполняется часть между «-- migrate:up» и «-- migrate:down». Через libpq:
// PQexec выполняет файл из многих команд целиком, Drogon так не умеет
void migrate(const std::string &url)
{
    const std::unique_ptr<PGconn, decltype(&PQfinish)> conn(PQconnectdb(url.c_str()), PQfinish);
    if (PQstatus(conn.get()) != CONNECTION_OK)
    {
        throw std::runtime_error("TEST_DATABASE_URL: " + std::string(PQerrorMessage(conn.get())));
    }
    exec(conn.get(),
         "SET client_min_messages = warning; "
         "CREATE TABLE IF NOT EXISTS schema_migrations (version text PRIMARY KEY)");

    std::vector<std::filesystem::path> files;
    for (const auto &entry : std::filesystem::directory_iterator(DEJAVIEW_MIGRATIONS_DIR))
    {
        files.push_back(entry.path());
    }
    std::ranges::sort(files);
    for (const auto &file : files)
    {
        // версия - 14 цифр даты из имени файла, кавычек в ней не бывает
        const std::string version = file.filename().string().substr(0, 14);
        const std::unique_ptr<PGresult, decltype(&PQclear)> applied(
            PQexec(conn.get(),
                   ("SELECT 1 FROM schema_migrations WHERE version = '" + version + "'").c_str()),
            PQclear);
        if (PQntuples(applied.get()) > 0)
        {
            continue;
        }
        std::stringstream text;
        text << std::ifstream(file).rdbuf();
        const std::string sql = text.str();
        const auto up = sql.find("-- migrate:up");
        const auto down = sql.find("-- migrate:down");
        exec(conn.get(), "BEGIN; " + sql.substr(up, down - up) +
                             "; INSERT INTO schema_migrations VALUES ('" + version + "'); COMMIT;");
    }
}

// Один клиент на процесс: ctest запускает каждый тест отдельным процессом
orm::DbClientPtr testDb()
{
    static const orm::DbClientPtr db = []
    {
        const char *url = std::getenv("TEST_DATABASE_URL");
        migrate(url);
        auto client = orm::DbClient::newPgClient(url, 1);
        client->setTimeout(5.0);
        return client;
    }();
    return db;
}

// Ответы фейкового сервиса, заданные тестом, и пришедшие запросы. Сервер работает в своём
// потоке, тест - в своём: отсюда мьютекс
std::mutex fakeMutex;
std::map<std::string, std::pair<HttpStatusCode, std::string>> fakeReplies;
std::map<std::string, HttpRequestPtr> fakeRequests;
std::thread fakeThread;

// Фейковый сервис - приложение Drogon в отдельном потоке, порт выбирает система. Запускается при
// первом вызове. Нужен и самим запросам: HttpClient работает в цикле событий приложения, без
// запуска обращение к внешнему сервису не завершилось бы никогда
const std::string &fakeServer()
{
    static const std::string url = []
    {
        std::promise<std::uint16_t> port;
        app()
            .addListener("127.0.0.1", 0)
            .registerBeginningAdvice([&port] { port.set_value(app().getListeners()[0].toPort()); })
            // advice до маршрутизации видит любой путь: обработчики регистрировать не нужно
            .registerPreRoutingAdvice(
                [](const HttpRequestPtr &req, AdviceCallback &&respond, AdviceChainCallback &&)
                {
                    const std::lock_guard lock(fakeMutex);
                    fakeRequests[req->path()] = req;
                    const auto reply = fakeReplies.find(req->path());
                    if (reply == fakeReplies.end())
                    {
                        respond(HttpResponse::newHttpResponse(k404NotFound, CT_APPLICATION_JSON));
                        return;
                    }
                    auto response =
                        HttpResponse::newHttpResponse(reply->second.first, CT_APPLICATION_JSON);
                    response->setBody(reply->second.second);
                    respond(response);
                });
        fakeThread = std::thread([] { app().run(); });
        return "http://127.0.0.1:" + std::to_string(port.get_future().get());
    }();
    return url;
}

// После всех тестов сервер останавливается: иначе процесс завершался бы при работающем потоке
class StopFakeServer : public testing::Environment
{
    void TearDown() override
    {
        if (fakeThread.joinable())
        {
            app().quit();
            fakeThread.join();
        }
    }
};
const auto *const kStopFakeServer = testing::AddGlobalTestEnvironment(new StopFakeServer);
}  // namespace

void DbTest::SetUp()
{
    if (!std::getenv("TEST_DATABASE_URL"))
    {
        GTEST_SKIP() << "TEST_DATABASE_URL не задан: интеграционные тесты пропущены";
    }
    db = testDb();
    // Каскад очищает и всё, что ссылается на эти таблицы: сессии, ссылки, письма, привязки
    db->execSqlSync(
        "TRUNCATE users, oidc_pending, security_events, movies RESTART IDENTITY CASCADE");

    fakeUrl = fakeServer();
    const std::lock_guard lock(fakeMutex);
    fakeReplies.clear();
    fakeRequests.clear();
}

void DbTest::fakeReply(const std::string &path, HttpStatusCode status, const std::string &json)
{
    const std::lock_guard lock(fakeMutex);
    fakeReplies[path] = {status, json};
}

HttpRequestPtr DbTest::fakeReceived(const std::string &path)
{
    const std::lock_guard lock(fakeMutex);
    const auto request = fakeRequests.find(path);
    return request == fakeRequests.end() ? nullptr : request->second;
}

HttpRequestPtr DbTest::request(HttpMethod method, const std::string &path, const Json::Value &body,
                               const std::map<std::string, std::string> &cookies)
{
    auto request =
        body.isNull() ? HttpRequest::newHttpRequest() : HttpRequest::newHttpJsonRequest(body);
    request->setMethod(method);
    request->setPath(path);
    for (const auto &[name, value] : cookies)
    {
        request->addCookie(name, value);
    }
    return request;
}

std::string DbTest::scalar(const std::string &sql)
{
    const auto result = db->execSqlSync(sql);
    return result.empty() || result[0][0].isNull() ? "" : result[0][0].as<std::string>();
}

std::int64_t DbTest::createUser(const std::string &username, const std::string &email,
                                const std::string &status)
{
    return db
        ->execSqlSync(
            "INSERT INTO users (username, email, birth_year, password_hash, status) "
            "VALUES ($1, $2, 2000, $3, $4) RETURNING user_id",
            username, email, hashPassword(kPassword), status)[0]["user_id"]
        .as<std::int64_t>();
}

Json::Value body(const HttpResponsePtr &response)
{
    const auto &json = response->getJsonObject();
    return json ? *json : Json::Value(Json::objectValue);
}
