#include "server_thread_pool.h"

#include <chrono>

namespace {
constexpr auto kDynamicThreadIdleTimeout = std::chrono::seconds(3);
}

ServerThreadPool::ServerThreadPool(size_t configured_size)
    : base_thread_count_(configured_size == 0 ? kDefaultThreadCount : configured_size),
      max_thread_count_(configured_size),
      unbounded_(configured_size == 0) {
    threads_.reserve(base_thread_count_);
    for (size_t i = 0; i < base_thread_count_; ++i) {
        threads_.emplace_back([this]() { worker(false); });
    }
}

bool ServerThreadPool::enqueue(std::function<void()> fn) {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (shutdown_) {
            return false;
        }

        jobs_.push_back(std::move(fn));

        if (unbounded_ && idle_thread_count_ == 0) {
            cleanup_finished_threads_locked();
            dynamic_threads_.emplace_back([this]() { worker(true); });
        }
    }

    cond_.notify_one();
    return true;
}

void ServerThreadPool::shutdown() {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        shutdown_ = true;
    }

    cond_.notify_all();

    for (auto& thread : threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    std::list<std::thread> remaining_dynamic;
    std::vector<std::thread> finished_threads;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        remaining_dynamic = std::move(dynamic_threads_);
        finished_threads = std::move(finished_threads_);
    }

    for (auto& thread : remaining_dynamic) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    for (auto& thread : finished_threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

void ServerThreadPool::worker(bool dynamic_worker) {
    for (;;) {
        std::function<void()> fn;

        {
            std::unique_lock<std::mutex> lock(mutex_);
            ++idle_thread_count_;

            if (dynamic_worker) {
                const bool has_work = cond_.wait_for(
                    lock,
                    kDynamicThreadIdleTimeout,
                    [&]() { return !jobs_.empty() || shutdown_; });
                if (!has_work) {
                    --idle_thread_count_;
                    move_dynamic_to_finished_locked(std::this_thread::get_id());
                    break;
                }
            } else {
                cond_.wait(lock, [&]() { return !jobs_.empty() || shutdown_; });
            }

            --idle_thread_count_;

            if (shutdown_ && jobs_.empty()) {
                break;
            }

            fn = std::move(jobs_.front());
            jobs_.pop_front();
        }

        if (fn) {
            fn();
        }
    }
}

void ServerThreadPool::move_dynamic_to_finished_locked(std::thread::id id) {
    for (auto it = dynamic_threads_.begin(); it != dynamic_threads_.end(); ++it) {
        if (it->get_id() == id) {
            finished_threads_.push_back(std::move(*it));
            dynamic_threads_.erase(it);
            return;
        }
    }
}

void ServerThreadPool::cleanup_finished_threads_locked() {
    for (auto& thread : finished_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    finished_threads_.clear();
}