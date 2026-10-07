#pragma once

#include "search/errors.hpp"

#include <json/value.h>

#include <cstdint>
#include <expected>
#include <vector>

// Разбор ответа ML-сервиса на поиск: id фильмов или ошибка поиска

// outage: учитывается автоматом защиты (нет соединения, таймаут, 5xx). 4xx и неверно
// сформированные ответы - не авария: сервис ответил.
struct MlFailure
{
    SearchError error;
    bool outage;
};

// body - разобранный JSON ответа, nullptr если ответ не JSON. Порядок id сохраняется (по
// релевантности).
[[nodiscard]] std::expected<std::vector<std::int64_t>, MlFailure> parseMlReply(
    MediaKind kind, int status, const Json::Value *body);
