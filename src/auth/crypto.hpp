#pragma once

#include <string>
#include <string_view>

// Хеш пароля Argon2id (#17094 п. 2.2) в формате PHC: $argon2id$v=19$m=65536,t=2,p=1$соль$хеш.
std::string hashPassword(std::string_view password);

bool verifyPassword(std::string_view password, const std::string &hash);

// Случайный токен для cookie и ссылок из писем: 32 байта (256 бит) в base64url, 43 символа.
std::string newToken();

// SHA-256 токена в hex. В БД хранится только хеш (wiki PostgreSQL): копия таблицы не даёт
// войти. В SQL - decode($1, 'hex').
std::string tokenHash(std::string_view token);

// PKCE (RFC 7636): code_challenge = base64url(SHA-256(code_verifier)) без '='. К провайдеру уходит
// хеш, а сам verifier - только при обмене кода на токен: перехваченный код без него бесполезен.
std::string pkceChallenge(std::string_view codeVerifier);

// Случайное число от 0 до upper - 1, равномерно (без перекоса, как у rand() % upper).
int randomBelow(int upper);
