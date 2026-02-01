#pragma once

#include <atomic>
#include <optional>
#include <memory>
#include <cstddef>

template<typename T, size_t Capacity = 65536>
class MpscQueue {
public:
    MpscQueue() : size_(0) {
        // Initialize with a dummy node (simplifies push/pop logic)
        Node* dummy = new Node();
        head_.store(dummy, std::memory_order_relaxed);
        tail_.store(dummy, std::memory_order_relaxed);
    }
    
    ~MpscQueue() {
        // Drain remaining nodes
        while (pop().has_value()) {}
        // Delete dummy node
        delete head_.load(std::memory_order_relaxed);
    }
    
    // Non-copyable, non-movable
    MpscQueue(const MpscQueue&) = delete;
    MpscQueue& operator=(const MpscQueue&) = delete;
    MpscQueue(MpscQueue&&) = delete;
    MpscQueue& operator=(MpscQueue&&) = delete;
    
    /**
     * @brief Push item to queue (thread-safe for multiple producers)
     * @param item Item to push
     * @return true if successful, false if queue is at capacity
     */
    bool push(const T& item) {
        // Check capacity (approximate - may slightly exceed due to race)
        if (size_.load(std::memory_order_relaxed) >= Capacity) {
            return false;
        }
        
        Node* node = new Node(item);
        
        // Lock-free push using exchange on tail
        // This is the Vyukov MPSC queue algorithm
        Node* prev = tail_.exchange(node, std::memory_order_acq_rel);
        
        // Link previous tail to new node
        // This store must happen after exchange (release ordering ensures visibility)
        prev->next.store(node, std::memory_order_release);
        
        size_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    
    /**
     * @brief Pop item from queue (single consumer only)
     * @return Item if available, std::nullopt if empty
     * 
     * IMPORTANT: This uses Vyukov's MPSC algorithm. The consumer must
     * wait for the producer to complete linking (next != nullptr).
     */
    std::optional<T> pop() {
        Node* head = head_.load(std::memory_order_relaxed);
        Node* next = head->next.load(std::memory_order_acquire);
        
        if (next == nullptr) {
            // Queue is empty (only dummy node) OR producer hasn't finished linking yet
            // For Vyukov MPSC, if tail != head but next == nullptr, we should spin-wait
            // But for simplicity, we return nullopt (caller should retry)
            return std::nullopt;
        }
        
        // Move head to next node (next becomes new dummy)
        T item = std::move(next->data);
        head_.store(next, std::memory_order_relaxed);  // No need for release here
        
        // Delete old dummy node
        delete head;
        
        size_.fetch_sub(1, std::memory_order_relaxed);
        return item;
    }
    
    /**
     * @brief Get approximate queue size
     * @return Approximate number of items in queue
     */
    size_t size() const {
        return size_.load(std::memory_order_relaxed);
    }
    
    /**
     * @brief Check if queue is empty
     * @return true if empty
     */
    bool empty() const {
        Node* head = head_.load(std::memory_order_relaxed);
        return head->next.load(std::memory_order_acquire) == nullptr;
    }

private:
    struct Node {
        T data;
        std::atomic<Node*> next{nullptr};
        
        Node() = default;
        explicit Node(const T& item) : data(item) {}
    };
    
    // Cache line padding to prevent false sharing
    alignas(64) std::atomic<Node*> head_;  // Consumer reads from head
    alignas(64) std::atomic<Node*> tail_;  // Producers write to tail
    alignas(64) std::atomic<size_t> size_; // Approximate size for capacity check
};
