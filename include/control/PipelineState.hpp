#pragma once
#include <atomic>
#include <cstdint>
#include <spdlog/spdlog.h>

/**
 * Pipeline State Machine - NGÔN NGỮ CHUNG giữa Admin (quyết định) và Workers (thực hiện)
 * 
 * AdminLoop là "não" - quyền duy nhất thay đổi state
 * Workers là "cơ thể" - chỉ đọc state, không thay đổi
 */
enum class PipelineState : uint8_t {
    RUNNING = 0,      // Bình thường - ingest, process bình thường
    PAUSED = 1,       // Worker ngừng consume từ queue, backlog tích tụ
    DRAINING = 2,     // Ngừng ingest mới, worker xả hết backlog
    DROPPING = 3,     // Drop batch events có kiểm soát
    EMERGENCY = 4     // Push tất cả failed events tới DLQ
};

/**
 * Quản lý state pipeline - thread-safe, read-heavy
 */
class PipelineStateManager {
public:
    PipelineStateManager();
    ~PipelineStateManager() = default;

    /**
     * Chỉ AdminLoop gọi hàm này
     * Other: không có quyền thay đổi state
     */
    void setState(PipelineState newState);

    /**
     * Worker/Dispatcher gọi để đọc state hiện tại
     * Non-blocking, memory_order_acquire
     */
    PipelineState getState() const;

    /**
     * Worker check trước khi consume/process event
     */
    bool isRunning() const {
        return getState() == PipelineState::RUNNING;
    }

    bool isPaused() const {
        return getState() == PipelineState::PAUSED;
    }

    bool isDraining() const {
        return getState() == PipelineState::DRAINING;
    }

    bool isDropping() const {
        return getState() == PipelineState::DROPPING;
    }

    bool isEmergency() const {
        return getState() == PipelineState::EMERGENCY;
    }

    /**
     * String representation (cho logging)
     */
    static const char* toString(PipelineState state);

private:
    std::atomic<PipelineState> state_{PipelineState::RUNNING};
};
