#pragma once

#include "common/BasicTypes.hpp"

#include <chrono>
#include <cstdint>
#include <type_traits>

namespace cmf
{

// ---------------------------------------------------------------------------
// FIX-like order & trade protocol between a trading engine and the (simulated)
// exchange that lives inside the backtest engine.
//
// Two message families flow over the wire:
//
//   Trading engine  ->  Backtest engine   (requests)
//     NewOrder, CancelOrder, ModifyOrder
//
//   Backtest engine ->  Trading engine    (notifications)
//     OrderAck, OrderFill, OrderReject
//
// Every message is a single trivially-copyable POD so it can travel through the
// lock-free SPSC ring buffer (LockFreeQueue) with a plain move/copy and no
// heap allocation on the hot path.
// ---------------------------------------------------------------------------

enum class MsgType : std::uint8_t
{
    None = 0,

    // Trading engine -> Backtest engine (requests)
    NewOrder,
    CancelOrder,
    ModifyOrder,

    // Backtest engine -> Trading engine (notifications)
    OrderAck,
    OrderFill,
    OrderReject,
};

// Lifecycle status carried by every message (FIX OrdStatus analogue).
enum class OrderStatus : std::uint8_t
{
    None = 0,
    PendingNew,      // request sent, not yet acknowledged
    New,             // accepted and resting on the book
    PartiallyFilled, // some quantity executed, remainder still working
    Filled,          // fully executed
    PendingCancel,   // cancel sent, not yet confirmed
    Cancelled,       // removed from the book
    Replaced,        // modify accepted
    Rejected,        // request refused
};

// Reason attached to an OrderReject (and to rejected cancels/modifies).
enum class RejectReason : std::uint8_t
{
    None = 0,
    UnknownOrder,      // cancel/modify referencing an order the book never saw
    DuplicateOrderId,  // NewOrder reusing a live order_id
    InvalidPrice,      // non-positive / malformed price
    InvalidSize,       // non-positive / malformed size
    InvalidInstrument, // instrument not tradable on this venue
    RiskLimit,         // blocked by pre-trade risk checks
    MarketClosed,      // venue not accepting orders
    Unsupported,       // message type not handled
};

// Monotonic high-resolution timestamp in nanoseconds since an arbitrary epoch.
// Used for latency measurement and for stamping protocol messages.
[[nodiscard]] inline NanoTime nowNanos() noexcept
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// ---------------------------------------------------------------------------
// OrderMessage
//
// A single flat record used for every protocol message. The required wire
// fields (order_id, instrument_id, side, price, size, timestamp_ns, status)
// are always present; fill-specific and reject-specific fields are populated
// only by OrderFill / OrderReject respectively.
// ---------------------------------------------------------------------------
struct OrderMessage
{
    // --- discriminator -----------------------------------------------------
    MsgType type = MsgType::None;
    OrderStatus status = OrderStatus::None;
    Side side = Side::None;
    RejectReason reject_reason = RejectReason::None;

    // --- routing -----------------------------------------------------------
    // Identifies which trading engine the message belongs to so that a single
    // backtest engine can multiplex many engines trading simultaneously.
    StrategyId engine_id = 0;
    SecurityId instrument_id = 0;

    // --- required wire fields ---------------------------------------------
    OrderId order_id = 0;
    NanoTime timestamp_ns = 0;
    Price price = 0.0;
    Quantity size = 0.0;

    // --- execution detail (OrderFill) -------------------------------------
    Price last_price = 0.0;   // price of this execution
    Quantity last_size = 0.0; // quantity of this execution
    Quantity leaves_size = 0.0; // remaining open quantity after this message

    // ---- factory helpers (the message schema) ----------------------------

    // NewOrder: trading engine submits a fresh limit order.
    [[nodiscard]] static OrderMessage makeNewOrder(StrategyId engine, OrderId id,
                                                   SecurityId instrument,
                                                   Side side, Price price,
                                                   Quantity size,
                                                   NanoTime ts) noexcept
    {
        OrderMessage m;
        m.type = MsgType::NewOrder;
        m.status = OrderStatus::PendingNew;
        m.engine_id = engine;
        m.order_id = id;
        m.instrument_id = instrument;
        m.side = side;
        m.price = price;
        m.size = size;
        m.leaves_size = size;
        m.timestamp_ns = ts;
        return m;
    }

