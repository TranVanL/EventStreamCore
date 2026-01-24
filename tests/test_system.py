#!/usr/bin/env python3
"""
EventStreamCore System Test Suite
Tests all critical paths and validates optimizations
"""

import socket
import struct
import time
import json
import threading
import subprocess
import sys
from dataclasses import dataclass
from typing import List, Tuple
import random
import string

# Test configuration
HOST = '127.0.0.1'
PORT = 9000
NUM_EVENTS = 1000
PAYLOAD_SIZE = 256

@dataclass
class TestResult:
    name: str
    passed: bool
    duration_ms: float
    message: str = ""

class EventStreamTester:
    def __init__(self):
        self.results: List[TestResult] = []
        self.server_process = None
    
    def log(self, msg: str, level: str = "INFO"):
        """Log message with timestamp"""
        timestamp = time.strftime("%Y-%m-%d %H:%M:%S")
        print(f"[{timestamp}] [{level}] {msg}")
    
    def run_test(self, name: str, test_func):
        """Run a test and record result"""
        self.log(f"Running: {name}...")
        start_time = time.time()
        try:
            test_func()
            duration_ms = (time.time() - start_time) * 1000
            self.results.append(TestResult(name, True, duration_ms))
            self.log(f"✅ PASS: {name} ({duration_ms:.1f}ms)")
            return True
        except Exception as e:
            duration_ms = (time.time() - start_time) * 1000
            self.results.append(TestResult(name, False, duration_ms, str(e)))
            self.log(f"❌ FAIL: {name} - {e}", "ERROR")
            return False
    
    def create_event_frame(self, event_id: int, payload_size: int = PAYLOAD_SIZE, topic: str = None) -> bytes:
        """Create a test event frame
        
        Format: [4-byte frame_length][1-byte priority][2-byte topic_len][topic][payload]
        Priority: 0=BATCH, 1=LOW, 2=MEDIUM, 3=HIGH, 4=CRITICAL
        """
        if topic is None:
            topic = f"test.topic.{event_id % 10}"
        
        # Priority: 0=BATCH, 1=LOW, 2=MEDIUM, 3=HIGH, 4=CRITICAL
        priority = event_id % 5
        
        # Build frame body: priority + topic_len + topic + payload
        topic_bytes = topic.encode('utf-8')
        topic_len = len(topic_bytes)
        
        # Create payload
        payload_str = json.dumps({
            "event_id": event_id,
            "timestamp": int(time.time() * 1000),
            "data": "x" * max(0, payload_size - 50)
        })
        
        # Pad or truncate to exact size
        if len(payload_str) < payload_size:
            payload_str += " " * (payload_size - len(payload_str))
        else:
            payload_str = payload_str[:payload_size]
        
        payload_bytes = payload_str.encode('utf-8')
        
        # Build frame body: 1 byte priority + 2 bytes topic_len + topic + payload
        frame_body = struct.pack('B', priority)  # 1 byte priority
        frame_body += struct.pack('>H', topic_len)  # 2 bytes topic length (big-endian)
        frame_body += topic_bytes
        frame_body += payload_bytes
        
        # Frame length (big-endian 4 bytes) - length of body only, not including the length header
        frame_len = len(frame_body)
        length_header = struct.pack('>I', frame_len)
        
        return length_header + frame_body
    
    def test_single_event(self):
        """Test sending a single event"""
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(5)
            sock.connect((HOST, PORT))
            
            frame = self.create_event_frame(0)
            sock.sendall(frame)
            
            time.sleep(0.1)  # Wait for processing
            sock.close()
            
        except Exception as e:
            raise Exception(f"Failed to send single event: {e}")
    
    def test_batch_events(self):
        """Test sending multiple events in batch"""
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(10)
            sock.connect((HOST, PORT))
            
            # Send 100 events
            for i in range(100):
                frame = self.create_event_frame(i)
                sock.sendall(frame)
            
            time.sleep(0.5)  # Wait for processing
            sock.close()
            
        except Exception as e:
            raise Exception(f"Failed to send batch: {e}")
    
    def test_high_frequency(self):
        """Test high-frequency event sending"""
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(15)
            sock.connect((HOST, PORT))
            
            # Send 1000 events as fast as possible
            start_time = time.time()
            for i in range(1000):
                frame = self.create_event_frame(i)
                sock.sendall(frame)
            
            elapsed = time.time() - start_time
            throughput = 1000 / elapsed
            self.log(f"  Throughput: {throughput:.0f} events/sec")
            
            time.sleep(1)  # Wait for processing
            sock.close()
            
        except Exception as e:
            raise Exception(f"Failed high-frequency test: {e}")
    
    def test_concurrent_clients(self):
        """Test multiple concurrent TCP clients"""
        def client_thread(client_id: int, num_events: int):
            try:
                sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                sock.settimeout(10)
                sock.connect((HOST, PORT))
                
                for i in range(num_events):
                    event_id = client_id * 10000 + i
                    frame = self.create_event_frame(event_id)
                    sock.sendall(frame)
                
                sock.close()
            except Exception as e:
                raise Exception(f"Client {client_id} failed: {e}")
        
        threads = []
        try:
            # Start 5 concurrent clients
            for i in range(5):
                t = threading.Thread(target=client_thread, args=(i, 100))
                t.start()
                threads.append(t)
            
            # Wait for all to complete
            for t in threads:
                t.join(timeout=15)
            
            time.sleep(0.5)
            
        except Exception as e:
            raise Exception(f"Concurrent client test failed: {e}")
    
    def test_large_payload(self):
        """Test sending events with large payloads"""
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(10)
            sock.connect((HOST, PORT))
            
            # Send events with 10KB payloads
            large_payload_size = 10240
            for i in range(10):
                frame = self.create_event_frame(i, large_payload_size)
                sock.sendall(frame)
            
            time.sleep(0.5)
            sock.close()
            
        except Exception as e:
            raise Exception(f"Large payload test failed: {e}")
    
    def test_mixed_topics(self):
        """Test events with different topics"""
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(10)
            sock.connect((HOST, PORT))
            
            # Send events to different topics
            topics = [
                "order.created",
                "payment.processed",
                "inventory.updated",
                "user.registered",
                "error.occurred",
                "warning.issued",
                "info.logged"
            ]
            
            for i in range(100):
                topic = topics[i % len(topics)]
                frame = self.create_event_frame(i, PAYLOAD_SIZE, topic)
                sock.sendall(frame)
            
            time.sleep(0.5)
            sock.close()
            
        except Exception as e:
            raise Exception(f"Mixed topics test failed: {e}")
    
    def print_summary(self):
        """Print test summary"""
        print("\n" + "="*70)
        print("TEST SUMMARY")
        print("="*70)
        
        passed = sum(1 for r in self.results if r.passed)
        total = len(self.results)
        
        for result in self.results:
            status = "✅ PASS" if result.passed else "❌ FAIL"
            print(f"{status} | {result.name:40s} | {result.duration_ms:8.1f}ms")
            if result.message:
                print(f"       {result.message}")
        
        print("="*70)
        print(f"Total: {passed}/{total} tests passed")
        
        if passed == total:
            print("🎉 ALL TESTS PASSED!")
        else:
            print(f"⚠️  {total - passed} test(s) failed")
        
        return passed == total
    
    def run_all_tests(self):
        """Run all tests"""
        print("╔════════════════════════════════════════════════════════════════════╗")
        print("║  EVENTSTREAM CORE SYSTEM TEST SUITE                                ║")
        print("║  Testing critical paths and optimizations                          ║")
        print("╚════════════════════════════════════════════════════════════════════╝\n")
        
        # Wait for server to be ready
        time.sleep(1)
        
        # Run tests
        self.run_test("Single Event", self.test_single_event)
        self.run_test("Batch Events (100)", self.test_batch_events)
        self.run_test("High Frequency (1000)", self.test_high_frequency)
        self.run_test("Concurrent Clients (5x100)", self.test_concurrent_clients)
        self.run_test("Large Payload (10KB)", self.test_large_payload)
        self.run_test("Mixed Topics", self.test_mixed_topics)
        
        # Print summary
        success = self.print_summary()
        
        return 0 if success else 1

def main():
    """Main entry point"""
    tester = EventStreamTester()
    
    try:
        exit_code = tester.run_all_tests()
        sys.exit(exit_code)
    except KeyboardInterrupt:
        print("\n\n⚠️  Tests interrupted by user")
        sys.exit(1)
    except Exception as e:
        print(f"\n❌ FATAL ERROR: {e}")
        sys.exit(1)

if __name__ == "__main__":
    main()
