#include "protocol/TradingEngineConnector.hpp"

#include <algorithm>

namespace cmf
{

namespace
{
// Pack the engine id into the high bits so order ids are unique across engines.
constexpr int kEngineIdShift = 48;
} // namespace

TradingEngineConnector::TradingEngineConnector(OrderChannel& channel,
                                               StrategyId engine_id)
    : channel_(channel), engine_id_(engine_id),
      next_order_id_(static_cast<OrderId>(engine_id) << kEngineIdShift)
{
}

OrderId TradingEngineConnector::sendOrder(SecurityId instrument, Side side,
                                          Price price, Quantity size)
{
    const OrderId id = ++next_order_id_;
    channel_.requests().push(OrderMessage::makeNewOrder(
        engine_id_, id, instrument, side, price, size, nowNanos()));
    return id;
}

void TradingEngineConnector::cancelOrder(OrderId id, SecurityId instrument)
{
    channel_.requests().push(
        OrderMessage::makeCancelOrder(engine_id_, id, instrument, nowNanos()));
}

void TradingEngineConnector::modifyOrder(OrderId id, SecurityId instrument,
                                         Side side, Price newPrice,
                                         Quantity newSize)
{
    channel_.requests().push(OrderMessage::makeModifyOrder(
        engine_id_, id, instrument, side, newPrice, newSize, nowNanos()));
}

void TradingEngineConnector::dispatch(const OrderMessage& msg) const
{
    switch (msg.type)
    {
    case MsgType::OrderAck:
        if (on_ack_)
            on_ack_(msg);
        break;
    case MsgType::OrderFill:
        if (on_fill_)
            on_fill_(msg);
        break;
    case MsgType::OrderReject:
        if (on_reject_)
            on_reject_(msg);
        break;
    default:
        // Requests never arrive on the responses queue; ignore defensively.
        break;
    }
}

std::size_t TradingEngineConnector::poll(std::size_t max_messages)
{
    OrderQueue& q = channel_.responses();

    // size() is a lower bound of published items for the consumer, so popping
    // exactly that many never blocks on an empty queue.
    const std::size_t budget = std::min(q.size(), max_messages);

    std::size_t handled = 0;
    for (; handled < budget; ++handled)
        (void)q.pop([this](OrderMessage&& m) { dispatch(m); });

    return handled;
}

} // namespace cmf
