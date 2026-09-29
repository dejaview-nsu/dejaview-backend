#include "config.hpp"
#include "health.hpp"

#include <drogon/drogon.h>

#include <exception>

using namespace drogon;

int main()
{
    Config config;
    try
    {
        config = loadConfig();
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "Ошибка конфигурации: " << e.what();
        return 1;
    }

    // Один клиент с пулом соединений на всё приложение. Если БД недоступна, запросы падают с
    // ошибкой, а клиент сам переподключается.
    auto db = orm::DbClient::newPgClient(config.databaseUrl, 4);
    db->setTimeout(2.0);

    LOG_INFO << "dejaview-backend слушает порт " << config.port;

    app()
        .addListener("0.0.0.0", config.port)
        .setThreadNum(0)
        .enableServerHeader(false)
        .setUploadPath("/tmp/dejaview-uploads")
        .registerHandler("/health", [db](HttpRequestPtr) { return healthHandler(db); }, {Get})
        .run();
}
