#include "protocol/BacktestGateway.hpp"
#include "protocol/OrderChannel.hpp"
#include "protocol/OrderMessages.hpp"
#include "protocol/TradingEngineConnector.hpp"

#include "catch2/catch_all.hpp"

#include <atomic>
#include <thread>
#include <vector>

using namespace cmf;

namespace
{
constexpr SecurityId kInstrument = 7;
}

TEST_CASE("OrderMessage - NewOrder carries all required fields", "[protocol]")
{
    const NanoTime ts = nowNanos();
    const OrderMessage m = OrderMessage::makeNewOrder(
        /*engine=*/3, /*id=*/101, kInstrument, Side::Buy, 100.5, 25.0, ts);

    REQUIRE(m.type == MsgType::NewOrder);
    REQUIRE(m.status == OrderStatus::PendingNew);
    REQUIRE(m.engine_id == 3);
    REQUIRE(m.order_id == 101);
    REQUIRE(m.instrument_id == kInstrument);
    REQUIRE(m.side == Side::Buy);
    REQUIRE(m.price == Catch::Approx(100.5));
    REQUIRE(m.size == Catch::Approx(25.0));
    REQUIRE(m.leaves_size == Catch::Approx(25.0));
    REQUIRE(m.timestamp_ns == ts);
}

TEST_CASE("OrderMessage - ack reflects acknowledged request kind", "[protocol]")
{
    const OrderMessage neworder = OrderMessage::makeNewOrder(
        1, 1, kInstrument, Side::Sell, 99.0, 5.0, nowNanos());
    REQUIRE(OrderMessage::makeAck(neworder, nowNanos()).status ==
            OrderStatus::New);

    const OrderMessage cancel =
        OrderMessage::makeCancelOrder(1, 1, kInstrument, nowNanos());
    REQUIRE(OrderMessage::makeAck(cancel, nowNanos()).status ==
            OrderStatus::Cancelled);

    const OrderMessage modify = OrderMessage::makeModifyOrder(
        1, 1, kInstrument, Side::Sell, 98.0, 4.0, nowNanos());
    REQUIRE(OrderMessage::makeAck(modify, nowNanos()).status ==
            OrderStatus::Replaced);
}

TEST_CASE("OrderMessage - fill computes status from remaining quantity",
          "[protocol]")
{
    const OrderMessage req = OrderMessage::makeNewOrder(
        1, 1, kInstrument, Side::Buy, 100.0, 10.0, nowNanos());

    const OrderMessage partial =
        OrderMessage::makeFill(req, 100.0, 4.0, 6.0, nowNanos());
    REQUIRE(partial.type == MsgType::OrderFill);
    REQUIRE(partial.status == OrderStatus::PartiallyFilled);
    REQUIRE(partial.last_size == Catch::Approx(4.0));
    REQUIRE(partial.leaves_size == Catch::Approx(6.0));

    const OrderMessage full =
        OrderMessage::makeFill(req, 100.0, 10.0, 0.0, nowNanos());
    REQUIRE(full.status == OrderStatus::Filled);
    REQUIRE(full.leaves_size == Catch::Approx(0.0));
}

TEST_CASE("Protocol - sendOrder round-trips to an ack", "[protocol]")
{
    OrderChannel channel;
    TradingEngineConnector engine(channel, /*engine_id=*/2);
    BacktestGateway gateway(channel);

    bool acked = false;
    OrderMessage received{};
    engine.onAck(
        [&](const OrderMessage& m)
        {
            acked = true;
            received = m;
        });

    const OrderId id =
        engine.sendOrder(kInstrument, Side::Buy, 100.25, 5.0);

    REQUIRE(gateway.pollAutoAck() == 1); // backtest sees and acks the order
    REQUIRE(engine.poll() == 1);         // engine receives the ack

    REQUIRE(acked);
    REQUIRE(received.type == MsgType::OrderAck);
    REQUIRE(received.status == OrderStatus::New);
    REQUIRE(received.order_id == id);
    REQUIRE(received.instrument_id == kInstrument);
    REQUIRE(received.side == Side::Buy);
}

TEST_CASE("Protocol - cancel is acknowledged as cancelled", "[protocol]")
{
    OrderChannel channel;
    TradingEngineConnector engine(channel, 1);
    BacktestGateway gateway(channel);

    OrderStatus status = OrderStatus::None;
    engine.onAck([&](const OrderMessage& m) { status = m.status; });

    const OrderId id = engine.sendOrder(kInstrument, Side::Sell, 50.0, 1.0);
    REQUIRE(gateway.pollAutoAck() == 1);
    REQUIRE(engine.poll() == 1);
    REQUIRE(status == OrderStatus::New);

    engine.cancelOrder(id, kInstrument);
    REQUIRE(gateway.pollAutoAck() == 1);
    REQUIRE(engine.poll() == 1);
    REQUIRE(status == OrderStatus::Cancelled);
}

