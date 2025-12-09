#include "utils/thread_pool.hpp"

ThreadPool::ThreadPool(size_t numThreads) : isRunning(true) {
    for (size_t i = 0; i < numThreads; ++i) {
        workers.emplace_back([this] {
            while (isRunning.load(std::memory_order_acquire)) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(queueMutex);
                    condition.wait(lock, [this] { return !tasks.empty() || !isRunning.load(); });
                    if (!isRunning.load() && tasks.empty()) return;
                    task = std::move(tasks.front());
                    tasks.pop();
                }
                task();
            }
        });
    }
}

ThreadPool::~ThreadPool() {
    shutdown();
}
void ThreadPool::submit(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        tasks.push(std::move(task));
    }
    condition.notify_one();
}

void ThreadPool::shutdown() {
    isRunning.store(false, std::memory_order_release);
    condition.notify_all();
    for (std::thread &worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    // Clear remaining tasks
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        while (!tasks.empty()) {
            tasks.pop();
        }
    }
}