    // CancelOrder: trading engine asks to remove a working order.
    [[nodiscard]] static OrderMessage makeCancelOrder(StrategyId engine,
                                                      OrderId id,
                                                      SecurityId instrument,
                                                      NanoTime ts) noexcept
    {
        OrderMessage m;
        m.type = MsgType::CancelOrder;
        m.status = OrderStatus::PendingCancel;
        m.engine_id = engine;
        m.order_id = id;
        m.instrument_id = instrument;
        m.timestamp_ns = ts;
        return m;
    }

    // ModifyOrder: trading engine amends price and/or size of a working order.
    [[nodiscard]] static OrderMessage makeModifyOrder(StrategyId engine,
                                                      OrderId id,
                                                      SecurityId instrument,
                                                      Side side, Price newPrice,
                                                      Quantity newSize,
                                                      NanoTime ts) noexcept
    {
        OrderMessage m;
        m.type = MsgType::ModifyOrder;
        m.status = OrderStatus::PendingNew;
        m.engine_id = engine;
        m.order_id = id;
        m.instrument_id = instrument;
        m.side = side;
        m.price = newPrice;
        m.size = newSize;
        m.leaves_size = newSize;
        m.timestamp_ns = ts;
        return m;
    }

    // OrderAck: backtest engine confirms acceptance of a request. The status
    // reflects the acknowledged request kind (New / Replaced / Cancelled).
    [[nodiscard]] static OrderMessage makeAck(const OrderMessage& req,
                                              NanoTime ts) noexcept
    {
        OrderMessage m = req;
        m.type = MsgType::OrderAck;
        switch (req.type)
        {
        case MsgType::ModifyOrder:
            m.status = OrderStatus::Replaced;
            break;
        case MsgType::CancelOrder:
            m.status = OrderStatus::Cancelled;
            break;
        default:
            m.status = OrderStatus::New;
            break;
        }
        m.reject_reason = RejectReason::None;
        m.timestamp_ns = ts;
        return m;
    }

    // OrderFill: backtest engine reports a (partial or full) execution.
    [[nodiscard]] static OrderMessage makeFill(const OrderMessage& req,
                                               Price fillPrice,
                                               Quantity fillSize,
                                               Quantity leaves,
                                               NanoTime ts) noexcept
    {
        OrderMessage m = req;
        m.type = MsgType::OrderFill;
        m.status = leaves > 0.0 ? OrderStatus::PartiallyFilled
                                : OrderStatus::Filled;
        m.reject_reason = RejectReason::None;
        m.last_price = fillPrice;
        m.last_size = fillSize;
        m.leaves_size = leaves;
        m.timestamp_ns = ts;
        return m;
    }

    // OrderReject: backtest engine refuses a request.
    [[nodiscard]] static OrderMessage makeReject(const OrderMessage& req,
                                                 RejectReason reason,
                                                 NanoTime ts) noexcept
    {
        OrderMessage m = req;
        m.type = MsgType::OrderReject;
        m.status = OrderStatus::Rejected;
        m.reject_reason = reason;
        m.timestamp_ns = ts;
        return m;
    }
};

// The message must be trivially copyable to live in the lock-free ring buffer.
static_assert(std::is_trivially_copyable_v<OrderMessage>,
              "OrderMessage must be trivially copyable for the SPSC ring");

[[nodiscard]] constexpr bool isRequest(MsgType t) noexcept
{
    return t == MsgType::NewOrder || t == MsgType::CancelOrder ||
           t == MsgType::ModifyOrder;
}

[[nodiscard]] constexpr bool isNotification(MsgType t) noexcept
{
    return t == MsgType::OrderAck || t == MsgType::OrderFill ||
           t == MsgType::OrderReject;
}

} // namespace cmf
