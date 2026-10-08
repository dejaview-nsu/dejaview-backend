#include "search/circuit_breaker.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace
{

using Gate = CircuitBreaker::Gate;
using Clock = CircuitBreaker::Clock;
using namespace std::chrono_literals;

constexpr int kLimit = 5;
constexpr Clock::duration kOpenFor = 30s;
constexpr Clock::time_point kT0{};

void outages(CircuitBreaker &breaker, int count, Clock::time_point now = kT0)
{
    for (int i = 0; i < count; ++i)
    {
        breaker.onOutage(now);
    }
}

void open(CircuitBreaker &breaker) { outages(breaker, kLimit); }

}  // namespace

TEST(CircuitBreakerTest, FreshBreakerAllowsCalls)
{
    CircuitBreaker breaker(kLimit, kOpenFor);
    EXPECT_EQ(breaker.gate(kT0), Gate::Call);
}

TEST(CircuitBreakerTest, StaysClosedBelowTheLimit)
{
    CircuitBreaker breaker(kLimit, kOpenFor);
    outages(breaker, kLimit - 1);
    EXPECT_EQ(breaker.gate(kT0), Gate::Call);
}

TEST(CircuitBreakerTest, OpensOnTheLimitOutage)
{
    CircuitBreaker breaker(kLimit, kOpenFor);
    outages(breaker, kLimit);
    EXPECT_EQ(breaker.gate(kT0), Gate::Reject);
}

TEST(CircuitBreakerTest, ReplyResetsTheFailureStreak)
{
    CircuitBreaker breaker(kLimit, kOpenFor);
    outages(breaker, kLimit - 1);
    breaker.onReply();
    outages(breaker, kLimit - 1);
    EXPECT_EQ(breaker.gate(kT0), Gate::Call);
}

TEST(CircuitBreakerTest, RejectsUntilTheDeadline)
{
    CircuitBreaker breaker(kLimit, kOpenFor);
    open(breaker);
    EXPECT_EQ(breaker.gate(kT0 + kOpenFor - 1ns), Gate::Reject);
}

TEST(CircuitBreakerTest, AllowsProbeAtTheDeadline)
{
    CircuitBreaker breaker(kLimit, kOpenFor);
    open(breaker);
    EXPECT_EQ(breaker.gate(kT0 + kOpenFor), Gate::Probe);
}

TEST(CircuitBreakerTest, AllowsOnlyOneProbeAtATime)
{
    CircuitBreaker breaker(kLimit, kOpenFor);
    open(breaker);
    EXPECT_EQ(breaker.gate(kT0 + kOpenFor), Gate::Probe);
    EXPECT_EQ(breaker.gate(kT0 + kOpenFor), Gate::Reject);
    EXPECT_EQ(breaker.gate(kT0 + kOpenFor + 1h), Gate::Reject);
}

TEST(CircuitBreakerTest, HealthyProbeClosesTheBreaker)
{
    CircuitBreaker breaker(kLimit, kOpenFor);
    open(breaker);
    const Clock::time_point due = kT0 + kOpenFor;
    ASSERT_EQ(breaker.gate(due), Gate::Probe);
    breaker.onProbe(true, due);
    EXPECT_EQ(breaker.gate(due), Gate::Call);
}

TEST(CircuitBreakerTest, HealthyProbeRestartsTheFailureStreak)
{
    CircuitBreaker breaker(kLimit, kOpenFor);
    open(breaker);
    const Clock::time_point due = kT0 + kOpenFor;
    ASSERT_EQ(breaker.gate(due), Gate::Probe);
    breaker.onProbe(true, due);
    outages(breaker, kLimit - 1, due);
    EXPECT_EQ(breaker.gate(due), Gate::Call);
    breaker.onOutage(due);
    EXPECT_EQ(breaker.gate(due), Gate::Reject);
}

TEST(CircuitBreakerTest, FailedProbeReopensForTheFullPeriod)
{
    CircuitBreaker breaker(kLimit, kOpenFor);
    open(breaker);
    const Clock::time_point t1 = kT0 + kOpenFor;
    ASSERT_EQ(breaker.gate(t1), Gate::Probe);
    breaker.onProbe(false, t1);
    EXPECT_EQ(breaker.gate(t1 + 29s), Gate::Reject);
    EXPECT_EQ(breaker.gate(t1 + 30s), Gate::Probe);
}

TEST(CircuitBreakerTest, FailedProbeWithZeroPeriodAllowsAnotherProbe)
{
    CircuitBreaker breaker(kLimit, Clock::duration::zero());
    outages(breaker, kLimit);
    ASSERT_EQ(breaker.gate(kT0), Gate::Probe);
    breaker.onProbe(false, kT0);
    EXPECT_EQ(breaker.gate(kT0), Gate::Probe);
}

TEST(CircuitBreakerTest, ZeroPeriodAllowsProbeRightAfterOpening)
{
    CircuitBreaker breaker(kLimit, Clock::duration::zero());
    outages(breaker, kLimit);
    EXPECT_EQ(breaker.gate(kT0), Gate::Probe);
}

TEST(CircuitBreakerTest, OutageWhileOpenDoesNotExtendTheDeadline)
{
    CircuitBreaker breaker(kLimit, kOpenFor);
    open(breaker);
    outages(breaker, 3, kT0 + 10s);
    EXPECT_EQ(breaker.gate(kT0 + kOpenFor), Gate::Probe);
}

TEST(CircuitBreakerTest, ReplyWhileOpenDoesNotCloseTheBreaker)
{
    CircuitBreaker breaker(kLimit, kOpenFor);
    open(breaker);
    breaker.onReply();
    EXPECT_EQ(breaker.gate(kT0 + kOpenFor - 1ns), Gate::Reject);
}

TEST(CircuitBreakerTest, LimitOfOneOpensOnFirstOutage)
{
    CircuitBreaker breaker(1, kOpenFor);
    breaker.onOutage(kT0);
    EXPECT_EQ(breaker.gate(kT0), Gate::Reject);
}

TEST(CircuitBreakerTest, GrantsExactlyOneProbeToConcurrentCallers)
{
    CircuitBreaker breaker(kLimit, kOpenFor);
    open(breaker);
    const Clock::time_point due = kT0 + kOpenFor;
    constexpr int kThreads = 8;
    std::atomic<int> probes{0};
    std::atomic<int> rejects{0};
    {
        std::vector<std::jthread> threads;
        for (int i = 0; i < kThreads; ++i)
        {
            threads.emplace_back(
                [&]
                {
                    const Gate gate = breaker.gate(due);
                    (gate == Gate::Probe ? probes : rejects).fetch_add(1);
                });
        }
    }
    EXPECT_EQ(probes.load(), 1);
    EXPECT_EQ(rejects.load(), kThreads - 1);
}
