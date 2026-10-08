#include "search/ml_client.hpp"

#include "integration/db_test.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

using namespace drogon;
using namespace std::chrono_literals;
using namespace std::string_literals;

namespace
{

using SteadyClock = std::chrono::steady_clock;
using Result = std::expected<std::vector<std::int64_t>, SearchError>;

const std::string kPng = "\x89PNG\r\n\x1a\n"s + "payload"s;
const std::string kWebm = "\x1A\x45\xDF\xA3webm-bytes"s;
const std::string kImagePath = "/v1/search/image";
const std::string kVideoPath = "/v1/search/video";
const std::string kFoundBody = R"({"movie_ids":[603]})";
const std::vector<std::int64_t> kFound{603};

// Слушающий сокет на 127.0.0.1 с портом от системы. Соединения ядро принимает само (backlog), а
// accept вызывает тест - так видно, сколько раз backend подключался
class Listener
{
  public:
    Listener()
    {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0)
        {
            throw std::runtime_error("socket");
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        socklen_t length = sizeof(address);
        if (::bind(fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
            ::listen(fd_, 16) != 0 ||
            ::getsockname(fd_, reinterpret_cast<sockaddr *>(&address), &length) != 0)
        {
            ::close(fd_);
            throw std::runtime_error("listener");
        }
        port_ = ntohs(address.sin_port);
        ::fcntl(fd_, F_SETFL, ::fcntl(fd_, F_GETFL) | O_NONBLOCK);
    }

    ~Listener()
    {
        for (const int connection : accepted_)
        {
            ::close(connection);
        }
        ::close(fd_);
    }

    Listener(const Listener &) = delete;
    Listener &operator=(const Listener &) = delete;

    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_); }

    // Принять одно ожидающее соединение (-1 - нет); закрывает его вызывающий
    int acceptOne() const { return ::accept(fd_, nullptr, nullptr); }

    // Принять все ожидающие соединения, вернуть сколько их было за всё время
    int acceptPending()
    {
        for (int connection = acceptOne(); connection >= 0; connection = acceptOne())
        {
            accepted_.push_back(connection);
        }
        return static_cast<int>(accepted_.size());
    }

  private:
    int fd_ = -1;
    std::uint16_t port_ = 0;
    std::vector<int> accepted_;
};

// ML, у которого /health отвечает через 300 мс, а поиск не отвечает никогда
class SlowMl
{
  public:
    SlowMl() : thread_([this](const std::stop_token &stop) { serve(stop); }) {}

    ~SlowMl()
    {
        thread_.request_stop();
        thread_.join();
    }

    SlowMl(const SlowMl &) = delete;
    SlowMl &operator=(const SlowMl &) = delete;

    std::string url() const { return listener_.url(); }

  private:
    static std::string readRequest(int connection)
    {
        const timeval timeout{.tv_sec = 1, .tv_usec = 0};
        ::setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        char buffer[1024];
        const ssize_t size = ::recv(connection, buffer, sizeof(buffer), 0);
        return size > 0 ? std::string(buffer, static_cast<std::size_t>(size)) : std::string();
    }

    void serve(const std::stop_token &stop)
    {
        static constexpr std::string_view kHealthReply =
            "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}";
        while (!stop.stop_requested())
        {
            const int connection = listener_.acceptOne();
            if (connection < 0)
            {
                std::this_thread::sleep_for(5ms);
                continue;
            }
            if (readRequest(connection).starts_with("GET /health"))
            {
                std::this_thread::sleep_for(300ms);
                ::send(connection, kHealthReply.data(), kHealthReply.size(), MSG_NOSIGNAL);
            }
            held_.push_back(connection);  // поиск остаётся без ответа
        }
        for (const int connection : held_)
        {
            ::close(connection);
        }
    }

    Listener listener_;
    std::vector<int> held_;
    std::jthread thread_;
};

}  // namespace

class MlClientTest : public DbTest
{
  protected:
    MlConfig config;

    void SetUp() override
    {
        DbTest::SetUp();
        config.url = fakeUrl;
        config.imageTimeout = 3000ms;
        config.videoTimeout = 3000ms;
    }

    std::shared_ptr<MlService> service() { return std::make_shared<MlService>(config); }

