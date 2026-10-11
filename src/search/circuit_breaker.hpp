#pragma once

#include <chrono>
#include <mutex>
#include <optional>

// Автоматическое отключение вызовов ML-сервиса при подряд идущих авариях.
// Класс, потому что состояние общее для всех потоков ввода-вывода Drogon и держит инвариант:
// пока выключатель открыт, вызовы отклоняются, а проверку /health делает не больше одного
// запроса за раз. Время передаётся снаружи, чтобы тестировать без пауз.
class CircuitBreaker
{
  public:
    using Clock = std::chrono::steady_clock;

    enum class Gate
    {
        Call,   // закрыт: можно звать ML
        Probe,  // срок вышел: вызвавший обязан сходить в GET /health и сообщить onProbe
        Reject  // открыт до срока или проверка уже идёт
    };

    CircuitBreaker(int failuresToOpen, Clock::duration openFor);

    [[nodiscard]] Gate gate(Clock::time_point now);

    // ML ответил (не авария): серия сбрасывается, но открытый выключатель закрывает только проба
    void onReply();

    // Авария: серия растёт; на failuresToOpen закрытый выключатель открывается на openFor.
    // Уже открытый срок не продлевается.
    void onOutage(Clock::time_point now);

    // Результат пробы; всегда снимает признак идущей пробы
    void onProbe(bool healthy, Clock::time_point now);

  private:
    const int failuresToOpen_;
    const Clock::duration openFor_;
    std::mutex mutex_;
    int failures_ = 0;
    std::optional<Clock::time_point> openUntil_;
    bool probing_ = false;
};
