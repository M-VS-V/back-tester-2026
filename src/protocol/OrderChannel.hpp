#pragma once

#include "common/LockFreeQueue.hpp"
#include "protocol/OrderMessages.hpp"

#include <cstddef>

namespace cmf
{

// ---------------------------------------------------------------------------
// OrderChannel
//
// A thread-safe in-process channel connecting exactly one trading engine to
// the backtest engine. It is built from two independent lock-free SPSC ring
// buffers, one per direction:
//
//   requests()   trading engine (producer) -> backtest engine (consumer)
//   responses()  backtest engine (producer) -> trading engine (consumer)
//
// Because each queue has a single producer and single consumer, the SPSC
// LockFreeQueue invariants hold without any additional locking. Run multiple
// trading engines simultaneously by giving each its own OrderChannel.
// ---------------------------------------------------------------------------

// Ring capacity per direction. Power of two as required by LockFreeQueue.
inline constexpr std::size_t kOrderChannelCapacity = 1u << 14; // 16384 slots

using OrderQueue = LockFreeQueue<OrderMessage, kOrderChannelCapacity>;

class OrderChannel
{
  public:
    OrderChannel() = default;
    OrderChannel(const OrderChannel&) = delete;
    OrderChannel& operator=(const OrderChannel&) = delete;
    OrderChannel(OrderChannel&&) = delete;
    OrderChannel& operator=(OrderChannel&&) = delete;
    ~OrderChannel() = default;

    // Trading engine -> backtest engine.
    [[nodiscard]] OrderQueue& requests() noexcept { return requests_; }
    [[nodiscard]] const OrderQueue& requests() const noexcept
    {
        return requests_;
    }

    // Backtest engine -> trading engine.
    [[nodiscard]] OrderQueue& responses() noexcept { return responses_; }
    [[nodiscard]] const OrderQueue& responses() const noexcept
    {
        return responses_;
    }

    // Stop both directions; in-flight messages can still be drained, after
    // which pop() returns false on the closed queue.
    void close() noexcept
    {
        requests_.close();
        responses_.close();
    }

  private:
    OrderQueue requests_;
    OrderQueue responses_;
};

} // namespace cmf
