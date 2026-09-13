// Fixed-size worker pool for the precache. Tasks may submit further tasks;
// wait() returns when every submitted task (including nested ones) finished.
#pragma once
#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace bf6::cache {

class ThreadPool {
public:
    explicit ThreadPool(std::size_t threads = 0)
    {
        if (threads == 0) threads = std::max<std::size_t>(1, std::thread::hardware_concurrency());
        for (std::size_t i = 0; i < threads; ++i)
            workers_.emplace_back([this] { run(); });
    }
    ~ThreadPool()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_all();
        for (std::thread& t : workers_) t.join();
    }
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    std::size_t size() const { return workers_.size(); }

    void submit(std::function<void()> task)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(std::move(task));
            ++outstanding_;
        }
        wake_.notify_one();
    }

    void wait()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        idle_.wait(lock, [this] { return outstanding_ == 0; });
    }

private:
    void run()
    {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
                if (stopping_ && queue_.empty()) return;
                task = std::move(queue_.front());
                queue_.pop_front();
            }
            try { task(); } catch (...) {}
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (--outstanding_ == 0) idle_.notify_all();
            }
        }
    }

    std::vector<std::thread> workers_;
    std::deque<std::function<void()>> queue_;
    std::mutex mutex_;
    std::condition_variable wake_, idle_;
    std::size_t outstanding_ = 0;
    bool stopping_ = false;
};

}  // namespace bf6::cache
