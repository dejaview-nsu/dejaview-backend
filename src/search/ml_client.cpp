#include "search/ml_client.hpp"

#include "search/ml_reply.hpp"

#include <drogon/drogon.h>

#include <algorithm>
#include <chrono>

using namespace drogon;

namespace
{

using Clock = CircuitBreaker::Clock;
using SearchResult = std::expected<std::vector<std::int64_t>, SearchError>;

constexpr std::chrono::seconds kHealthTimeout{1};
constexpr std::size_t kLoggedBodyBytes = 512;

// Тайм-аут Drogon 0 означает "без тайм-аута", поэтому остаток передаётся дробными секундами
// и только положительным
double seconds(Clock::duration duration) { return std::chrono::duration<double>(duration).count(); }

// Возвращает слот при любом выходе из корутины
class SlotGuard
{
  public:
    explicit SlotGuard(std::counting_semaphore<> &slots) : slots_(slots) {}
    ~SlotGuard() { slots_.release(); }
    SlotGuard(const SlotGuard &) = delete;
    SlotGuard &operator=(const SlotGuard &) = delete;

  private:
    std::counting_semaphore<> &slots_;
};

// Выключатель ждёт onProbe после каждого Probe: без отчёта выход из корутины считается
// неудачной пробой, иначе выключатель навсегда остался бы с идущей пробой
class ProbeGuard
{
  public:
    explicit ProbeGuard(CircuitBreaker &breaker) : breaker_(breaker) {}
    ~ProbeGuard()
    {
        if (!reported_)
        {
            breaker_.onProbe(false, Clock::now());
        }
    }
    ProbeGuard(const ProbeGuard &) = delete;
    ProbeGuard &operator=(const ProbeGuard &) = delete;

    void report(bool healthy)
    {
        reported_ = true;
        breaker_.onProbe(healthy, Clock::now());
    }

  private:
    CircuitBreaker &breaker_;
    bool reported_ = false;
};

HttpRequestPtr newRequest(HttpMethod method, const std::string &path, const std::string &requestId)
{
    auto request = HttpRequest::newHttpRequest();
    request->setMethod(method);
    request->setPath(path);
    request->addHeader("X-Request-Id", requestId);
    return request;
}

std::string searchPath(MediaKind kind)
{
    return kind == MediaKind::Image ? "/v1/search/image" : "/v1/search/video";
}

Task<bool> isHealthy(std::string url, std::string requestId, double timeoutSeconds)
{
    auto client = HttpClient::newHttpClient(url);
    try
    {
        const auto response =
            co_await client->sendRequestCoro(newRequest(Get, "/health", requestId), timeoutSeconds);
        co_return response->statusCode() == k200OK;
    }
    catch (const std::exception &e)
    {
        LOG_WARN << "ML /health недоступен [X-Request-Id " << requestId << "]: " << e.what();
        co_return false;
    }
}

// POST в ML и учёт результата в выключателе: сервис ответил - серия аварий сброшена, нет
// соединения, тайм-аут или 5xx - авария
Task<SearchResult> postSearch(std::shared_ptr<MlService> ml, MediaKind kind, std::string mime,
                              std::string content, std::string requestId, double timeoutSeconds)
{
    auto client = HttpClient::newHttpClient(ml->config.url);
    auto request = newRequest(Post, searchPath(kind), requestId);
    request->setContentTypeString(mime);
    request->setBody(std::move(content));
    try
    {
        const auto response = co_await client->sendRequestCoro(request, timeoutSeconds);
        const int status = static_cast<int>(response->statusCode());
        auto parsed = parseMlReply(kind, status, response->getJsonObject().get());
        if (parsed)
        {
            ml->breaker.onReply();
            co_return *parsed;
        }
        LOG_WARN << "ML ответил " << status << " [X-Request-Id " << requestId
                 << "]: " << response->body().substr(0, kLoggedBodyBytes);
        if (parsed.error().outage)
        {
            ml->breaker.onOutage(Clock::now());
        }
        else
        {
            ml->breaker.onReply();
        }
        co_return std::unexpected(parsed.error().error);
    }
    catch (const std::exception &e)
    {
        LOG_WARN << "ML недоступен [X-Request-Id " << requestId << "]: " << e.what();
        ml->breaker.onOutage(Clock::now());
        co_return std::unexpected(SearchError::Unavailable);
    }
}

}  // namespace

MlService::MlService(MlConfig config)
    : config(std::move(config)),
      breaker(this->config.failuresToOpen, this->config.openFor),
      slots(static_cast<std::ptrdiff_t>(this->config.maxConcurrent))
{
}

Task<SearchResult> searchMl(std::shared_ptr<MlService> ml, MediaKind kind, std::string mime,
                            std::string content, std::string requestId)
{
    if (!ml->slots.try_acquire())
    {
        LOG_WARN << "Все слоты ML заняты [X-Request-Id " << requestId << "]";
        co_return std::unexpected(SearchError::Busy);
    }
    const SlotGuard slot(ml->slots);

    // Один срок на всё: проверка /health съедает его часть, на POST остаётся остаток
    const Clock::time_point deadline =
        Clock::now() +
        (kind == MediaKind::Image ? ml->config.imageTimeout : ml->config.videoTimeout);

    switch (ml->breaker.gate(Clock::now()))
    {
        case CircuitBreaker::Gate::Call:
            break;
        case CircuitBreaker::Gate::Reject:
            LOG_WARN << "Вызовы ML отключены после серии аварий [X-Request-Id " << requestId << "]";
            co_return std::unexpected(SearchError::Unavailable);
        case CircuitBreaker::Gate::Probe:
        {
            ProbeGuard probe(ml->breaker);
            const auto timeout = std::min<Clock::duration>(kHealthTimeout, deadline - Clock::now());
            if (timeout <= Clock::duration::zero())
            {
                // Drogon понимает тайм-аут 0 как "без тайм-аута"; ProbeGuard отметит пробу
                // неудачной
                co_return std::unexpected(SearchError::Unavailable);
            }
            const bool healthy = co_await isHealthy(ml->config.url, requestId, seconds(timeout));
            probe.report(healthy);
            LOG_WARN << "Проверка /health ML: " << (healthy ? "жив" : "не отвечает")
                     << " [X-Request-Id " << requestId << "]";
            if (!healthy)
            {
                co_return std::unexpected(SearchError::Unavailable);
            }
            break;
        }
    }

    const Clock::duration remaining = deadline - Clock::now();
    if (remaining <= Clock::duration::zero())
    {
        LOG_WARN << "Срок поиска вышел до запроса к ML [X-Request-Id " << requestId << "]";
        co_return std::unexpected(SearchError::Unavailable);
    }
    co_return co_await postSearch(ml, kind, std::move(mime), std::move(content),
                                  std::move(requestId), seconds(remaining));
}
