#pragma once

#include "config.hpp"

#include <drogon/orm/DbClient.h>

// Отправка писем из очереди email_outbox (паттерн transactional outbox): обработчики запросов
// только кладут письмо в таблицу вместе со своими изменениями, отправляет - фоновый поток.

// Отправляет одну порцию писем, которым пора. Блокирующая: SMTP-запрос ждёт до 30 с, поэтому
// вызывать только из фонового потока, не из обработчика запросов.
void sendPendingEmails(const drogon::orm::DbClientPtr &db, const SmtpConfig &smtp);
