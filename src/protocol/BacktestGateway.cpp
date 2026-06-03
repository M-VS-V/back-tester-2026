#include "protocol/BacktestGateway.hpp"

#include <algorithm>

namespace cmf
{

std::size_t BacktestGateway::poll(const RequestHandler& handler,
                                  std::size_t max_messages)
{
    OrderQueue& q = channel_.requests();

    // size() is a lower bound of published items for the consumer, so popping
    // exactly that many never blocks on an empty queue.
    const std::size_t budget = std::min(q.size(), max_messages);

    std::size_t handled = 0;
    for (; handled < budget; ++handled)
        (void)q.pop([&handler](OrderMessage&& m) { handler(m); });

    return handled;
}

std::size_t BacktestGateway::pollAutoAck(std::size_t max_messages)
{
    return poll(
        [this](const OrderMessage& req)
        {
            switch (req.type)
            {
            case MsgType::NewOrder:
            case MsgType::ModifyOrder:
            case MsgType::CancelOrder:
                ack(req);
                break;
            default:
                reject(req, RejectReason::Unsupported);
                break;
            }
        },
        max_messages);
}

} // namespace cmf
