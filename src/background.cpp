#include "background.hpp"

#include "email/outbox.hpp"

#include <curl/curl.h>
#include <drogon/drogon.h>

#include <chrono>
#include <condition_variable>
#include <mutex>

using namespace drogon;
using namespace std::chrono_literals;

// Одним запросом (contracts/db-schema.md): каждый DELETE в WITH выполняется, даже если результат
// не читают.
void deleteExpired(const orm::DbClientPtr &db)
{
    db->execSqlSync(
        "WITH s AS (DELETE FROM sessions WHERE expires_at < now()), "
        // Истёкшие ссылки держим ещё 7 дней - по ним работает «Отправить новую ссылку»
        // (resend-confirmation с token). Оставил пока что 7 дней, мб потом обсудим и поменяем
        "t AS (DELETE FROM auth_tokens WHERE expires_at < now() - interval '7 days'), "
        "p AS (DELETE FROM oidc_pending WHERE expires_at < now()), "
        // журнал безопасности хранится 30 дней (#17094 п. 5.2)
        "e AS (DELETE FROM security_events WHERE occurred_at < now() - interval '30 days'), "
        // неотправленные письма: ссылки в них за сутки истекли
        "m AS (DELETE FROM email_outbox WHERE status = 'failed' "
        "AND created_at < now() - interval '24 hours') "
        "SELECT 1");
}

std::jthread startBackgroundJobs(orm::DbClientPtr db, SmtpConfig smtp)
{
    // libcurl инициализируется один раз до запуска потоков, которые им пользуются
    curl_global_init(CURL_GLOBAL_DEFAULT);

    return std::jthread(
        [db, smtp](std::stop_token stop)
        {
            std::mutex mutex;
            std::condition_variable_any wakeUp;
            auto lastCleanup = std::chrono::steady_clock::time_point{};  // первая - сразу
            while (!stop.stop_requested())
            {
                // Ошибка (БД недоступна, сбой SQL) не останавливает поток: повтор на следующем
                // круге
                try
                {
                    sendPendingEmails(db, smtp);
                    if (std::chrono::steady_clock::now() - lastCleanup >= 1h)
                    {
                        deleteExpired(db);
                        lastCleanup = std::chrono::steady_clock::now();
                    }
                }
                catch (const std::exception &e)
                {
                    LOG_ERROR << "фоновые задачи: " << e.what();
                }
                // Пауза, которую прерывает остановка: не ждать 5 с при выключении сервера
                std::unique_lock lock(mutex);
                wakeUp.wait_for(lock, stop, 5s, [] { return false; });
            }
        });
}