    static Result image(const std::shared_ptr<MlService> &ml, std::string requestId = "req-1")
    {
        return run(searchMl(ml, MediaKind::Image, "image/png", kPng, std::move(requestId)));
    }

    static Result video(const std::shared_ptr<MlService> &ml)
    {
        return run(searchMl(ml, MediaKind::Video, "video/webm", kWebm, "req-1"));
    }

    static void expectError(const Result &result, SearchError error)
    {
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error(), error);
    }

    static void expectFound(const Result &result)
    {
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(*result, kFound);
    }

    static void replyImage(HttpStatusCode status, const std::string &json)
    {
        fakeReply(kImagePath, status, json);
    }
};

TEST_F(MlClientTest, ReturnsMovieIdsFromMl)
{
    replyImage(k200OK, kFoundBody);
    expectFound(image(service()));
}

TEST_F(MlClientTest, SendsRawBytesWithDetectedTypeAndRequestId)
{
    replyImage(k200OK, kFoundBody);
    expectFound(image(service()));

    const auto received = fakeReceived(kImagePath);
    ASSERT_NE(received, nullptr);
    EXPECT_EQ(received->method(), Post);
    EXPECT_EQ(received->getHeader("content-type"), "image/png");
    EXPECT_EQ(received->getHeader("x-request-id"), "req-1");
    EXPECT_EQ(std::string(received->body()), kPng);
}

TEST_F(MlClientTest, SendsVideoToVideoEndpoint)
{
    fakeReply(kVideoPath, k200OK, kFoundBody);
    expectFound(video(service()));

    const auto received = fakeReceived(kVideoPath);
    ASSERT_NE(received, nullptr);
    EXPECT_EQ(received->getHeader("content-type"), "video/webm");
    EXPECT_EQ(std::string(received->body()), kWebm);
    EXPECT_EQ(fakeReceived(kImagePath), nullptr);
}

TEST_F(MlClientTest, MapsMlClientErrors)
{
    replyImage(k422UnprocessableEntity, R"({"code":"FILE_CORRUPTED"})");
    expectError(image(service()), SearchError::FileCorrupted);

    replyImage(k413RequestEntityTooLarge, R"({"code":"FILE_TOO_LARGE"})");
    expectError(image(service()), SearchError::FileTooLarge);

    replyImage(k415UnsupportedMediaType, R"({"code":"UNSUPPORTED_MEDIA_TYPE"})");
    expectError(image(service()), SearchError::UnsupportedFormat);
}

TEST_F(MlClientTest, MapsVideoTooLongForVideo)
{
    fakeReply(kVideoPath, k422UnprocessableEntity, R"({"code":"DURATION_TOO_LONG"})");
    expectError(video(service()), SearchError::VideoTooLong);
}

TEST_F(MlClientTest, MapsServerErrorsAndMissingEndpointToUnavailable)
{
    replyImage(k503ServiceUnavailable, R"({"code":"NOT_READY"})");
    expectError(image(service()), SearchError::Unavailable);

    replyImage(k500InternalServerError, "{}");
    expectError(image(service()), SearchError::Unavailable);

    fakeReply(kImagePath, k404NotFound, "{}");
    expectError(image(service()), SearchError::Unavailable);
}

TEST_F(MlClientTest, ReportsUnavailableWhenConnectionIsRefused)
{
    config.url = "http://127.0.0.1:1";
    const auto start = SteadyClock::now();
    expectError(image(service()), SearchError::Unavailable);
    EXPECT_LT(SteadyClock::now() - start, 1s);
}

TEST_F(MlClientTest, TimesOutOnHungMlWithoutRetry)
{
    Listener hung;
    config.url = hung.url();
    config.imageTimeout = 300ms;

    const auto start = SteadyClock::now();
    expectError(image(service()), SearchError::Unavailable);
    const auto elapsed = SteadyClock::now() - start;

    EXPECT_GE(elapsed, 300ms);
    EXPECT_LT(elapsed, 2s);
    EXPECT_EQ(hung.acceptPending(), 1);
}

