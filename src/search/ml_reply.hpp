#pragma once

#include "search/errors.hpp"

#include <json/value.h>

#include <cstdint>
#include <expected>
#include <vector>

// Разбор ответа ML-сервиса на поиск: id фильмов или ошибка поиска

// outage: ответ учитывается автоматом защиты. 503 SERVER_BUSY подтверждает, что сервис
// отвечает и ограничивает нагрузку.
struct MlFailure
{
    SearchError error;
    bool outage;
};

// body - разобранный JSON ответа, nullptr если ответ не JSON. Порядок id сохраняется (по
// релевантности).
[[nodiscard]] std::expected<std::vector<std::int64_t>, MlFailure> parseMlReply(
    MediaKind kind, int status, const Json::Value *body);
