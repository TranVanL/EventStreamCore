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

    // Submit a task to the thread pool
    void submit(std::function<void()> task);
    
    // Get number of pending tasks
    size_t getPendingTasks() const;

    // Stop all threads and clear the task queue
    void shutdown();
private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    mutable std::mutex queueMutex;  // mutable for const getPendingTasks
    std::condition_variable condition;
    std::atomic<bool> isRunning;
};