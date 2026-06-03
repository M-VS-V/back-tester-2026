# Order & Trade Protocol (FIX-like in-process connector)

Group 1 deliverable: the internal messaging protocol between a **trading
engine** and the **backtest engine** (the in-process exchange simulation).

A trading engine collects market data and information about its own previous
orders from the backtest engine, processes it, and sends new orders. Multiple
trading engines can trade simultaneously, each over its own channel. This
module defines the message schema, the transport, and the C++ API used by both
sides.

## Message flow

```
            NewOrder / CancelOrder / ModifyOrder
   Trading  ───────────────────────────────────────►  Backtest
   engine                                               engine
            ◄───────────────────────────────────────
            OrderAck / OrderFill / OrderReject
```

* **Trading engine → backtest** (requests): `NewOrder`, `CancelOrder`,
  `ModifyOrder`
* **Backtest → trading engine** (notifications): `OrderAck`, `OrderFill`,
  `OrderReject`

## Files

| File | Responsibility |
| --- | --- |
| `OrderMessages.hpp` | Message schema (`OrderMessage` POD), `MsgType`, `OrderStatus`, `RejectReason`, factory helpers, `nowNanos()` |
| `OrderChannel.hpp` | Bidirectional channel = two lock-free SPSC ring buffers, one per direction |
| `TradingEngineConnector.hpp/.cpp` | Trading-engine-side C++ API |
| `BacktestGateway.hpp/.cpp` | Backtest-engine-side (simulated exchange) API |

## Message schema

Every message is a single, trivially-copyable `OrderMessage` POD so it can pass
through the lock-free ring buffer with no heap allocation on the hot path. The
`type` field discriminates the six message kinds.

Each message carries the required wire fields:

| Field | Type | Notes |
| --- | --- | --- |
| `order_id` | `OrderId` (u64) | unique within an engine |
| `instrument_id` | `SecurityId` (u16) | traded instrument |
| `side` | `Side` | `Buy` / `Sell` / `None` |
| `price` | `Price` (double) | order / limit price |
| `size` | `Quantity` (double) | order quantity |
| `timestamp_ns` | `NanoTime` (i64) | monotonic ns timestamp |
| `status` | `OrderStatus` | lifecycle state (FIX `OrdStatus` analogue) |

Extra fields: `engine_id` (routing across simultaneous engines),
`reject_reason` (for `OrderReject`), and `last_price` / `last_size` /
`leaves_size` (for `OrderFill`).

Messages are built through named factories that keep the fields consistent, e.g.
`OrderMessage::makeNewOrder(...)`, `makeAck(req, ts)`, `makeFill(...)`,
`makeReject(...)`.

## Transport: thread-safe in-process channel

`OrderChannel` is built from two independent `LockFreeQueue` instances
(the project's existing **lock-free SPSC** ring buffer):

* `requests()` — trading engine (single producer) → backtest engine (single
  consumer)
* `responses()` — backtest engine (single producer) → trading engine (single
  consumer)

Because each queue has exactly one producer and one consumer, the SPSC
invariants hold with **no locks**. To run many trading engines at once, give
each its own `OrderChannel`; the `engine_id` on every message identifies the
owner.

## C++ API

### Trading engine — `TradingEngineConnector`

```cpp
OrderChannel channel;
TradingEngineConnector engine(channel, /*engine_id=*/1);

engine.onAck   ([](const OrderMessage& m) { /* order accepted        */ });
engine.onFill  ([](const OrderMessage& m) { /* (partial) execution   */ });
engine.onReject([](const OrderMessage& m) { /* request refused       */ });

OrderId id = engine.sendOrder(instrument, Side::Buy, price, size);
engine.modifyOrder(id, instrument, Side::Buy, newPrice, newSize);
engine.cancelOrder(id, instrument);

engine.poll(); // non-blocking: drains notifications, fires callbacks
```

* `sendOrder()` / `cancelOrder()` / `modifyOrder()` — produce requests.
* `onAck()` / `onFill()` / `onReject()` — register inbound callbacks.
* `poll()` — drains only already-published notifications (never blocks on an
  empty queue) and dispatches them.

All methods must be called from the single thread that owns the engine.

### Backtest engine — `BacktestGateway`

```cpp
BacktestGateway gateway(channel);

// Custom matching:
gateway.poll([&](const OrderMessage& req) {
    if (req.price <= 0.0) gateway.reject(req, RejectReason::InvalidPrice);
    else                  gateway.ack(req);
});

// Or the built-in auto-acknowledging stub:
gateway.pollAutoAck();
```

`ack()`, `fill()`, and `reject()` emit the corresponding notifications back to
the trading engine.

## Latency benchmark

`bench/OrderProtocolBench.cpp` measures **round-trip time from order submission
to acknowledgement**. A dedicated backtest thread acks inbound orders; the
benchmark thread sends an order and waits for its ack (a ping-pong across the
two SPSC queues).

Two variants are measured:

* `OrderProtocol/RoundTrip/Raw` — bare channel, no callback dispatch.
* `OrderProtocol/RoundTrip/Api` — through the public connector API.

Reference numbers on the development machine (Apple Silicon, Release `-O3
-march=native`):

| Benchmark | Median RTT |
| --- | --- |
| Raw channel | ~209 ns |
| Connector API | ~222 ns |

## Build, test, run

From the repository root:

```bash
cmake -B build -S .
cmake --build build -j

# unit tests (includes the protocol tests in test/OrderProtocolTest.cpp)
ctest --test-dir build -j

# latency benchmark
build/bin/bench/back-tester-benchmarks --benchmark_filter=OrderProtocol
```

## Tests

`test/OrderProtocolTest.cpp` covers the schema, the single-threaded round trip,
cancel and modify acknowledgements, the reject and fill paths, simultaneous
multi-engine trading with unique order ids, and a concurrent cross-thread round
trip.
