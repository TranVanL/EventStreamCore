#pragma once
#include <vector>
#include <thread>
#include <atomic>
#include <queue>
#include <functional>
#include <condition_variable>
#include <mutex>

class ThreadPool {
public:
    ThreadPool(size_t numThreads);
    ~ThreadPool();

    void submit(std::function<void()> task);
    size_t getPendingTasks() const;
    void shutdown();
private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    mutable std::mutex queueMutex;  // mutable for const getPendingTasks
    std::condition_variable condition;
    std::atomic<bool> isRunning;
};