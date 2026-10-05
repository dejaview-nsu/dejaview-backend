#include "error_response.hpp"

#include <gtest/gtest.h>

using namespace drogon;

TEST(FrameworkErrorTest, AnswersInContractFormatWithoutDetails)
{
    for (const auto status :
         {k404NotFound, k405MethodNotAllowed, k413RequestEntityTooLarge, k503ServiceUnavailable})
    {
        SCOPED_TRACE(status);
        const auto response = frameworkError(status);
        EXPECT_EQ(response->statusCode(), status);
        const auto &json = response->getJsonObject();
        ASSERT_TRUE(json);
        EXPECT_FALSE((*json)["code"].asString().empty());
        // ни версии Drogon, ни HTML - только текст для пользователя (#17094 п. 4.3)
        EXPECT_EQ(response->body().find("drogon"), std::string::npos);
    }
    EXPECT_EQ((*frameworkError(k404NotFound)->getJsonObject())["code"].asString(), "NOT_FOUND");
    EXPECT_EQ((*frameworkError(k503ServiceUnavailable)->getJsonObject())["code"].asString(),
              "INTERNAL_ERROR");
}
