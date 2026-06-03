#pragma once

#include "protocol/OrderChannel.hpp"
#include "protocol/OrderMessages.hpp"

#include <cstddef>
#include <functional>
#include <limits>

namespace cmf
{

// ---------------------------------------------------------------------------
// BacktestGateway
//
// The backtest-engine (simulated exchange) side of an OrderChannel. It is the
// single consumer of the requests queue and the single producer of the
// responses queue, mirroring TradingEngineConnector.
//
// poll() drains inbound requests (NewOrder / CancelOrder / ModifyOrder) and
// hands each to a user-supplied matching callback. The callback decides the
// outcome and replies through the emit helpers:
//
//   ack()    -> OrderAck
//   fill()   -> OrderFill
//   reject() -> OrderReject
//
// A default auto-acknowledging poll (pollAutoAck) is provided for the latency
// benchmark and simple smoke tests.
// ---------------------------------------------------------------------------
class BacktestGateway
{
  public:
    using RequestHandler = std::function<void(const OrderMessage&)>;

    explicit BacktestGateway(OrderChannel& channel) : channel_(channel) {}

    // --- outbound notifications (backtest -> engine) -----------------------

    void ack(const OrderMessage& request)
    {
        channel_.responses().push(OrderMessage::makeAck(request, nowNanos()));
    }

    void fill(const OrderMessage& request, Price fillPrice, Quantity fillSize,
              Quantity leaves)
    {
        channel_.responses().push(OrderMessage::makeFill(
            request, fillPrice, fillSize, leaves, nowNanos()));
    }

    void reject(const OrderMessage& request, RejectReason reason)
    {
        channel_.responses().push(
            OrderMessage::makeReject(request, reason, nowNanos()));
    }

    // --- inbound requests (engine -> backtest) -----------------------------

    // Drain pending requests, dispatching each to handler. Non-blocking.
    std::size_t poll(const RequestHandler& handler,
                     std::size_t max_messages =
                         std::numeric_limits<std::size_t>::max());

    // Drain pending requests, immediately acknowledging NewOrder/ModifyOrder
    // and confirming CancelOrder. Useful as a minimal matching stub.
    std::size_t pollAutoAck(std::size_t max_messages =
                                std::numeric_limits<std::size_t>::max());

  private:
    OrderChannel& channel_;
};

} // namespace cmf
