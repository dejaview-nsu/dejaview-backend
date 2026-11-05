#include "background.hpp"
#include "email/outbox.hpp"
#include "integration/db_test.hpp"

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <thread>

namespace
{
// SMTP-сервер на 127.0.0.1 для одного сеанса: принимает отправителя, получателя и текст письма,
// а на конец текста отвечает rejection
class FakeSmtp
{
  public:
    explicit FakeSmtp(std::string rejection)
        : listener_(socket(AF_INET, SOCK_STREAM, 0)), rejection_(std::move(rejection))
    {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // порт 0 - любой свободный
        socklen_t size = sizeof address;
        bind(listener_, reinterpret_cast<sockaddr *>(&address), size);
        listen(listener_, 1);
        getsockname(listener_, reinterpret_cast<sockaddr *>(&address), &size);
        port_ = ntohs(address.sin_port);
        thread_ = std::jthread([this] { serve(); });
    }

    ~FakeSmtp()
    {
        shutdown(listener_, SHUT_RDWR);  // accept не ждёт вечно, если клиент так и не пришёл
        thread_.join();
        close(listener_);
    }

    FakeSmtp(const FakeSmtp &) = delete;
    FakeSmtp &operator=(const FakeSmtp &) = delete;

    std::string url() const { return "smtp://127.0.0.1:" + std::to_string(port_); }

  private:
    void serve() const
    {
        const int client = accept(listener_, nullptr, nullptr);
        if (client < 0)
        {
            return;
        }
        const auto reply = [client](std::string_view text)
        { send(client, text.data(), text.size(), MSG_NOSIGNAL); };

        reply("220 smtp.example.com ESMTP\r\n");
        for (std::string line; readLine(client, line) && !line.starts_with("QUIT");)
        {
            if (!line.starts_with("DATA"))
            {
                reply("250 OK\r\n");
                continue;
            }
            reply("354 Start mail input\r\n");
            while (readLine(client, line) && line != ".\r\n")
            {
            }
            reply(rejection_);
        }
        close(client);
    }

    // Строка от клиента вместе с CRLF; false - клиент закрыл соединение
    static bool readLine(int client, std::string &line)
    {
        line.clear();
        for (char c; recv(client, &c, 1, 0) == 1;)
        {
            line += c;
            if (c == '\n')
            {
                return true;
            }
        }
        return false;
    }

    int listener_;
    std::uint16_t port_ = 0;
    std::string rejection_;
    std::jthread thread_;
};
}  // namespace

class BackgroundTest : public DbTest
{
  protected:
    // SMTP-сервера на этом порту нет: каждая отправка - сбой соединения
    SmtpConfig deadSmtp{.url = "smtp://127.0.0.1:1", .from = "noreply@dejaview.ru"};

    void queueEmail()
    {
        db->execSqlSync(
            "INSERT INTO email_outbox (user_id, kind, payload) "
            "SELECT user_id, 'email_confirm', '{\"link\": \"https://x/confirm-email?token=t\"}' "
            "FROM users LIMIT 1");
    }
};

TEST_F(BackgroundTest, FailedSendIsRetriedLater)
{
    createUser("ivan", "ivan@example.com");
    queueEmail();
    sendPendingEmails(db, deadSmtp);

    EXPECT_EQ(scalar("SELECT status || ' ' || attempts FROM email_outbox"), "pending 1");
    EXPECT_NE(scalar("SELECT last_error FROM email_outbox"), "");
    // следующая попытка через минуту: сразу письмо не берётся снова
    EXPECT_EQ(scalar("SELECT next_attempt_at > now() + interval '50 seconds' FROM email_outbox"),
              "t");
    sendPendingEmails(db, deadSmtp);
    EXPECT_EQ(scalar("SELECT attempts FROM email_outbox"), "1");
}

TEST_F(BackgroundTest, GivesUpAfterLastAttempt)
{
    createUser("ivan", "ivan@example.com");
    queueEmail();
    db->execSqlSync("UPDATE email_outbox SET attempts = 7");  // осталась последняя попытка
    sendPendingEmails(db, deadSmtp);

    EXPECT_EQ(scalar("SELECT status || ' ' || attempts FROM email_outbox"), "failed 8");
}

TEST_F(BackgroundTest, RejectionReplyIsSavedWithoutAddress)
{
    createUser("ivan", "ivan@example.com");
    queueEmail();
    {
        FakeSmtp smtp("554 5.7.1 <ivan@example.com> Message rejected\r\n");
        sendPendingEmails(db, SmtpConfig{.url = smtp.url(), .from = "noreply@dejaview.ru"});
    }

    EXPECT_TRUE(scalar("SELECT last_error FROM email_outbox")
                    .ends_with(": 554 5.7.1 <***> Message rejected"))
        << scalar("SELECT last_error FROM email_outbox");
}

TEST_F(BackgroundTest, DeleteExpiredKeepsFreshRows)
{
    const auto userId = createUser("ivan", "ivan@example.com");
    const auto id = std::to_string(userId);
    db->execSqlSync(
        "INSERT INTO sessions (token_hash, user_id, created_at, expires_at) VALUES "
        "(sha256('old'), " +
        id +
        ", now() - interval '2 days', now() - interval '1 day'), "
        "(sha256('new'), " +
        id + ", now(), now() + interval '1 hour')");
    db->execSqlSync(
        "INSERT INTO security_events (occurred_at, event_type, user_id) VALUES "
        "(now() - interval '31 days', 'logout', " +
        id + "), (now() - interval '29 days', 'logout', " + id + ")");
    db->execSqlSync(
        "INSERT INTO auth_tokens (token_hash, user_id, purpose, created_at, expires_at) "
        "VALUES (sha256('old'), " +
        id +
        ", 'email_confirm', now() - interval '9 days', "
        "now() - interval '8 days')");
    db->execSqlSync("INSERT INTO email_outbox (user_id, kind, status, created_at) VALUES (" + id +
                    ", 'email_confirm', 'failed', now() - interval '25 hours'), (" + id +
                    ", 'email_confirm', 'failed', now() - interval '1 hour')");

    deleteExpired(db);

    EXPECT_EQ(scalar("SELECT count(*) FROM sessions"), "1");
    EXPECT_EQ(scalar("SELECT count(*) FROM security_events"), "1");
    EXPECT_EQ(scalar("SELECT count(*) FROM auth_tokens"), "0");
    EXPECT_EQ(scalar("SELECT count(*) FROM email_outbox"), "1");
}
