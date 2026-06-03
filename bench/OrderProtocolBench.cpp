#include "protocol/BacktestGateway.hpp"
#include "protocol/OrderChannel.hpp"
#include "protocol/OrderMessages.hpp"
#include "protocol/TradingEngineConnector.hpp"

#include <benchmark/benchmark.h>

#include <atomic>
#include <thread>

// ---------------------------------------------------------------------------
// Round-trip latency: time from submitting an order on the trading-engine side
// to receiving its acknowledgement back from the backtest engine.
//
// A dedicated backtest thread plays the role of the simulated exchange: it
// drains inbound requests and immediately acknowledges them. The benchmark
// thread is the trading engine; each timed iteration sends one order and waits
// for the matching ack — a single producer/consumer ping-pong across the two
// SPSC queues of one OrderChannel.
// ---------------------------------------------------------------------------

namespace
{

using namespace cmf;

constexpr SecurityId kInstrument = 42;
constexpr Price kPrice = 100.25;
constexpr Quantity kSize = 10.0;

// Raw channel RTT: measures the bare two-queue messaging cost with no
// std::function dispatch in the hot path.
void BM_OrderProtocol_RoundTrip_Raw(benchmark::State& state)
{
    OrderChannel channel;

    std::thread backtest(
        [&channel]
        {
            // Block-pop a request, ack it, repeat until the channel closes.
            while (channel.requests().pop(
                [&channel](OrderMessage&& req)
                {
                    channel.responses().push(
                        OrderMessage::makeAck(req, nowNanos()));
                }))
            {
            }
        });

    OrderId id = 0;
    for (auto _ : state)
    {
        channel.requests().push(OrderMessage::makeNewOrder(
            1, ++id, kInstrument, Side::Buy, kPrice, kSize, nowNanos()));

        // Block until the ack returns — one message guaranteed per request.
        (void)channel.responses().pop(
            [](OrderMessage&& ack) { benchmark::DoNotOptimize(ack); });
    }

    channel.requests().close(); // releases the backtest thread's blocking pop
    backtest.join();

    state.SetItemsProcessed(state.iterations());
}

// API-level RTT: measures the same round-trip through the public connector API
// (sendOrder + onAck callback dispatch), capturing the cost of the API layer.
void BM_OrderProtocol_RoundTrip_Api(benchmark::State& state)
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

    bool acked = false;
    engine.onAck([&acked](const OrderMessage&) { acked = true; });

    for (auto _ : state)
    {
        acked = false;
        engine.sendOrder(kInstrument, Side::Buy, kPrice, kSize);
        while (!acked)
            engine.poll();
    }

    stop.store(true, std::memory_order_release);
    backtest.join();

    state.SetItemsProcessed(state.iterations());
}

} // namespace

BENCHMARK(BM_OrderProtocol_RoundTrip_Raw)
    ->Name("OrderProtocol/RoundTrip/Raw")
    ->Unit(benchmark::kNanosecond)
    ->MinWarmUpTime(0.5)
    ->MinTime(1.0)
    ->Repetitions(10)
    ->DisplayAggregatesOnly()
    ->UseRealTime();

BENCHMARK(BM_OrderProtocol_RoundTrip_Api)
    ->Name("OrderProtocol/RoundTrip/Api")
    ->Unit(benchmark::kNanosecond)
    ->MinWarmUpTime(0.5)
    ->MinTime(1.0)
    ->Repetitions(10)
    ->DisplayAggregatesOnly()
    ->UseRealTime();