TEST_CASE("Protocol - reject reaches the onReject handler", "[protocol]")
{
    OrderChannel channel;
    TradingEngineConnector engine(channel, 1);
    BacktestGateway gateway(channel);

    RejectReason reason = RejectReason::None;
    bool rejected = false;
    engine.onReject(
        [&](const OrderMessage& m)
        {
            rejected = true;
            reason = m.reject_reason;
        });

    engine.sendOrder(kInstrument, Side::Buy, -1.0, 5.0);

    // Custom matching logic: refuse the order.
    const std::size_t handled = gateway.poll(
        [&gateway](const OrderMessage& req)
        { gateway.reject(req, RejectReason::InvalidPrice); });
    REQUIRE(handled == 1);

    REQUIRE(engine.poll() == 1);
    REQUIRE(rejected);
    REQUIRE(reason == RejectReason::InvalidPrice);
}

TEST_CASE("Protocol - fill notification carries execution detail", "[protocol]")
{
    OrderChannel channel;
    TradingEngineConnector engine(channel, 1);
    BacktestGateway gateway(channel);

    OrderMessage fill{};
    bool filled = false;
    engine.onFill(
        [&](const OrderMessage& m)
        {
            filled = true;
            fill = m;
        });

    engine.sendOrder(kInstrument, Side::Buy, 100.0, 10.0);

    gateway.poll([&gateway](const OrderMessage& req)
                 { gateway.fill(req, 100.0, 10.0, 0.0); });

    REQUIRE(engine.poll() == 1);
    REQUIRE(filled);
    REQUIRE(fill.type == MsgType::OrderFill);
    REQUIRE(fill.status == OrderStatus::Filled);
    REQUIRE(fill.last_price == Catch::Approx(100.0));
    REQUIRE(fill.last_size == Catch::Approx(10.0));
    REQUIRE(fill.leaves_size == Catch::Approx(0.0));
}

TEST_CASE("Protocol - poll is non-blocking when nothing is pending",
          "[protocol]")
{
    OrderChannel channel;
    TradingEngineConnector engine(channel, 1);
    BacktestGateway gateway(channel);

    REQUIRE(engine.poll() == 0);
    REQUIRE(gateway.pollAutoAck() == 0);
}

TEST_CASE("Protocol - multiple engines trade simultaneously with unique ids",
          "[protocol]")
{
    OrderChannel channel_a;
    OrderChannel channel_b;
    TradingEngineConnector engine_a(channel_a, /*engine_id=*/1);
    TradingEngineConnector engine_b(channel_b, /*engine_id=*/2);
    BacktestGateway gateway_a(channel_a);
    BacktestGateway gateway_b(channel_b);

    const OrderId id_a = engine_a.sendOrder(kInstrument, Side::Buy, 10.0, 1.0);
    const OrderId id_b = engine_b.sendOrder(kInstrument, Side::Buy, 10.0, 1.0);

    // Ids are unique across engines despite identical local sequence.
    REQUIRE(id_a != id_b);

    StrategyId ack_engine_a = 0;
    StrategyId ack_engine_b = 0;
    engine_a.onAck([&](const OrderMessage& m) { ack_engine_a = m.engine_id; });
    engine_b.onAck([&](const OrderMessage& m) { ack_engine_b = m.engine_id; });

    REQUIRE(gateway_a.pollAutoAck() == 1);
    REQUIRE(gateway_b.pollAutoAck() == 1);
    REQUIRE(engine_a.poll() == 1);
    REQUIRE(engine_b.poll() == 1);

    REQUIRE(ack_engine_a == 1);
    REQUIRE(ack_engine_b == 2);
}

TEST_CASE("Protocol - concurrent round-trip across threads", "[protocol]")
{
    OrderChannel channel;
    BacktestGateway gateway(channel);

    std::atomic<bool> stop{false};
    std::thread backtest(
        [&]
        {
            while (!stop.load(std::memory_order_acquire))
                gateway.pollAutoAck();
        });

    TradingEngineConnector engine(channel, /*engine_id=*/1);

    constexpr int kOrders = 1000;
    int acks = 0;
    engine.onAck([&acks](const OrderMessage&) { ++acks; });

    for (int i = 0; i < kOrders; ++i)
    {
        const int before = acks;
        engine.sendOrder(kInstrument, Side::Buy, 100.0 + i, 1.0);
        while (acks == before)
            engine.poll();
    }

    stop.store(true, std::memory_order_release);
    backtest.join();

    REQUIRE(acks == kOrders);
}
