#include "auth/crypto.hpp"

#include <sodium.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>

namespace
{
[[maybe_unused]] const bool sodiumReady = []
{
    if (sodium_init() < 0)
    {
        std::abort();
    }
    return true;
}();

// base64url без '=' - алфавит, безопасный для URL и cookie
std::string base64Url(const unsigned char *bytes, std::size_t size)
{
    constexpr int variant = sodium_base64_VARIANT_URLSAFE_NO_PADDING;
    std::string result(sodium_base64_ENCODED_LEN(size, variant), '\0');
    sodium_bin2base64(result.data(), result.size(), bytes, size, variant);
    result.pop_back();  // sodium_bin2base64 дописывает '\0'
    return result;
}
}  // namespace

std::string hashPassword(std::string_view password)
{
    std::array<char, crypto_pwhash_STRBYTES> hash{};
    if (crypto_pwhash_str_alg(
            hash.data(), password.data(), password.size(), crypto_pwhash_OPSLIMIT_INTERACTIVE,
            crypto_pwhash_MEMLIMIT_INTERACTIVE, crypto_pwhash_ALG_ARGON2ID13) != 0)
    {
        throw std::runtime_error("Argon2id: не хватило памяти");
    }
    return hash.data();
}

bool verifyPassword(std::string_view password, const std::string &hash)
{
    return crypto_pwhash_str_verify(hash.c_str(), password.data(), password.size()) == 0;
}

std::string newToken()
{
    std::array<unsigned char, 32> bytes{};
    randombytes_buf(bytes.data(), bytes.size());
    return base64Url(bytes.data(), bytes.size());
}

std::string tokenHash(std::string_view token)
{
    std::array<unsigned char, crypto_hash_sha256_BYTES> hash{};
    crypto_hash_sha256(hash.data(), reinterpret_cast<const unsigned char *>(token.data()),
                       token.size());

    std::string hex(hash.size() * 2 + 1, '\0');
    sodium_bin2hex(hex.data(), hex.size(), hash.data(), hash.size());
    hex.pop_back();
    return hex;
}

std::string pkceChallenge(std::string_view codeVerifier)
{
    std::array<unsigned char, crypto_hash_sha256_BYTES> hash{};
    crypto_hash_sha256(hash.data(), reinterpret_cast<const unsigned char *>(codeVerifier.data()),
                       codeVerifier.size());
    return base64Url(hash.data(), hash.size());
}

int randomBelow(int upper)
{
    return static_cast<int>(randombytes_uniform(static_cast<std::uint32_t>(upper)));
}
