#include "email/outbox.hpp"

#include "email/message.hpp"

#include <curl/curl.h>
#include <drogon/drogon.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string_view>

using namespace drogon;

namespace
{
// libcurl читает текст письма кусками через этот вызов; userdata - ещё не отданный остаток
size_t readMessage(char *buffer, size_t size, size_t count, void *userdata)
{
    auto *rest = static_cast<std::string_view *>(userdata);
    const size_t length = std::min(size * count, rest->size());
    std::memcpy(buffer, rest->data(), length);
    rest->remove_prefix(length);
    return length;
}

// Одно письмо по SMTP. Возвращает текст ошибки для email_outbox.last_error, пустой - отправлено.
std::string sendEmail(const SmtpConfig &smtp, const std::string &to, const std::string &message)
{
    // unique_ptr с функцией очистки: curl освобождается на любом выходе из функции
    const std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(),
                                                                   curl_easy_cleanup);
    if (!curl)
    {
        return "curl_easy_init";
    }
    const std::string from = "<" + smtp.from + ">";
    const std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> recipients(
        curl_slist_append(nullptr, ("<" + to + ">").c_str()), curl_slist_free_all);
    std::string_view rest = message;
    char error[CURL_ERROR_SIZE] = "";

    curl_easy_setopt(curl.get(), CURLOPT_URL, smtp.url.c_str());
    if (!smtp.user.empty())
    {
        // отдельно от URL: пароль не нужно экранировать, и он не попадёт в лог вместе с адресом
        curl_easy_setopt(curl.get(), CURLOPT_USERNAME, smtp.user.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_PASSWORD, smtp.password.c_str());
    }
    // smtp:// - перейти на TLS, если сервер предлагает STARTTLS; smtps:// - TLS и так с начала
    curl_easy_setopt(curl.get(), CURLOPT_USE_SSL, static_cast<long>(CURLUSESSL_TRY));
    curl_easy_setopt(curl.get(), CURLOPT_MAIL_FROM, from.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_MAIL_RCPT, recipients.get());
    curl_easy_setopt(curl.get(), CURLOPT_READFUNCTION, readMessage);
    curl_easy_setopt(curl.get(), CURLOPT_READDATA, &rest);
    curl_easy_setopt(curl.get(), CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 30L);
    // без сигналов: в многопоточной программе таймаут через SIGALRM опасен
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_ERRORBUFFER, error);

    const CURLcode code = curl_easy_perform(curl.get());
    if (code == CURLE_OK)
    {
        return "";
    }
    return error[0] != '\0' ? error : curl_easy_strerror(code);
}
}  // namespace

void sendPendingEmails(const orm::DbClientPtr &db, const SmtpConfig &smtp)
{
    // Порция писем берётся «в аренду»: next_attempt_at сдвигается на 10 минут, и другие
    // экземпляры backend их не видят. SKIP LOCKED пропускает строки, которые прямо сейчас берёт
    // другой экземпляр. Транзакция на время отправки не держится: SMTP идёт до 30 с на письмо, а
    // соединений с БД в пуле всего 4. Упадёт процесс посреди порции - аренда истечёт, и письма
    // отправит кто-то другой (не меньше одного раза). 10 писем по 30с укладываются в 10 минут.
    const auto batch = db->execSqlSync(
        "UPDATE email_outbox o SET next_attempt_at = now() + interval '10 minutes' "
        "FROM users u "
        "WHERE u.user_id = o.user_id AND o.email_id IN ("
        "SELECT email_id FROM email_outbox WHERE status = 'pending' AND next_attempt_at <= now() "
        "ORDER BY next_attempt_at LIMIT 10 FOR UPDATE SKIP LOCKED) "
        "RETURNING o.email_id, o.kind, o.attempts, o.payload->>'link' AS link, u.email, "
        "u.username");

    for (const auto &row : batch)
    {
        const auto emailId = row["email_id"].as<std::int64_t>();
        const auto kind = row["kind"].as<std::string>();
        // Адрес - из users в момент отправки (db-schema.md): сменил email - письмо уйдёт на новый
        const auto email =
            renderEmail(kind, row["email"].as<std::string>(), row["username"].as<std::string>(),
                        row["link"].isNull() ? "" : row["link"].as<std::string>());
        const std::string error =
            email ? sendEmail(smtp, email->to,
                              formatMessage(*email, smtp.from, std::chrono::system_clock::now()))
                  : "нет шаблона письма для " + kind;

        if (error.empty())
        {
            // В payload ссылка с токеном в открытом виде: отправленное письмо незачем хранить
            db->execSqlSync("DELETE FROM email_outbox WHERE email_id = $1", emailId);
            LOG_INFO << "письмо " << emailId << " (" << kind << ") отправлено";
            continue;
        }

        // Без шаблона повторять бессмысленно, иначе - пауза, после kMaxEmailAttempts - failed.
        // В лог - номер письма, но не адрес: адрес - персональные данные (#17094 п. 2.3)
        const int attempts = row["attempts"].as<int>() + 1;
        const bool giveUp = !email || attempts >= kMaxEmailAttempts;
        LOG_WARN << "письмо " << emailId << ": попытка " << attempts << " не удалась"
                 << (giveUp ? ", больше не повторяем" : "") << ": " << error;
        db->execSqlSync(
            "UPDATE email_outbox SET attempts = $2::int, last_error = $3, "
            "status = CASE WHEN $4::boolean THEN 'failed' ELSE 'pending' END, "
            "next_attempt_at = now() + $5::int * interval '1 second' WHERE email_id = $1",
            emailId, attempts, error, giveUp, retryDelaySeconds(attempts));
    }
}
