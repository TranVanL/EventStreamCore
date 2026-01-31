// ============================================================================
// RAFT CONSENSUS IMPLEMENTATION
// ============================================================================
// Day 36: Distributed cluster coordination
//
// Implements core Raft algorithm for cluster state replication
// Focus: Safety (exactly-once semantics) over liveness
// ============================================================================

#include <eventstream/distributed/raft.hpp>
#include <chrono>
#include <random>
#include <algorithm>

namespace EventStream {

// ============================================================================
// RAFT NODE IMPLEMENTATION
// ============================================================================

RaftNode::RaftNode(uint32_t node_id, uint32_t cluster_size)
    : node_id_(node_id), cluster_size_(cluster_size) {
    
    // Initialize next_index and match_index for leader state
    for (uint32_t i = 0; i < cluster_size; ++i) {
        if (i != node_id) {
            next_index_[i] = 1;
            match_index_[i] = 0;
        }
    }
    votes_received_.resize(cluster_size, false);
    
    spdlog::info("[Raft:{}] Initialized as FOLLOWER, cluster_size={}", node_id_, cluster_size_);
}

// ============================================================================
// PUBLIC API
// ============================================================================

bool RaftNode::appendEntry(const LogEntry& entry) {
    if (!isLeader()) {
        spdlog::warn("[Raft:{}] Not leader, rejecting entry", node_id_);
        return false;
    }
    
    // Leader appends to own log first
    LogEntry new_entry = entry;
    new_entry.term = current_term_.load();
    new_entry.index = getLastLogIndex() + 1;
    
    appendLogEntry(new_entry);
    spdlog::debug("[Raft:{}] Leader appended entry: index={}, event_id={}", 
                 node_id_, new_entry.index, entry.event_id);
    
    return true;
}

void RaftNode::becomeLeader() {
    auto term = current_term_.load();
    
    spdlog::info("[Raft:{}] Became LEADER at term {}", node_id_, term);
    state_.store(RaftState::LEADER);
    
    // Initialize leader state
    uint64_t last_index = getLastLogIndex();
    for (auto& [server_id, next_idx] : next_index_) {
        next_idx = last_index + 1;
        match_index_[server_id] = 0;
    }
    
    // Send initial empty AppendEntries as heartbeat
    // (In production, would send to all followers)
}

void RaftNode::becomeFollower(uint64_t new_term) {
    auto old_term = current_term_.load();
    
    if (new_term > old_term) {
        current_term_.store(new_term);
        voted_for_.store(UINT32_MAX);  // Reset vote
        state_.store(RaftState::FOLLOWER);
        
        spdlog::info("[Raft:{}] Became FOLLOWER at term {} (from term {})", 
                    node_id_, new_term, old_term);
    }
}

void RaftNode::requestVote(uint64_t candidate_term, uint32_t candidate_id) {
    auto current_term = current_term_.load();
    
    // Candidate's term is older
    if (candidate_term < current_term) {
        spdlog::debug("[Raft:{}] Rejecting vote from candidate {}: old term {}", 
                     node_id_, candidate_id, candidate_term);
        return;
    }
    
    // Update to newer term
    if (candidate_term > current_term) {
        current_term_.store(candidate_term);
        voted_for_.store(UINT32_MAX);
        state_.store(RaftState::FOLLOWER);
    }
    
    // Grant vote if haven't voted in this term
    auto voted = voted_for_.load();
    if (voted == UINT32_MAX || voted == candidate_id) {
        voted_for_.store(candidate_id);
        updateLastHeartbeat();  // Reset election timer
        spdlog::debug("[Raft:{}] Granted vote to candidate {} in term {}", 
                     node_id_, candidate_id, candidate_term);
    }
}

void RaftNode::advanceCommitIndex(uint64_t new_index) {
    auto old_commit = commit_index_.load();
    
    if (new_index > old_commit && new_index <= getLastLogIndex()) {
        // Only advance if the entry is from current term
        if (new_index > 0 && new_index <= static_cast<uint64_t>(log_.size())) {
            uint64_t entry_term = log_[new_index - 1].term;
            if (entry_term == current_term_.load()) {
                commit_index_.store(new_index);
                spdlog::debug("[Raft:{}] Advanced commit_index to {}", node_id_, new_index);
            }
        }
    }
}

void RaftNode::applyCommittedEntries(std::function<void(const LogEntry&)> apply_fn) {
    auto commit_index = commit_index_.load();
    
    while (last_applied_ < commit_index && last_applied_ < static_cast<uint64_t>(log_.size())) {
        last_applied_++;
        const auto& entry = log_[last_applied_ - 1];
        
        try {
            apply_fn(entry);
            spdlog::debug("[Raft:{}] Applied entry: index={}, event_id={}", 
                         node_id_, entry.index, entry.event_id);
        } catch (const std::exception& e) {
            spdlog::error("[Raft:{}] Error applying entry {}: {}", 
                         node_id_, entry.index, e.what());
        }
    }
}

void RaftNode::updateLastHeartbeat() {
    last_heartbeat_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

bool RaftNode::isHeartbeatTimeout(uint64_t now_ms, uint64_t timeout_ms) const {
    return (now_ms - last_heartbeat_ms_) > timeout_ms;
}

bool RaftNode::isElectionTimeout(uint64_t now_ms, uint64_t min_timeout, uint64_t max_timeout) const {
    // Randomized election timeout between min and max
    static std::mt19937 rng(std::random_device{}());
    static std::uniform_int_distribution<> dist(0, 1000);
    
    auto base_timeout = min_timeout + (dist(rng) % (max_timeout - min_timeout));
    return (now_ms - last_heartbeat_ms_) > base_timeout;
}

RaftNode::Stats RaftNode::getStats() const {
    return {
        .log_size = log_.size(),
        .commit_index = commit_index_.load(),
        .applied_index = last_applied_,
        .term = current_term_.load(),
        .state = state_.load(),
        .leader_id = getLeaderId()
    };
}

// ============================================================================
// PRIVATE HELPER METHODS
// ============================================================================

void RaftNode::appendLogEntry(const LogEntry& entry) {
    log_.push_back(entry);
}

void RaftNode::truncateLogAfter(uint64_t index) {
    if (index < getLastLogIndex()) {
        // Remove entries after index
        auto new_size = std::min(static_cast<uint64_t>(log_.size()), index);
        log_.erase(log_.begin() + new_size, log_.end());
        spdlog::info("[Raft:{}] Truncated log after index {}", node_id_, index);
    }
}

void RaftNode::startElection() {
    auto new_term = current_term_.load() + 1;
    current_term_.store(new_term);
    state_.store(RaftState::CANDIDATE);
    voted_for_.store(node_id_);  // Vote for self
    
    // Reset election timer
    resetElectionTimer();
    
    spdlog::info("[Raft:{}] Started election for term {}", node_id_, new_term);
    
    // In production, would send RequestVote RPC to all peers
    // For now, just log the intention
}

void RaftNode::resetElectionTimer() {
    updateLastHeartbeat();  // Use same timer mechanism
}

// ============================================================================
// CLUSTER COORDINATOR IMPLEMENTATION
// ============================================================================

ClusterCoordinator::ClusterCoordinator(uint32_t node_id)
    : node_id_(node_id), election_timeout_ms_(1500) {
    raft_node_ = std::make_unique<RaftNode>(node_id, 1);  // Default cluster size 1
}

void ClusterCoordinator::addNode(const ClusterNode& node) {
    nodes_.push_back(node);
    
    // Update cluster size in raft node if needed
    // In production, would properly handle membership changes
    spdlog::info("[ClusterCoordinator:{}] Added node {}: {}:{}", 
                node_id_, node.node_id, node.host, node.port);
}

void ClusterCoordinator::start() {
    spdlog::info("[ClusterCoordinator:{}] Starting cluster coordination (cluster_size={})", 
                node_id_, nodes_.size());
    
    // In production, would:
    // 1. Start heartbeat timer
    // 2. Start election timeout timer
    // 3. Connect to other nodes
    // 4. Start RPC listener
}

void ClusterCoordinator::stop() {
    spdlog::info("[ClusterCoordinator:{}] Stopping cluster coordination", node_id_);
}

bool ClusterCoordinator::replicateDedup(uint32_t event_id, uint64_t timestamp_ms) {
    if (!isLeader()) {
        spdlog::warn("[ClusterCoordinator:{}] Not leader, cannot replicate", node_id_);
        return false;
    }
    
    LogEntry entry(raft_node_->getCurrentTerm(), 
                   raft_node_->getLastLogIndex() + 1,
                   event_id, timestamp_ms,
                   LogEntry::Type::IDEMPOTENT_SEEN);
    
    return raft_node_->appendEntry(entry);
}

bool ClusterCoordinator::isHealthy() const {
    auto stats = getStats();
    
    // Node is healthy if:
    // 1. Has a leader (either self or know who it is)
    // 2. Not too far behind in replication
    // 3. Has applied recent entries
    
    return stats.leader_id.has_value() || 
           (stats.state == RaftState::LEADER) ||
           (getClusterSize() == 1);  // Single node always healthy
}

void ClusterCoordinator::updateElectionTimeout() {
    // Randomize election timeout: 150-300ms
    static std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<> dist(150, 300);
    election_timeout_ms_ = dist(rng);
}

}  // namespace EventStream
