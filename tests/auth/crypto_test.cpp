#include "auth/crypto.hpp"

#include <gtest/gtest.h>

TEST(CryptoTest, PasswordHashIsArgon2idAndVerifies)
{
    const std::string hash = hashPassword("Kino#2026");
    EXPECT_TRUE(hash.starts_with("$argon2id$")) << hash;  // этого требует CHECK в users
    EXPECT_TRUE(verifyPassword("Kino#2026", hash));
    EXPECT_FALSE(verifyPassword("kino#2026", hash));
    EXPECT_FALSE(verifyPassword("", hash));
}

TEST(CryptoTest, SamePasswordGivesDifferentHashes)
{
    // случайная соль: по хешу нельзя понять, что у двух пользователей одинаковый пароль
    EXPECT_NE(hashPassword("Kino#2026"), hashPassword("Kino#2026"));
}

TEST(CryptoTest, TokensAreRandomBase64Url)
{
    const std::string token = newToken();
    EXPECT_EQ(token.size(),
              43U);  // из 32 байт получается 256 бит. Base64 кодирует 6 бит одним символом, 256 / 6
                     // ≈ 42.67, округляем вверх и получаем 43 символа =))))
    EXPECT_EQ(
        token.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"),
        std::string::npos)
        << token;
    EXPECT_NE(newToken(), token);
}

TEST(CryptoTest, TokenHashIsSha256Hex)
{
    // известное значение SHA-256 строки "abc"
    EXPECT_EQ(tokenHash("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}
