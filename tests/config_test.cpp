#include "config.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <string>

// loadConfig читает переменные окружения процесса, поэтому каждый тест начинает с известного
// состояния: корректный POSTGRES_URL и никакого PORT.
class LoadConfigTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        unsetenv("PORT");
        setenv("POSTGRES_URL", "postgres://user:secret@db:5432/dejaview", 1);
        setenv("APP_URL", "https://dejaview.ru", 1);
        unsetenv("SMARTCAPTCHA_SERVER_KEY");
        setenv("SMTP_URL", "smtp://mailpit:1025", 1);
        setenv("SMTP_FROM", "noreply@dejaview.ru", 1);
        unsetenv("SMTP_USER");
        unsetenv("SMTP_PASSWORD");
        for (const char *name :
             {"API_URL", "OIDC_YANDEX_CLIENT_ID", "OIDC_YANDEX_CLIENT_SECRET",
              "OIDC_GOOGLE_CLIENT_ID", "OIDC_GOOGLE_CLIENT_SECRET", "OIDC_VK_CLIENT_ID", "ML_URL",
              "ML_MAX_CONCURRENT", "ML_IMAGE_TIMEOUT_MS", "ML_VIDEO_TIMEOUT_MS",
              "ML_FAILURES_TO_OPEN", "ML_OPEN_FOR_MS"})
        {
            unsetenv(name);
        }
    }

    void TearDown() override
    {
        unsetenv("PORT");
        unsetenv("POSTGRES_URL");
        unsetenv("APP_URL");
        unsetenv("SMARTCAPTCHA_SERVER_KEY");
        for (const char *name : {"SMTP_URL", "SMTP_FROM", "SMTP_USER", "SMTP_PASSWORD", "ML_URL",
                                 "ML_MAX_CONCURRENT", "ML_IMAGE_TIMEOUT_MS", "ML_VIDEO_TIMEOUT_MS",
                                 "ML_FAILURES_TO_OPEN", "ML_OPEN_FOR_MS"})
        {
            unsetenv(name);
        }
    }
};

TEST_F(LoadConfigTest, UsesDefaultPort)
{
    const Config config = loadConfig();
    EXPECT_EQ(config.port, 8081);
    EXPECT_EQ(config.databaseUrl, "postgres://user:secret@db:5432/dejaview");
}

TEST_F(LoadConfigTest, ReadsPort)
{
    setenv("PORT", "9090", 1);
    EXPECT_EQ(loadConfig().port, 9090);
}

TEST_F(LoadConfigTest, RejectsInvalidPort)
{
    for (const char *port : {"abc", "0", "70000", "8081x", "", "-1", " 8081"})
    {
        SCOPED_TRACE(port);  // при падении покажет, на каком значении
        setenv("PORT", port, 1);
        try
        {
            loadConfig();
            ADD_FAILURE() << "ожидалось исключение";
        }
        catch (const std::runtime_error &error)
        {
            EXPECT_NE(std::string(error.what()).find("'" + std::string(port) + "'"),
                      std::string::npos)
                << error.what();
        }
    }
}

TEST_F(LoadConfigTest, AcceptsPostgresqlScheme)
{
    setenv("POSTGRES_URL", "postgresql://user:secret@db/dejaview", 1);
    EXPECT_NO_THROW(loadConfig());
}

TEST_F(LoadConfigTest, RequiresDatabaseUrl)
{
    unsetenv("POSTGRES_URL");
    EXPECT_THROW(loadConfig(), std::runtime_error);
}

TEST_F(LoadConfigTest, ErrorDoesNotContainPassword)
{
    setenv("POSTGRES_URL", "mysql://user:secret@db/dejaview", 1);
    try
    {
        loadConfig();
        FAIL() << "ожидалось исключение";
    }
    catch (const std::runtime_error &e)
    {
        EXPECT_EQ(std::string(e.what()).find("secret"), std::string::npos) << e.what();
    }
}

TEST_F(LoadConfigTest, RequiresAppUrl)
{
    for (const char *url : {"", "dejaview.ru", "ftp://dejaview.ru"})
    {
        SCOPED_TRACE(url);
        setenv("APP_URL", url, 1);
        EXPECT_THROW(loadConfig(), std::runtime_error);
    }
    unsetenv("APP_URL");
    EXPECT_THROW(loadConfig(), std::runtime_error);
}

TEST_F(LoadConfigTest, StripsTrailingSlashFromAppUrl)
{
    // иначе ссылка в письме получится https://dejaview.ru//confirm-email
    setenv("APP_URL", "http://localhost:57437/", 1);
    EXPECT_EQ(loadConfig().appUrl, "http://localhost:57437");
}