TEST_F(MlClientTest, ReturnsBusyWhenAllSlotsAreTaken)
{
    replyImage(k200OK, kFoundBody);
    config.maxConcurrent = 1;
    const auto ml = service();
    ASSERT_TRUE(ml->slots.try_acquire());

    expectError(image(ml), SearchError::Busy);
    EXPECT_EQ(fakeReceived(kImagePath), nullptr);

    ml->slots.release();
    expectFound(image(ml));
}

TEST_F(MlClientTest, ReleasesSlotAfterFailure)
{
    config.maxConcurrent = 1;
    const auto ml = service();
    replyImage(k500InternalServerError, "{}");
    expectError(image(ml), SearchError::Unavailable);
    replyImage(k200OK, kFoundBody);
    expectFound(image(ml));
}

TEST_F(MlClientTest, OpensBreakerAfterConsecutiveOutages)
{
    config.failuresToOpen = 2;
    config.openFor = 1h;
    const auto ml = service();
    replyImage(k500InternalServerError, "{}");
    expectError(image(ml), SearchError::Unavailable);
    expectError(image(ml), SearchError::Unavailable);

    replyImage(k200OK, kFoundBody);
    expectError(image(ml), SearchError::Unavailable);
}

TEST_F(MlClientTest, ClientErrorsDoNotOpenBreaker)
{
    config.failuresToOpen = 2;
    config.openFor = 1h;
    const auto ml = service();
    replyImage(k422UnprocessableEntity, R"({"code":"FILE_CORRUPTED"})");
    for (int i = 0; i < 3; ++i)
    {
        expectError(image(ml), SearchError::FileCorrupted);
    }

    replyImage(k200OK, kFoundBody);
    expectFound(image(ml));
}

TEST_F(MlClientTest, ReplyResetsOutageStreak)
{
    config.failuresToOpen = 2;
    config.openFor = 1h;
    const auto ml = service();
    replyImage(k500InternalServerError, "{}");
    expectError(image(ml), SearchError::Unavailable);
    replyImage(k200OK, kFoundBody);
    expectFound(image(ml));
    replyImage(k500InternalServerError, "{}");
    expectError(image(ml), SearchError::Unavailable);

    replyImage(k200OK, kFoundBody);
    expectFound(image(ml));
}

TEST_F(MlClientTest, ProbesHealthAfterBreakerOpenPeriod)
{
    config.failuresToOpen = 1;
    config.openFor = 0ms;
    const auto ml = service();
    replyImage(k500InternalServerError, "{}");
    expectError(image(ml), SearchError::Unavailable);

    replyImage(k200OK, kFoundBody);
    expectError(image(ml), SearchError::Unavailable);  // /health -> 404

    fakeReply("/health", k200OK, "{}");
    expectFound(image(ml));

    fakeReply("/health", k503ServiceUnavailable, "{}");
    expectFound(image(ml));  // breaker closed, no probe
}

TEST_F(MlClientTest, FailedProbeTransportDoesNotLeaveBreakerStuck)
{
    config.failuresToOpen = 1;
    config.openFor = 0ms;
    config.url = "http://127.0.0.1:1";
    const auto ml = service();
    expectError(image(ml), SearchError::Unavailable);  // POST refused -> open
    expectError(image(ml), SearchError::Unavailable);  // probe refused

    ml->config.url = fakeUrl;
    fakeReply("/health", k200OK, "{}");
    replyImage(k200OK, kFoundBody);
    expectFound(image(ml));
}

TEST_F(MlClientTest, ProbeAndSearchShareOneDeadline)
{
    SlowMl slow;
    config.url = slow.url();
    config.failuresToOpen = 1;
    config.openFor = 0ms;
    config.imageTimeout = 500ms;
    const auto ml = service();
    ml->breaker.onOutage(SteadyClock::now());

    const auto start = SteadyClock::now();
    expectError(image(ml), SearchError::Unavailable);
    const auto elapsed = SteadyClock::now() - start;
    EXPECT_GE(elapsed, 450ms);  // здоровый зонд, затем поиск ждал ответа до общего срока
    EXPECT_LT(elapsed, 700ms);
}

TEST_F(MlClientTest, CompletesWhenCallerGivesUpTheOnlyOwner)
{
    replyImage(k200OK, kFoundBody);
    expectFound(run(searchMl(std::make_shared<MlService>(config), MediaKind::Image, "image/png",
                             kPng, "req-1")));
}
