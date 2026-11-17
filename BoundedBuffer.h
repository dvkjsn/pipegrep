#pragma once
#include <queue>
#include <mutex>
#include <condition_variable>

// thread-safe bounded fifo queue connecting one producer thread to one consumer thread.
// push() blocks while the queue is full; pop() blocks while it is empty and not closed.
template <typename T>
class BoundedBuffer {
    std::queue<T> q_;                    // stored records in fifo order
    size_t cap_;                         // maximum number of records (buffsize)
    bool closed_ = false;                // set once the producer will send no more records
    std::mutex m_;                       // guards q_ and closed_
    std::condition_variable not_full_;   // signaled when space becomes available
    std::condition_variable not_empty_;  // signaled when a record or close arrives

public:
    explicit BoundedBuffer(size_t cap) : cap_(cap) {}

    // appends a record, waiting until the queue has room.
    // the predicate is rechecked in a loop after every wakeup.
    void push(T x) {
        std::unique_lock<std::mutex> lk(m_);
        while (q_.size() >= cap_) not_full_.wait(lk);
        q_.push(std::move(x));
        not_empty_.notify_one();
    }

    // removes the oldest record into out and returns true.
    // returns false only when the queue is closed and fully drained.
    bool pop(T& out) {
        std::unique_lock<std::mutex> lk(m_);
        while (q_.empty() && !closed_) not_empty_.wait(lk);
        if (q_.empty()) return false;
        out = std::move(q_.front());
        q_.pop();
        not_full_.notify_one();
        return true;
    }

    // marks the queue as finished and wakes a waiting consumer.
    // records already queued are still delivered by pop().
    void close() {
        std::lock_guard<std::mutex> lk(m_);
        closed_ = true;
        not_empty_.notify_all();
    }
};