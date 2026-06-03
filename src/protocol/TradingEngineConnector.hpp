#pragma once

#include "common/BasicTypes.hpp"
#include "protocol/OrderChannel.hpp"
#include "protocol/OrderMessages.hpp"

#include <cstddef>
#include <functional>
#include <limits>

namespace cmf
{

// ---------------------------------------------------------------------------
// TradingEngineConnector
//
// The trading-engine side of an OrderChannel. It is the single producer of the
// requests queue and the single consumer of the responses queue, so every
// method below must be called from the one thread that owns this engine.
//
// Outbound API (engine -> backtest):
//   sendOrder()   - submit a NewOrder, returns the assigned order_id
//   cancelOrder() - submit a CancelOrder
//   modifyOrder() - submit a ModifyOrder
//
// Inbound API (backtest -> engine):
//   onAck(), onFill(), onReject() register callbacks; poll() drains pending
//   notifications and dispatches them to those callbacks.
// ---------------------------------------------------------------------------
class TradingEngineConnector
{
  public:
    using Handler = std::function<void(const OrderMessage&)>;

    // engine_id distinguishes this engine when many trade simultaneously and
    // also seeds locally-unique order ids.
    TradingEngineConnector(OrderChannel& channel, StrategyId engine_id);

    [[nodiscard]] StrategyId engineId() const noexcept { return engine_id_; }

    // --- outbound -----------------------------------------------------------

    // Submit a new limit order; returns the generated order_id.
    OrderId sendOrder(SecurityId instrument, Side side, Price price,
                      Quantity size);

    // Cancel a previously submitted working order.
    void cancelOrder(OrderId id, SecurityId instrument);

    // Amend price and/or size of a working order.
    void modifyOrder(OrderId id, SecurityId instrument, Side side,
                     Price newPrice, Quantity newSize);

    // --- inbound callbacks --------------------------------------------------

    void onAck(Handler handler) { on_ack_ = std::move(handler); }
    void onFill(Handler handler) { on_fill_ = std::move(handler); }
    void onReject(Handler handler) { on_reject_ = std::move(handler); }

    // Drain pending notifications from the backtest engine and dispatch each to
    // the matching callback. Non-blocking: only messages already published are
    // consumed. Returns the number of notifications handled.
    std::size_t poll(std::size_t max_messages =
                         std::numeric_limits<std::size_t>::max());

  private:
    void dispatch(const OrderMessage& msg) const;

    OrderChannel& channel_;
    StrategyId engine_id_;
    OrderId next_order_id_;

    Handler on_ack_;
    Handler on_fill_;
    Handler on_reject_;
};

} // namespace cmf