TEST_F(LoadConfigTest, CaptchaKeyIsOptional)
{
    EXPECT_EQ(loadConfig().smartCaptchaServerKey, "");
    setenv("SMARTCAPTCHA_SERVER_KEY", "ysc2_secret", 1);
    EXPECT_EQ(loadConfig().smartCaptchaServerKey, "ysc2_secret");
}

TEST_F(LoadConfigTest, ReadsSmtp)
{
    setenv("SMTP_URL", "smtps://smtp.yandex.ru:465", 1);
    setenv("SMTP_USER", "noreply@dejaview.ru", 1);
    setenv("SMTP_PASSWORD", "app-password", 1);
    const SmtpConfig smtp = loadConfig().smtp;
    EXPECT_EQ(smtp.url, "smtps://smtp.yandex.ru:465");
    EXPECT_EQ(smtp.from, "noreply@dejaview.ru");
    EXPECT_EQ(smtp.user, "noreply@dejaview.ru");
    EXPECT_EQ(smtp.password, "app-password");
}

TEST_F(LoadConfigTest, SmtpCredentialsAreOptional)
{
    const SmtpConfig smtp = loadConfig().smtp;
    EXPECT_EQ(smtp.user, "");
    EXPECT_EQ(smtp.password, "");
}

TEST_F(LoadConfigTest, RejectsInvalidSmtp)
{
    for (const char *url : {"", "http://mailpit:1025", "mailpit:1025"})
    {
        SCOPED_TRACE(url);
        setenv("SMTP_URL", url, 1);
        EXPECT_THROW(loadConfig(), std::runtime_error);
    }
    setenv("SMTP_URL", "smtp://mailpit:1025", 1);
    // перевод строки в адресе отправителя - внедрение заголовков письма
    for (const char *from : {"", "noreply", "noreply@dejaview.ru\r\nBcc: victim@example.com",
                             "DejaView <noreply@dejaview.ru>"})
    {
        SCOPED_TRACE(from);
        setenv("SMTP_FROM", from, 1);
        EXPECT_THROW(loadConfig(), std::runtime_error);
    }
}

TEST_F(LoadConfigTest, ApiUrlDefaultsToAppUrl)
{
    // в продакшене frontend и API на одном домене
    EXPECT_EQ(loadConfig().apiUrl, "https://dejaview.ru");
    setenv("API_URL", "http://localhost:8081/", 1);
    EXPECT_EQ(loadConfig().apiUrl, "http://localhost:8081");
    setenv("API_URL", "localhost:8081", 1);
    EXPECT_THROW(loadConfig(), std::runtime_error);
    unsetenv("API_URL");
}

TEST_F(LoadConfigTest, OidcProviderIsOptional)
{
    EXPECT_EQ(loadConfig().yandex.clientId, "");
    setenv("OIDC_YANDEX_CLIENT_ID", "id", 1);
    setenv("OIDC_YANDEX_CLIENT_SECRET", "secret", 1);
    const Config config = loadConfig();
    EXPECT_EQ(config.yandex.clientId, "id");
    EXPECT_EQ(config.yandex.clientSecret, "secret");
    EXPECT_EQ(config.google.clientId, "");  // каждый провайдер включается отдельно
    unsetenv("OIDC_YANDEX_CLIENT_ID");
    unsetenv("OIDC_YANDEX_CLIENT_SECRET");
}

