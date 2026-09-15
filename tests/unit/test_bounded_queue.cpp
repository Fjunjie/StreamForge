#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>
#include <vector>

#include "streamforge/core/bounded_queue.hpp"

using streamforge::BoundedQueue;

// Note: Catch2 assertion macros must only run on the main thread; worker threads set
// atomic flags that the main thread asserts on after joining.

TEST_CASE("queue preserves fifo order", "[queue]") {
    BoundedQueue<int> q(8);
    REQUIRE(q.push(1));
    REQUIRE(q.push(2));
    REQUIRE(q.push(3));
    int out = 0;
    REQUIRE(q.pop(out) == BoundedQueue<int>::PopStatus::Got);
    CHECK(out == 1);
    REQUIRE(q.pop(out) == BoundedQueue<int>::PopStatus::Got);
    CHECK(out == 2);
}

TEST_CASE("queue rejects push when full", "[queue]") {
    BoundedQueue<int> q(2);
    REQUIRE(q.push(1));
    REQUIRE(q.push(2));
    CHECK_FALSE(q.try_push(3));
    int out = 0;
    REQUIRE(q.pop(out) == BoundedQueue<int>::PopStatus::Got);
    CHECK(q.try_push(3));
}

TEST_CASE("close wakes blocked producers and consumers", "[queue]") {
    BoundedQueue<int> q(1);
    REQUIRE(q.push(1)); // fill the queue so the producer below blocks
    std::atomic<bool> producer_released{false};
    std::atomic<bool> push_failed_after_close{false};
    std::thread producer([&] {
        if (!q.push(99))
            push_failed_after_close.store(true);
        producer_released.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(q.size() == 1);
    q.close();
    producer.join();
    CHECK(producer_released.load());
    CHECK(push_failed_after_close.load());

    int out = 0;
    CHECK(q.pop(out) == BoundedQueue<int>::PopStatus::Got);
    CHECK(out == 1);
    CHECK(q.pop(out) == BoundedQueue<int>::PopStatus::Closed);
}

TEST_CASE("pop timeout", "[queue]") {
    BoundedQueue<int> q(4);
    int out = 0;
    CHECK(q.pop(out, std::chrono::milliseconds(20)) == BoundedQueue<int>::PopStatus::Timeout);
}

TEST_CASE("multi producer multi consumer stays consistent", "[queue]") {
    BoundedQueue<int> q(16);
    constexpr int kPerProducer = 500;
    constexpr int kProducers = 4;
    std::atomic<long long> sum{0};
    std::atomic<int> received{0};
    std::atomic<bool> push_failure{false};
    std::vector<std::thread> threads;
    threads.reserve(kProducers + 3);

    for (int p = 0; p < kProducers; ++p) {
        threads.emplace_back([&q, p, &push_failure] {
            for (int i = 0; i < kPerProducer; ++i) {
                if (!q.push(p * kPerProducer + i + 1))
                    push_failure.store(true);
            }
        });
    }
    for (int c = 0; c < 3; ++c) {
        threads.emplace_back([&] {
            int out = 0;
            while (received.load() < kPerProducer * kProducers) {
                auto st = q.pop(out, std::chrono::milliseconds(500));
                if (st == BoundedQueue<int>::PopStatus::Got) {
                    sum += out;
                    ++received;
                } else if (st == BoundedQueue<int>::PopStatus::Closed) {
                    break;
                }
            }
        });
    }
    for (auto& t : threads)
        t.join();
    q.close();
    CHECK_FALSE(push_failure.load());
    CHECK(received.load() == kPerProducer * kProducers);
    long long expected = static_cast<long long>(kProducers) * kPerProducer * (kPerProducer * kProducers + 1) / 2;
    CHECK(sum.load() == expected);
}
