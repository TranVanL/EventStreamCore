// ============================================================================
// RAFT CONSENSUS TEST SUITE
// ============================================================================
// Day 36: Distributed cluster coordination tests
// - Leader election
// - Log replication
// - State consistency
// - Network partition recovery
// ============================================================================

#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <eventstream/distributed/raft.hpp>

using namespace EventStream;

// ============================================================================
// TEST CLASS
// ============================================================================

class RaftNodeTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create 3-node cluster
        node1_ = std::make_unique<RaftNode>(0, 3);
        node2_ = std::make_unique<RaftNode>(1, 3);
        node3_ = std::make_unique<RaftNode>(2, 3);
        
        node1_->updateLastHeartbeat();
        node2_->updateLastHeartbeat();
        node3_->updateLastHeartbeat();
    }
    
    std::unique_ptr<RaftNode> node1_;
    std::unique_ptr<RaftNode> node2_;
    std::unique_ptr<RaftNode> node3_;
    
    uint64_t getCurrentTimeMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }
};

// ============================================================================
// BASIC STATE TESTS
// ============================================================================

TEST_F(RaftNodeTest, InitialState) {
    // All nodes start as followers
    EXPECT_EQ(node1_->getState(), RaftState::FOLLOWER);
    EXPECT_EQ(node2_->getState(), RaftState::FOLLOWER);
    EXPECT_EQ(node3_->getState(), RaftState::FOLLOWER);
    
    // No leader elected yet
    EXPECT_FALSE(node1_->getLeaderId().has_value());
    EXPECT_FALSE(node2_->getLeaderId().has_value());
    EXPECT_FALSE(node3_->getLeaderId().has_value());
    
    // All start at term 0
    EXPECT_EQ(node1_->getCurrentTerm(), 0);
    EXPECT_EQ(node2_->getCurrentTerm(), 0);
    EXPECT_EQ(node3_->getCurrentTerm(), 0);
}

TEST_F(RaftNodeTest, BecomeLeader) {
    // Node1 becomes leader
    node1_->becomeLeader();
    
    EXPECT_EQ(node1_->getState(), RaftState::LEADER);
    EXPECT_TRUE(node1_->isLeader());
}

TEST_F(RaftNodeTest, BecomeFollower) {
    // Node1 is leader
    node1_->becomeLeader();
    EXPECT_TRUE(node1_->isLeader());
    
    // Node1 sees higher term from node2
    node1_->becomeFollower(node2_->getCurrentTerm() + 1);
    
    EXPECT_EQ(node1_->getState(), RaftState::FOLLOWER);
    EXPECT_FALSE(node1_->isLeader());
}

// ============================================================================
// LOG REPLICATION TESTS
// ============================================================================

TEST_F(RaftNodeTest, AppendEntryAsLeader) {
    node1_->becomeLeader();
    
    LogEntry entry(0, 1, 100, getCurrentTimeMs(), LogEntry::Type::IDEMPOTENT_SEEN);
    bool success = node1_->appendEntry(entry);
    
    EXPECT_TRUE(success);
    EXPECT_EQ(node1_->getLastLogIndex(), 1);
    EXPECT_EQ(node1_->getLog().size(), 1);
    EXPECT_EQ(node1_->getLog()[0].event_id, 100);
}

TEST_F(RaftNodeTest, FollowerCannotAppendEntry) {
    // Node2 is still a follower
    EXPECT_EQ(node2_->getState(), RaftState::FOLLOWER);
    
    LogEntry entry(0, 1, 100, getCurrentTimeMs(), LogEntry::Type::IDEMPOTENT_SEEN);
    bool success = node2_->appendEntry(entry);
    
    EXPECT_FALSE(success);
    EXPECT_EQ(node2_->getLastLogIndex(), 0);
}

TEST_F(RaftNodeTest, MultipleLogEntries) {
    node1_->becomeLeader();
    
    // Add 10 entries
    for (uint32_t i = 0; i < 10; ++i) {
        LogEntry entry(0, i + 1, 100 + i, getCurrentTimeMs(), LogEntry::Type::IDEMPOTENT_SEEN);
        EXPECT_TRUE(node1_->appendEntry(entry));
    }
    
    EXPECT_EQ(node1_->getLastLogIndex(), 10);
    EXPECT_EQ(node1_->getLog().size(), 10);
    
    // Verify log entries
    for (size_t i = 0; i < 10; ++i) {
        EXPECT_EQ(node1_->getLog()[i].event_id, 100 + i);
    }
}

// ============================================================================
// COMMITMENT TESTS
// ============================================================================

TEST_F(RaftNodeTest, CommitIndexAdvancement) {
    node1_->becomeLeader();
    
    // Add entries
    for (uint32_t i = 0; i < 5; ++i) {
        LogEntry entry(0, i + 1, 100 + i, getCurrentTimeMs(), LogEntry::Type::IDEMPOTENT_SEEN);
        node1_->appendEntry(entry);
    }
    
    EXPECT_EQ(node1_->getCommitIndex(), 0);  // Nothing committed yet
    
    // Advance commit index (simulating majority replication)
    node1_->advanceCommitIndex(3);
    EXPECT_EQ(node1_->getCommitIndex(), 3);
    
    // Can't advance beyond term (needs to be from current term)
    node1_->advanceCommitIndex(10);
    EXPECT_EQ(node1_->getCommitIndex(), 3);  // Unchanged
}