TEST_F(LoadConfigTest, UsesContractDefaultsForMl)
{
    const MlConfig ml = loadConfig().ml;
    EXPECT_EQ(ml.url, "http://ml:8000");
    EXPECT_EQ(ml.maxConcurrent, 4u);
    EXPECT_EQ(ml.imageTimeout, std::chrono::milliseconds(9'000));
    EXPECT_EQ(ml.videoTimeout, std::chrono::milliseconds(14'000));
    EXPECT_EQ(ml.failuresToOpen, 5);
    EXPECT_EQ(ml.openFor, std::chrono::milliseconds(30'000));
}

TEST_F(LoadConfigTest, StripsTrailingSlashFromMlUrl)
{
    setenv("ML_URL", "http://ml:9000/", 1);
    EXPECT_EQ(loadConfig().ml.url, "http://ml:9000");
}

TEST_F(LoadConfigTest, RejectsMlUrlWithoutHttpScheme)
{
    setenv("ML_URL", "ml:8000", 1);
    EXPECT_THROW(loadConfig(), std::runtime_error);
}

TEST_F(LoadConfigTest, ReadsMlMaxConcurrent)
{
    for (const auto &[text, expected] : {std::pair<const char *, std::size_t>{"1", 1},
                                         std::pair<const char *, std::size_t>{"16", 16}})
    {
        SCOPED_TRACE(text);
        setenv("ML_MAX_CONCURRENT", text, 1);
        EXPECT_EQ(loadConfig().ml.maxConcurrent, expected);
    }
}

TEST_F(LoadConfigTest, RejectsInvalidMlMaxConcurrent)
{
    for (const char *value : {"0", "", "abc", "-1", " 4", "4x"})
    {
        SCOPED_TRACE(value);
        setenv("ML_MAX_CONCURRENT", value, 1);
        try
        {
            loadConfig();
            ADD_FAILURE() << "ожидалось исключение";
        }
        catch (const std::runtime_error &error)
        {
            EXPECT_NE(std::string(error.what()).find("ML_MAX_CONCURRENT"), std::string::npos);
        }
    }
}

TEST_F(LoadConfigTest, RejectsMlMaxConcurrentAboveIntRange)
{
    for (const char *value : {"2147483648", "4294967296", "99999999999999999999"})
    {
        SCOPED_TRACE(value);
        setenv("ML_MAX_CONCURRENT", value, 1);
        try
        {
            loadConfig();
            ADD_FAILURE() << "ожидалось исключение";
        }
        catch (const std::runtime_error &error)
        {
            EXPECT_NE(std::string(error.what()).find("ML_MAX_CONCURRENT"), std::string::npos);
        }
    }
}

namespace
{

// Четыре новых настройки ML: одинаковые правила, различается переменная и поле
constexpr const char *kMlNumberNames[] = {"ML_IMAGE_TIMEOUT_MS", "ML_VIDEO_TIMEOUT_MS",
                                          "ML_FAILURES_TO_OPEN", "ML_OPEN_FOR_MS"};

std::string loadConfigError()
{
    try
    {
        loadConfig();
    }
    catch (const std::runtime_error &error)
    {
        return error.what();
    }
    ADD_FAILURE() << "ожидалось исключение";
    return {};
}

}  // namespace

TEST_F(LoadConfigTest, ReadsMlImageTimeoutInMilliseconds)
{
    setenv("ML_IMAGE_TIMEOUT_MS", "2500", 1);
    EXPECT_EQ(loadConfig().ml.imageTimeout, std::chrono::milliseconds(2'500));
}

TEST_F(LoadConfigTest, ReadsMlVideoTimeoutInMilliseconds)
{
    setenv("ML_VIDEO_TIMEOUT_MS", "7000", 1);
    EXPECT_EQ(loadConfig().ml.videoTimeout, std::chrono::milliseconds(7'000));
}

TEST_F(LoadConfigTest, ReadsMlFailuresToOpenAsCount)
{
    setenv("ML_FAILURES_TO_OPEN", "3", 1);
    EXPECT_EQ(loadConfig().ml.failuresToOpen, 3);
}

TEST_F(LoadConfigTest, ReadsMlOpenForInMilliseconds)
{
    setenv("ML_OPEN_FOR_MS", "1500", 1);
    EXPECT_EQ(loadConfig().ml.openFor, std::chrono::milliseconds(1'500));
}

TEST_F(LoadConfigTest, MlNumbersAreIndependentOfEachOther)
{
    setenv("ML_IMAGE_TIMEOUT_MS", "1", 1);
    const MlConfig ml = loadConfig().ml;
    EXPECT_EQ(ml.imageTimeout, std::chrono::milliseconds(1));
    EXPECT_EQ(ml.videoTimeout, std::chrono::milliseconds(14'000));
    EXPECT_EQ(ml.failuresToOpen, 5);
    EXPECT_EQ(ml.openFor, std::chrono::milliseconds(30'000));
    EXPECT_EQ(ml.maxConcurrent, 4u);
}

TEST_F(LoadConfigTest, AcceptsMlNumbersAtBothBoundaries)
{
    for (const char *name : kMlNumberNames)
    {
        for (const char *value : {"1", "2147483647"})
        {
            SCOPED_TRACE(std::string(name) + "=" + value);
            setenv(name, value, 1);
            EXPECT_NO_THROW(loadConfig());
        }
        unsetenv(name);
    }
}

TEST_F(LoadConfigTest, RejectsInvalidMlNumbersNamingVariableAndValue)
{
    for (const char *name : kMlNumberNames)
    {
        for (const char *value :
             {"0", "-1", "abc", "12x", "", " 5", "2147483648", "99999999999999999999"})
        {
            SCOPED_TRACE(std::string(name) + "=" + value);
            setenv(name, value, 1);
            const std::string message = loadConfigError();
            EXPECT_NE(message.find(name), std::string::npos) << message;
            EXPECT_NE(message.find("'" + std::string(value) + "'"), std::string::npos) << message;
        }
        unsetenv(name);
    }
}

TEST_F(LoadConfigTest, MlNumberErrorUsesTheSameFormatAsMaxConcurrent)
{
    setenv("ML_OPEN_FOR_MS", "0", 1);
    EXPECT_EQ(loadConfigError(),
              "ML_OPEN_FOR_MS: ожидается целое число от 1 до 2147483647, получено '0'");
}
