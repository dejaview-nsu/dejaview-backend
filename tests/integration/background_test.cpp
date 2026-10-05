#include "background.hpp"
#include "email/outbox.hpp"
#include "integration/db_test.hpp"

class BackgroundTest : public DbTest
{
  protected:
    // SMTP-сервера на этом порту нет: каждая отправка - сбой соединения
    SmtpConfig deadSmtp{.url = "smtp://127.0.0.1:1", .from = "noreply@dejaview.ru"};

    void queueEmail(const std::string &kind = "email_confirm")
    {
        db->execSqlSync(
            "INSERT INTO email_outbox (user_id, kind, payload) "
            "SELECT user_id, $1, '{\"link\": \"https://x/confirm-email?token=t\"}' "
            "FROM users LIMIT 1",
            kind);
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

TEST_F(BackgroundTest, GivesUpAfterLastAttemptOrWithoutTemplate)
{
    createUser("ivan", "ivan@example.com");
    queueEmail();
    db->execSqlSync("UPDATE email_outbox SET attempts = 7");  // осталась последняя попытка
    queueEmail("password_reset");                             // шаблона пока нет (Sprint 2)
    sendPendingEmails(db, deadSmtp);

    EXPECT_EQ(
        scalar("SELECT string_agg(kind || ' ' || status, ', ' ORDER BY kind) FROM email_outbox"),
        "email_confirm failed, password_reset failed");
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
