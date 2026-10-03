#include "auth/session.hpp"

#include <gtest/gtest.h>

// Атрибуты cookie - часть контракта (SessionSetCookie, SessionClearCookie в api/openapi.yaml):
// без HttpOnly cookie прочитает скрипт, без SameSite её отправит чужой сайт.
TEST(SessionCookieTest, HasContractAttributes)
{
    const std::string cookie = sessionCookie("token123").cookieString();
    for (const char *part : {"dv_session=token123", "HttpOnly", "Secure", "SameSite=Lax",
                             "Path=/api/v1", "Max-Age=86400"})
    {
        EXPECT_NE(cookie.find(part), std::string::npos) << part << " нет в " << cookie;
    }
}

TEST(SessionCookieTest, ClearCookieExpiresImmediately)
{
    const std::string cookie = clearSessionCookie().cookieString();
    for (const char *part : {"dv_session=;", "Path=/api/v1", "Max-Age=0"})
    {
        EXPECT_NE(cookie.find(part), std::string::npos) << part << " нет в " << cookie;
    }
}
