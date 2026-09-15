#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <utility>

namespace streamforge {

// Thread-safe bounded blocking queue used between pipeline stages (requirement 8.1).
// push() blocks while the queue is full; close() wakes all waiting threads and is idempotent.
template <typename T> class BoundedQueue {
public:
    explicit BoundedQueue(size_t capacity) : capacity_(capacity) {}

    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;

    // Blocks while full. Returns false if the queue was closed.
    bool push(T value) {
        std::unique_lock<std::mutex> lock(mu_);
        cv_not_full_.wait(lock, [this] { return closed_ || q_.size() < capacity_; });
        if (closed_)
            return false;
        q_.push_back(std::move(value));
        lock.unlock();
        cv_not_empty_.notify_one();
        return true;
    }

    // Non-blocking push. Returns false when full or closed.
    bool try_push(T value) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (closed_ || q_.size() >= capacity_)
                return false;
            q_.push_back(std::move(value));
        }
        cv_not_empty_.notify_one();
        return true;
    }

    enum class PopStatus { Got, Closed, Timeout };

    // Blocks until an element is available, the queue closes (and drains), or the timeout
    // elapses. Returns Closed only when the queue is closed AND empty.
    PopStatus pop(T& out, std::chrono::milliseconds timeout = std::chrono::milliseconds{-1}) {
        std::unique_lock<std::mutex> lock(mu_);
        auto wait_ok = [&] {
            if (timeout.count() < 0) {
                cv_not_empty_.wait(lock, [this] { return !q_.empty() || closed_; });
                return true;
            }
            return cv_not_empty_.wait_for(lock, timeout, [this] { return !q_.empty() || closed_; });
        };
        if (!wait_ok())
            return PopStatus::Timeout;
        if (q_.empty())
            return PopStatus::Closed; // closed and drained
        out = std::move(q_.front());
        q_.pop_front();
        lock.unlock();
        cv_not_full_.notify_one();
        return PopStatus::Got;
    }

    void close() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            closed_ = true;
        }
        cv_not_full_.notify_all();
        cv_not_empty_.notify_all();
    }

    bool closed() const {
        std::lock_guard<std::mutex> lock(mu_);
        return closed_;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mu_);
        return q_.size();
    }

    size_t capacity() const { return capacity_; }

private:
    mutable std::mutex mu_;
    std::condition_variable cv_not_full_;
    std::condition_variable cv_not_empty_;
    std::deque<T> q_;
    size_t capacity_;
    bool closed_ = false;
};

} // namespace streamforge
