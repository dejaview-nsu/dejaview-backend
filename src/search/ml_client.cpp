#include "search/ml_client.hpp"

#include "search/ml_reply.hpp"

#include <drogon/drogon.h>

#include <sys/socket.h>

#include <algorithm>
#include <cerrno>
#include <chrono>

using namespace drogon;

namespace
{

using Clock = CircuitBreaker::Clock;
using SearchResult = std::expected<std::vector<std::int64_t>, SearchError>;

constexpr std::chrono::seconds kHealthTimeout{1};
// установка соединения - не дольше 1 с, внутри общего срока
constexpr std::chrono::seconds kConnectTimeout{1};
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

// Drogon при тайм-ауте только вызывает callback, а отправленный запрос и соединение держит, пока
// ML не ответит (HttpClientImpl.cc, sendRequestInLoop), поэтому сокет закрываем сами. Он же
// ограничивает установку соединения. Сокет запоминается в sockopt-callback, таймер и shutdown
// работают в потоке цикла клиента; корутина после co_await возобновляется из callback Drogon
// в том же потоке (HttpRespAwaiter::await_suspend), т.е. пока соединение живо и fd не мог быть
// закрыт и выдан заново. Деструктор вызывается там же и помечает запрос завершённым: таймер
// соединения, сработав позже, ничего не делает.
class ConnectionWatch
{
  public:
    ConnectionWatch(HttpClient &client, double timeoutSeconds) : state_(std::make_shared<State>())
    {
        client.setSockOptCallback([state = state_](int fd) { state->fd = fd; });
        client.getLoop()->runAfter(std::min(seconds(kConnectTimeout), timeoutSeconds),
                                   [state = state_] { state->abortIfNotConnected(); });
    }
    ~ConnectionWatch() { state_->finished = true; }
    ConnectionWatch(const ConnectionWatch &) = delete;
    ConnectionWatch &operator=(const ConnectionWatch &) = delete;

    void abortOnTimeout(const std::exception &error) const
    {
        const auto *http = dynamic_cast<const HttpException *>(&error);
        if (http != nullptr && http->code() == ReqResult::Timeout)
        {
            state_->abort();
        }
    }

  private:
    struct State
    {
        int fd = -1;
        bool finished = false;

        void abort() const
        {
            if (fd >= 0)
            {
                ::shutdown(fd, SHUT_RDWR);
            }
        }

        void abortIfNotConnected() const
        {
            sockaddr_storage peer{};
            socklen_t length = sizeof(peer);
            if (!finished && fd >= 0 &&
                ::getpeername(fd, reinterpret_cast<sockaddr *>(&peer), &length) != 0 &&
                errno == ENOTCONN)
            {
                abort();
            }
        }
    };

    std::shared_ptr<State> state_;
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
    const ConnectionWatch watch(*client, timeoutSeconds);
    try
    {
        const auto response =
            co_await client->sendRequestCoro(newRequest(Get, "/health", requestId), timeoutSeconds);
        co_return response->statusCode() == k200OK;
    }
    catch (const std::exception &e)
    {
        watch.abortOnTimeout(e);
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
    const ConnectionWatch watch(*client, timeoutSeconds);
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
        watch.abortOnTimeout(e);
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
