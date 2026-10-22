#pragma once
#include <condition_variable>
#include <deque>
#include <mutex>

// Bounded blocking queue between decoder thread and consumer.
template <typename T>
class FrameQueue {
public:
    explicit FrameQueue(size_t cap) : cap_(cap) {}

    bool push(T v) {
        std::unique_lock lk(m_);
        cvNotFull_.wait(lk, [&] { return aborted_ || q_.size() < cap_; });
        if (aborted_) return false;
        q_.push_back(std::move(v));
        cvNotEmpty_.notify_one();
        return true;
    }

    bool pop(T& out) {
        std::unique_lock lk(m_);
        cvNotEmpty_.wait(lk, [&] { return aborted_ || !q_.empty(); });
        if (q_.empty()) return false;
        out = std::move(q_.front());
        q_.pop_front();
        cvNotFull_.notify_one();
        return true;
    }

    bool tryPop(T& out) {
        std::lock_guard lk(m_);
        if (q_.empty()) return false;
        out = std::move(q_.front());
        q_.pop_front();
        cvNotFull_.notify_one();
        return true;
    }

    bool peek(T& out) const {
        std::lock_guard lk(m_);
        if (q_.empty()) return false;
        out = q_.front();
        return true;
    }

    size_t size() const {
        std::lock_guard lk(m_);
        return q_.size();
    }

    size_t capacity() const { return cap_; }

    void flush() {
        std::lock_guard lk(m_);
        q_.clear();
        cvNotFull_.notify_all();
    }

    void abort() {
        std::lock_guard lk(m_);
        aborted_ = true;
        cvNotFull_.notify_all();
        cvNotEmpty_.notify_all();
    }

    void reset() {
        std::lock_guard lk(m_);
        aborted_ = false;
    }

private:
    mutable std::mutex m_;
    std::condition_variable cvNotFull_, cvNotEmpty_;
    std::deque<T> q_;
    size_t cap_;
    bool aborted_ = false;
};
