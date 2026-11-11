#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

// Письма: шаблоны, формат для SMTP и политика повторов. Чистая логика без сети и БД.

struct Email
{
    std::string to;
    std::string subject;
    std::string body;  // текстовая версия, строки через \n
    std::string html;  // HTML-версия из src/email/templates; пустая - письмо только текстом
};

// Письмо по виду из email_outbox.kind: email_confirm (#17148 п. 2.5), password_reset (#17150
// п. 1.2), password_changed (#17150 п. 2.5, 3.3). Другой вид - nullopt.
std::optional<Email> renderEmail(std::string_view kind, std::string_view to,
                                 std::string_view username, std::string_view link);

// Письмо целиком в формате RFC 5322: заголовки, пустая строка, тело. Строки через CRLF - так
// требует SMTP. С HTML - две версии в одном письме (multipart/alternative, RFC 2046): клиент
// показывает лучшую, какую умеет. from - адрес отправителя, now - для заголовка Date.
std::string formatMessage(const Email &email, std::string_view from,
                          std::chrono::system_clock::time_point now);

// Текст для заголовка письма. Заголовки - только ASCII, поэтому кириллица кодируется по
// RFC 2047: =?UTF-8?B?<base64>?=, кусками не длиннее 75 символов через перенос строки с
// пробелом. ASCII-текст возвращается как есть.
std::string encodeHeader(std::string_view text);

// Текст сбоя отправки для email_outbox.last_error и лога: ошибка libcurl и, если сервер отказал
// (код 4xx или 5xx), его последний ответ - libcurl включает его в ошибку не всегда. replies -
// ответы сервера за сеанс, как их отдаёт CURLOPT_HEADERFUNCTION. Из ответа - только печатный
// ASCII (остальное - «?») и не больше 512 символов, адрес to в нём в любом регистре заменяется на
// «***»: адреса в лог не пишутся.
std::string sendError(std::string_view curlError, std::string_view replies, std::string_view to);

// Короткая ссылка из ответа сокращателя shortener (EMAIL_LINK_SHORTENER, #18974) без перевода
// строки в конце: на том же адресе, что и сокращатель (схема, хост и порт), не длиннее
// kMaxShortLinkLength, печатный ASCII без пробелов. Иначе - nullopt: ответ пришёл по сети и идёт
// в письмо, а это может быть страница с капчей или ссылка на чужой сайт.
constexpr std::size_t kMaxShortLinkLength = 256;
std::optional<std::string> shortLink(std::string_view reply, std::string_view shortener);

// Повторы при сбое SMTP (contracts/db-schema.md, email_outbox): после kMaxEmailAttempts
// неудач письмо помечается failed. Паузы 1, 2, 4, ... минут, не больше часа: всего около
// 3 часов попыток, ссылка в письме живёт 24 ч.
constexpr int kMaxEmailAttempts = 8;
int retryDelaySeconds(int failedAttempts);
