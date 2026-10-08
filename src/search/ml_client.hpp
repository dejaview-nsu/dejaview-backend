#pragma once

#include "config.hpp"
#include "search/circuit_breaker.hpp"
#include "search/errors.hpp"

#include <drogon/utils/coroutine.h>

#include <cstdint>
#include <expected>
#include <memory>
#include <semaphore>
#include <string>
#include <vector>

// Состояние вызовов ML одного экземпляра backend: лимит M и выключатель общие для всех
// запросов, поэтому живёт в shared_ptr - его делят обработчики маршрутов и корутины в полёте.
struct MlService
{
    explicit MlService(MlConfig config);

    MlConfig config;
    CircuitBreaker breaker;
    std::counting_semaphore<> slots;  // M; слот занят на всё обращение к ML
};

// Один поиск в ML: movie_id по релевантности или ошибка клиента (в том числе Busy и
// Unavailable). Сбои ML и сети не бросаются исключением.
drogon::Task<std::expected<std::vector<std::int64_t>, SearchError>> searchMl(
    std::shared_ptr<MlService> ml, MediaKind kind, std::string mime, std::string content,
    std::string requestId);
