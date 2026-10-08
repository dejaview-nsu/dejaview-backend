#include "search/circuit_breaker.hpp"

CircuitBreaker::CircuitBreaker(int failuresToOpen, Clock::duration openFor)
    : failuresToOpen_(failuresToOpen), openFor_(openFor)
{
}

CircuitBreaker::Gate CircuitBreaker::gate(Clock::time_point now)
{
    const std::scoped_lock lock(mutex_);
    if (!openUntil_)
    {
        return Gate::Call;
    }
    if (probing_ || now < *openUntil_)
    {
        return Gate::Reject;
    }
    probing_ = true;
    return Gate::Probe;
}

void CircuitBreaker::onReply()
{
    const std::scoped_lock lock(mutex_);
    failures_ = 0;
}

void CircuitBreaker::onOutage(Clock::time_point now)
{
    const std::scoped_lock lock(mutex_);
    ++failures_;
    if (failures_ >= failuresToOpen_ && !openUntil_)
    {
        openUntil_ = now + openFor_;
    }
}

void CircuitBreaker::onProbe(bool healthy, Clock::time_point now)
{
    const std::scoped_lock lock(mutex_);
    probing_ = false;
    if (healthy)
    {
        failures_ = 0;
        openUntil_.reset();
    }
    else
    {
        openUntil_ = now + openFor_;
    }
}
