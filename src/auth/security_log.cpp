#include "auth/security_log.hpp"

#include <drogon/drogon.h>

using namespace drogon;

namespace
{
// Адрес клиента. За nginx peerAddr - адрес самого nginx, поэтому настоящий берём из X-Real-IP:
// nginx перезаписывает этот заголовок, клиент подделать его не может. Без nginx (локально)
// заголовка нет - берём адрес соединения.
std::string clientIp(const HttpRequestPtr &req)
{
    const std::string &realIp = req->getHeader("x-real-ip");
    return realIp.empty() ? req->peerAddr().toIp() : realIp;
}
}  // namespace

Task<> logSecurityEvent(orm::DbClientPtr db, std::string eventType,
                        std::optional<std::int64_t> userId, HttpRequestPtr req, Json::Value details)
{
    try
    {
        co_await db->execSqlCoro(
            "INSERT INTO security_events (event_type, user_id, ip, details) "
            "VALUES ($1, $2, NULLIF($3, '')::inet, $4::jsonb)",
            eventType, userId, clientIp(req), details.toStyledString());
    }
    catch (const orm::DrogonDbException &e)
    {
        LOG_ERROR << "security_events: " << eventType << ": " << e.base().what();
    }
}
