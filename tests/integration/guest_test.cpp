#include "auth/login.hpp"
#include "auth/registration.hpp"
#include "auth/session.hpp"
#include "integration/db_test.hpp"

using namespace drogon;

// Операция гостя с недействительной cookie выполняется как для гостя, cookie стирается (тег Auth)
class GuestTest : public DbTest
{
  protected:
    // Как в приложении: advice до обработчика, обработчик, advice после (main.cpp)
    HttpResponsePtr handle(const HttpRequestPtr &req, Task<HttpResponsePtr> handler)
    {
        run(resolveSession(db, req));
        auto response = run(std::move(handler));
        clearStaleSessionCookie(req, response);
        return response;
    }

    HttpResponsePtr registerAs(const std::map<std::string, std::string> &cookies)
    {
        Json::Value json;
        json["username"] = "newcomer";
        json["email"] = "newcomer@example.com";
        json["birth_year"] = 2000;
        json["password"] = kPassword;
        const auto req = request(Post, "/api/v1/auth/register", json, cookies);
        return handle(req, registerHandler(db, "https://dejaview.ru", req));
    }

    HttpResponsePtr loginAs(const std::map<std::string, std::string> &cookies)
    {
        Json::Value json;
        json["login"] = "ivan";
        json["password"] = kPassword;
        const auto req = request(Post, "/api/v1/auth/login", json, cookies);
        return handle(req, loginHandler(db, "", req));
    }
};

TEST_F(GuestTest, StaleCookieIsCleared)
{
    const auto response = registerAs({{"dv_session", "stale"}});
    EXPECT_EQ(response->statusCode(), k201Created);  // операция выполнена как для гостя
    EXPECT_EQ(response->getCookie("dv_session").maxAge(), 0);
}

TEST_F(GuestTest, ValidOrMissingCookieIsLeftAlone)
{
    createUser("ivan", "ivan@example.com");
    const std::string token = loginAs({})->getCookie("dv_session").value();

    EXPECT_TRUE(registerAs({{"dv_session", token}})->getCookies().empty());
    db->execSqlSync("TRUNCATE users RESTART IDENTITY CASCADE");
    EXPECT_TRUE(registerAs({})->getCookies().empty());
}

TEST_F(GuestTest, LoginWithStaleCookieGetsNewSession)
{
    createUser("ivan", "ivan@example.com");
    const auto response = loginAs({{"dv_session", "stale"}});
    EXPECT_EQ(response->statusCode(), k200OK);
    EXPECT_FALSE(response->getCookie("dv_session").value().empty());  // не стёрта, а новая
}
