#include "config.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <stdexcept>
#include <string>

// loadConfig читает переменные окружения процесса, поэтому каждый тест начинает с известного
// состояния: корректный DATABASE_URL и никакого PORT.
class LoadConfigTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        unsetenv("PORT");
        setenv("DATABASE_URL", "postgres://user:secret@db:5432/dejaview", 1);
        setenv("APP_URL", "https://dejaview.ru", 1);
        unsetenv("SMARTCAPTCHA_SERVER_KEY");
    }

    void TearDown() override
    {
        unsetenv("PORT");
        unsetenv("DATABASE_URL");
        unsetenv("APP_URL");
        unsetenv("SMARTCAPTCHA_SERVER_KEY");
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
        EXPECT_THROW(loadConfig(), std::runtime_error);
    }
}

TEST_F(LoadConfigTest, AcceptsPostgresqlScheme)
{
    setenv("DATABASE_URL", "postgresql://user:secret@db/dejaview", 1);
    EXPECT_NO_THROW(loadConfig());
}

TEST_F(LoadConfigTest, RequiresDatabaseUrl)
{
    unsetenv("DATABASE_URL");
    EXPECT_THROW(loadConfig(), std::runtime_error);
}

TEST_F(LoadConfigTest, ErrorDoesNotContainPassword)
{
    setenv("DATABASE_URL", "mysql://user:secret@db/dejaview", 1);
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
