// ============================================================================
// DISTRIBUTED CONSENSUS - RAFT PROTOCOL
// ============================================================================
// Day 36: Distributed cluster coordination for EventStreamCore
//
// Raft Consensus Algorithm:
// - Strong leader model: exactly one leader at a time
// - Log replication: all writes go through leader
// - State machine: applied in order to all nodes
// - Safety: if it's committed, all future leaders have it
//
// For EventStreamCore:
// - Replicate dedup state across cluster
// - Ensure at-least-once idempotency guarantees
// - Handle node failures & network partitions
// - Support dynamic cluster membership changes
// ============================================================================

#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <memory>
#include <atomic>
#include <map>
#include <deque>
#include <optional>
#include <spdlog/spdlog.h>

namespace EventStream {

// ============================================================================
// CLUSTER NODE DEFINITIONS
// ============================================================================

struct ClusterNode {
    uint32_t node_id;
    std::string host;
    uint16_t port;
    
    ClusterNode(uint32_t id, const std::string& h, uint16_t p)
        : node_id(id), host(h), port(p) {}
};

// ============================================================================
// LOG ENTRY FOR REPLICATED STATE
// ============================================================================

struct LogEntry {
    uint64_t term;                  // Term when entry was received by leader
    uint64_t index;                 // Position in log
    
    // Payload: replicated dedup operation
    uint32_t event_id;              // Which event is this about?
    uint64_t timestamp_ms;          // When did event occur?
    
    enum Type : uint8_t {
        IDEMPOTENT_SEEN = 1,        // Mark event as seen (for dedup)
        CHECKPOINT = 2,             // Snapshot of dedup state
        CONFIG_CHANGE = 3           // Membership change
    } type;
    
    LogEntry(uint64_t t, uint64_t i, uint32_t eid, uint64_t ts, Type ty)
        : term(t), index(i), event_id(eid), timestamp_ms(ts), type(ty) {}
};

// ============================================================================
// RAFT CONSENSUS STATE
// ============================================================================

enum class RaftState {
    FOLLOWER = 0,       // Receiving RPCs from leader or candidate
    CANDIDATE = 1,      // Competing to become leader
    LEADER = 2          // Elected leader, sending heartbeats
};

class RaftNode {
public:
    // ========================================================================
    // PUBLIC API
    // ========================================================================
    
    explicit RaftNode(uint32_t node_id, uint32_t cluster_size);
    ~RaftNode() = default;
    
    // Node info
    uint32_t getNodeId() const { return node_id_; }
    RaftState getState() const { return state_.load(); }
    uint64_t getCurrentTerm() const { return current_term_.load(); }
    std::optional<uint32_t> getLeaderId() const {
        auto leader = voted_for_.load();
        return (leader == UINT32_MAX) ? std::nullopt : std::make_optional(leader);
    }
    
    // Replication
    bool appendEntry(const LogEntry& entry);
    bool isLeader() const { return getState() == RaftState::LEADER; }
    
    // Leadership
    void becomeLeader();
    void becomeFollower(uint64_t new_term);
    void requestVote(uint64_t candidate_term, uint32_t candidate_id);
    
    // Log access
    const std::deque<LogEntry>& getLog() const { return log_; }
    uint64_t getLastLogIndex() const { return log_.empty() ? 0 : log_.back().index; }
    uint64_t getLastLogTerm() const { return log_.empty() ? 0 : log_.back().term; }
    uint64_t getCommitIndex() const { return commit_index_.load(); }
    
    // Commit & apply
    void advanceCommitIndex(uint64_t new_index);
    void applyCommittedEntries(std::function<void(const LogEntry&)> apply_fn);
    
    // Heartbeat & RPC timing
    void updateLastHeartbeat();
    bool isHeartbeatTimeout(uint64_t now_ms, uint64_t timeout_ms = 3000) const;
    bool isElectionTimeout(uint64_t now_ms, uint64_t min_timeout = 1500, uint64_t max_timeout = 3000) const;
    
    // Statistics
    struct Stats {
        uint64_t log_size;
        uint64_t commit_index;
        uint64_t applied_index;
        uint64_t term;
        RaftState state;
        std::optional<uint32_t> leader_id;
    };
    Stats getStats() const;
    
private:
    // ========================================================================
    // PERSISTENT STATE (on all servers)
    // ========================================================================
    uint32_t node_id_;
    std::atomic<uint64_t> current_term_{0};     // Latest term known to server
    std::atomic<uint32_t> voted_for_{UINT32_MAX}; // CandidateId that received vote in current term
    std::deque<LogEntry> log_;                  // Log entries; each entry contains command for state machine
    
    // ========================================================================
    // VOLATILE STATE (on all servers)
    // ========================================================================
    std::atomic<RaftState> state_{RaftState::FOLLOWER};
    std::atomic<uint64_t> commit_index_{0};     // Index of highest log entry known to be committed
    uint64_t last_applied_{0};                  // Index of highest log entry applied to state machine
    uint64_t last_heartbeat_ms_{0};             // Timestamp of last heartbeat received
    
    // ========================================================================
    // VOLATILE STATE (on leaders)
    // ========================================================================
    uint32_t cluster_size_;
    std::map<uint32_t, uint64_t> next_index_;   // For each server, index of next log entry to send
    std::map<uint32_t, uint64_t> match_index_;  // For each server, index of highest log entry known to be replicated
    std::vector<bool> votes_received_;          // For leader election
    
    // ========================================================================
    // HELPER METHODS
    // ========================================================================
    
    // Election
    void startElection();
    void resetElectionTimer();
    
    // Log management
    void appendLogEntry(const LogEntry& entry);
    void truncateLogAfter(uint64_t index);
};

// ============================================================================
// CLUSTER COORDINATOR
// ============================================================================

class ClusterCoordinator {
public:
    explicit ClusterCoordinator(uint32_t node_id);
    
    // Cluster setup
    void addNode(const ClusterNode& node);
    void start();
    void stop();
    
    // Leadership queries
    bool isLeader() const { return raft_node_->isLeader(); }
    std::optional<uint32_t> getLeader() const { return raft_node_->getLeaderId(); }
    
    // Replication
    bool replicateDedup(uint32_t event_id, uint64_t timestamp_ms);
    
    // Statistics
    RaftNode::Stats getStats() const { return raft_node_->getStats(); }
    uint32_t getClusterSize() const { return nodes_.size(); }
    uint32_t getNodeId() const { return node_id_; }
    
    // Monitoring
    bool isHealthy() const;
    
private:
    uint32_t node_id_;
    std::vector<ClusterNode> nodes_;
    std::unique_ptr<RaftNode> raft_node_;
    
    // Election timeout randomization
    uint64_t election_timeout_ms_;
    
    void updateElectionTimeout();
};

}  // namespace EventStream