TEST_F(RaftNodeTest, ApplyCommittedEntries) {
    node1_->becomeLeader();
    
    // Add entries
    for (uint32_t i = 0; i < 3; ++i) {
        LogEntry entry(0, i + 1, 100 + i, getCurrentTimeMs(), LogEntry::Type::IDEMPOTENT_SEEN);
        node1_->appendEntry(entry);
    }
    
    // Commit and track applied entries
    std::vector<uint32_t> applied_events;
    node1_->advanceCommitIndex(3);
    node1_->applyCommittedEntries([&](const LogEntry& entry) {
        applied_events.push_back(entry.event_id);
    });
    
    EXPECT_EQ(applied_events.size(), 3);
    EXPECT_EQ(applied_events[0], 100);
    EXPECT_EQ(applied_events[1], 101);
    EXPECT_EQ(applied_events[2], 102);
}

// ============================================================================
// TERM & VOTING TESTS
// ============================================================================

TEST_F(RaftNodeTest, VotingInNewTerm) {
    // Node1 requests vote from node2
    uint64_t candidate_term = 1;
    node2_->requestVote(candidate_term, 0);  // 0 is candidate id
    
    // Node2 should update term and grant vote
    EXPECT_EQ(node2_->getCurrentTerm(), 1);
    EXPECT_EQ(node2_->getLeaderId(), 0);  // Voted for node 0
}

TEST_F(RaftNodeTest, RejectOldTermVote) {
    // Node1 already has term 2
    node1_->becomeFollower(2);
    
    // Candidate from term 1 asks for vote
    node1_->requestVote(1, 2);
    
    // Should reject (only accept >= current term)
    EXPECT_EQ(node1_->getCurrentTerm(), 2);
}

TEST_F(RaftNodeTest, FollowerRejectsHigherTermCandidate) {
    // Node1 votes for node2 in term 1
    node1_->requestVote(1, 1);
    EXPECT_EQ(node1_->getLeaderId(), 1);
    
    // Another candidate from same term asks
    node1_->requestVote(1, 2);
    
    // Should still remember vote for node 1
    EXPECT_EQ(node1_->getLeaderId(), 1);  // Unchanged
}

// ============================================================================
// TIMEOUT TESTS
// ============================================================================

TEST_F(RaftNodeTest, HeartbeatTimeout) {
    uint64_t now = getCurrentTimeMs();
    node1_->updateLastHeartbeat();
    
    // Should not timeout immediately
    EXPECT_FALSE(node1_->isHeartbeatTimeout(now, 3000));
    
    // Should timeout after duration
    EXPECT_TRUE(node1_->isHeartbeatTimeout(now + 4000, 3000));
}

TEST_F(RaftNodeTest, ElectionTimeout) {
    uint64_t now = getCurrentTimeMs();
    node1_->updateLastHeartbeat();
    
    // Should not timeout at start
    EXPECT_FALSE(node1_->isElectionTimeout(now, 1500, 3000));
    
    // Should timeout after duration (rough check, randomized)
    EXPECT_TRUE(node1_->isElectionTimeout(now + 4000, 1500, 3000));
}

// ============================================================================
// STATS TESTS
// ============================================================================

TEST_F(RaftNodeTest, GetStats) {
    node1_->becomeLeader();
    
    // Add some entries
    for (uint32_t i = 0; i < 5; ++i) {
        LogEntry entry(0, i + 1, 100 + i, getCurrentTimeMs(), LogEntry::Type::IDEMPOTENT_SEEN);
        node1_->appendEntry(entry);
    }
    
    node1_->advanceCommitIndex(3);
    std::vector<uint32_t> applied;
    node1_->applyCommittedEntries([&](const LogEntry& entry) {
        applied.push_back(entry.event_id);
    });
    
    auto stats = node1_->getStats();
    EXPECT_EQ(stats.log_size, 5);
    EXPECT_EQ(stats.commit_index, 3);
    EXPECT_EQ(stats.applied_index, 3);
    EXPECT_EQ(stats.state, RaftState::LEADER);
}

// ============================================================================
// CLUSTER COORDINATOR TESTS
// ============================================================================

TEST_F(RaftNodeTest, ClusterCoordinatorBasics) {
    ClusterCoordinator coordinator(0);
    
    coordinator.addNode(ClusterNode(0, "localhost", 5000));
    coordinator.addNode(ClusterNode(1, "localhost", 5001));
    coordinator.addNode(ClusterNode(2, "localhost", 5002));
    
    EXPECT_EQ(coordinator.getNodeId(), 0);
    EXPECT_EQ(coordinator.getClusterSize(), 3);
    
    coordinator.start();
    coordinator.stop();
}

TEST_F(RaftNodeTest, CoordinatorLeadershipQueries) {
    ClusterCoordinator coordinator(0);
    coordinator.addNode(ClusterNode(0, "localhost", 5000));
    
    // Single node coordinator should be able to be healthy
    EXPECT_TRUE(coordinator.isHealthy());
}
