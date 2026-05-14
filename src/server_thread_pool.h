#pragma once

#include <httplib.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <list>
#include <mutex>
#include <thread>
#include <vector>

class ServerThreadPool final : public httplib::TaskQueue {
public:
    static constexpr size_t kDefaultThreadCount = 8;

    explicit ServerThreadPool(size_t configured_size);

    static size_t currentWorkerCount();

    bool enqueue(std::function<void()> fn) override;
    void shutdown() override;

private:
    void start_worker_locked(bool dynamic_worker);
    void worker(bool dynamic_worker);
    void move_dynamic_to_finished_locked(std::thread::id id);
    void cleanup_finished_threads_locked();

    const size_t base_thread_count_;
    const size_t max_thread_count_;
    const bool unbounded_;

    size_t idle_thread_count_ = 0;
    bool shutdown_ = false;

    std::list<std::function<void()>> jobs_;
    std::vector<std::thread> threads_;
    std::list<std::thread> dynamic_threads_;
    std::vector<std::thread> finished_threads_;

    std::condition_variable cond_;
    std::mutex mutex_;
};