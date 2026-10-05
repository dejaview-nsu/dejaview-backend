#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <json/value.h>

#include <cstdint>
#include <optional>
#include <string>

// Запись в журнал безопасности security_events (#17094 п. 5.1): login_success, login_failure,
// logout, password_change, access_denied. В details - способ входа, причина отказа, путь.
// Ошибка записи не ломает запрос пользователя: она уходит в лог приложения.
// db - основной клиент, а не транзакция: событие остаётся, даже если действие откатилось.
drogon::Task<> logSecurityEvent(drogon::orm::DbClientPtr db, std::string eventType,
                                std::optional<std::int64_t> userId, drogon::HttpRequestPtr req,
                                Json::Value details);
