#pragma once

// Fixed-size worker pool with a bounded queue.
//
// The bound is the backpressure mechanism: when every worker is busy and max_queue tasks
// are already waiting, try_submit() returns false immediately instead of letting the
// queue (and memory) grow without limit. The server answers such clients "ERR busy".

#include "tcpsync/log.hpp"

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace tcpsync {

class ThreadPool {
public:
    using Task = std::function<void()>;

    ThreadPool(size_t workers, size_t max_queue) : max_queue_(max_queue) {
        if (workers == 0) workers = 1;
        workers_.reserve(workers);
        for (size_t i = 0; i < workers; ++i) workers_.emplace_back([this] { worker_loop(); });
    }

    ~ThreadPool() { shutdown(); }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Never blocks. False when the queue is full or the pool is shutting down.
    bool try_submit(Task task) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (stopping_ || queue_.size() >= max_queue_) return false;
            queue_.push(std::move(task));
        }
        cv_.notify_one();  // notify after unlocking so the woken worker doesn't block on mu_
        return true;
    }

    // Stops accepting tasks, lets workers finish everything already queued, joins them.
    // Safe to call more than once and from more than one thread.
    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            stopping_ = true;
        }
        cv_.notify_all();
        std::call_once(joined_, [this] {
            for (std::thread& t : workers_) t.join();
        });
    }

    size_t worker_count() const noexcept { return workers_.size(); }

private:
    void worker_loop() {
        for (;;) {
            Task task;
            {
                std::unique_lock<std::mutex> lock(mu_);
                // The predicate guards against spurious wakeups and lost notifications.
                cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
                if (queue_.empty()) return;  // stopping and fully drained
                task = std::move(queue_.front());
                queue_.pop();
            }
            // Run outside the lock. An exception escaping a std::thread would call
            // std::terminate and take the whole server down, so contain it here.
            try {
                task();
            } catch (const std::exception& e) {
                log_error("worker task threw: ", e.what());
            } catch (...) {
                log_error("worker task threw a non-std exception");
            }
        }
    }

    const size_t max_queue_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::queue<Task> queue_;
    bool stopping_ = false;
    std::vector<std::thread> workers_;
    std::once_flag joined_;
};

}  // namespace tcpsync
