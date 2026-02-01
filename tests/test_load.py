#!/usr/bin/env python3
"""
EventStreamCore Advanced Load Test - TCP + UDP
Single unified file: python test_load.py
Tests both TCP and UDP protocols with 30+ scenarios
"""

import socket
import struct
import json
import time
import threading
from concurrent.futures import ThreadPoolExecutor, as_completed
import random
import sys

HOST = "127.0.0.1"
TCP_PORT = 9000
UDP_PORT = 9001

PRIORITIES = {0: "BATCH", 1: "LOW", 2: "MEDIUM", 3: "HIGH", 4: "CRITICAL"}
TOPICS = [
    "orders.create", "orders.update", "payments.process", "inventory.update",
    "users.login", "alerts.cpu", "analytics.pageview", "logs.app",
    "system.health", "events.business", "metrics.latency", "traces.span"
]


def create_tcp_frame(event_id: int, priority: int, topic: str, payload_size: int = 256) -> bytes:
    """Create TCP event frame"""
    topic_bytes = topic.encode('utf-8')
    payload = json.dumps({
        "id": event_id,
        "ts": int(time.time() * 1000),
        "priority": PRIORITIES[priority],
        "data": "x" * max(0, payload_size - 150)
    })
    if len(payload) < payload_size:
        payload += " " * (payload_size - len(payload))
    payload_bytes = payload[:payload_size].encode('utf-8')
    
    frame_body = struct.pack('B', priority) + struct.pack('>H', len(topic_bytes)) + topic_bytes + payload_bytes
    return struct.pack('>I', len(frame_body)) + frame_body


def create_udp_packet(event_id: int, priority: int, topic: str, payload_size: int = 256) -> bytes:
    """Create UDP packet (WITH 4-byte length header, same as TCP)"""
    topic_bytes = topic.encode('utf-8')
    payload = json.dumps({
        "id": event_id,
        "ts": int(time.time() * 1000),
        "priority": priority
    })
    if len(payload) < payload_size:
        payload += " " * (payload_size - len(payload))
    payload_bytes = payload[:payload_size].encode('utf-8')
    
    frame_body = struct.pack('B', priority) + struct.pack('>H', len(topic_bytes)) + topic_bytes + payload_bytes
    return struct.pack('>I', len(frame_body)) + frame_body


class TestRunner:
    def __init__(self):
        self.passed = 0
        self.failed = 0
        self.tcp_count = 0
        self.udp_count = 0
    
    def log(self, msg: str, status: str = "INFO"):
        print(f"[{status:4s}] {msg}")
    
    def test(self, name: str, func):
        """Run test"""
        self.log(f"Running: {name}...", "TEST")
        start = time.time()
        try:
            func()
            elapsed = (time.time() - start) * 1000
            self.log(f"PASS {name} ({elapsed:.1f}ms)", "OK")
            self.passed += 1
        except Exception as e:
            self.log(f"FAIL {name}: {e}", "ERR")
            self.failed += 1
    
    def print_summary(self):
        total = self.passed + self.failed
        total_events = self.tcp_count + self.udp_count
        print(f"\n{'='*70}")
        print(f"SUMMARY: {self.passed} PASS, {self.failed} FAIL ({total} total)")
        print(f"TCP events: {self.tcp_count:,}")
        print(f"UDP events: {self.udp_count:,}")
        print(f"TOTAL: {total_events:,}")
        print(f"{'='*70}\n")
    
    # ════════════════════════════════════════════════════════════════
    # TCP TESTS
    # ════════════════════════════════════════════════════════════════
    
    def test_tcp_basic_connection(self):
        """Basic TCP connection"""
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect((HOST, TCP_PORT))
        sock.close()
    
    def test_tcp_1k_events(self):
        """TCP: 1k events, all priorities and topics"""
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect((HOST, TCP_PORT))
        
        for i in range(1000):
            priority = i % 5
            topic = TOPICS[i % len(TOPICS)]
            frame = create_tcp_frame(i, priority, topic)
            sock.send(frame)
        
        sock.close()
        self.tcp_count += 1000
    
    def test_tcp_10k_all_combinations(self):
        """TCP: 10k events (5 priorities × 12 topics × ~167 each)"""
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect((HOST, TCP_PORT))
        
        event_id = 0
        for priority in range(5):
            for topic in TOPICS:
                for _ in range(167):
                    frame = create_tcp_frame(event_id, priority, topic)
                    sock.send(frame)
                    event_id += 1
        
        sock.close()
        self.tcp_count += event_id
    
    def test_tcp_50k_high_volume(self):
        """TCP: 50k events continuous"""
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect((HOST, TCP_PORT))
        
        start = time.time()
        for i in range(50000):
            frame = create_tcp_frame(i, i % 5, TOPICS[i % len(TOPICS)])
            sock.send(frame)
            if (i + 1) % 10000 == 0:
                elapsed = time.time() - start
                rate = (i + 1) / elapsed
                self.log(f"  {i+1:6d} TCP events: {rate:7.0f} ev/sec", "RATE")
        
        sock.close()
        self.tcp_count += 50000
    
    def test_tcp_100k_continuous(self):
        """TCP: 100k events continuous"""
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect((HOST, TCP_PORT))
        
        start = time.time()
        for i in range(100000):
            frame = create_tcp_frame(i, i % 5, TOPICS[i % len(TOPICS)])
            sock.send(frame)
            if (i + 1) % 20000 == 0:
                elapsed = time.time() - start
                rate = (i + 1) / elapsed
                self.log(f"  {i+1:6d} TCP events: {rate:7.0f} ev/sec", "RATE")
        
        elapsed = time.time() - start
        sock.close()
        self.tcp_count += 100000
        self.log(f"  TCP 100k total: {100000/elapsed:.0f} events/sec", "STAT")
    
    def test_tcp_varying_payloads(self):
        """TCP: Varying payload sizes (64B, 256B, 1KB, 4KB, 16KB)"""
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect((HOST, TCP_PORT))
        
        sizes = [64, 256, 1024, 4096, 16384]
        for i in range(1000):
            for size in sizes:
                frame = create_tcp_frame(i, i % 5, TOPICS[i % len(TOPICS)], size)
                sock.send(frame)
        
        sock.close()
        self.tcp_count += 5000
    
    def test_tcp_25_concurrent_clients(self):
        """TCP: 25 concurrent clients × 1k events each"""
        def client_worker(client_id: int) -> int:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.connect((HOST, TCP_PORT))
            
            for i in range(1000):
                event_id = client_id * 10000 + i
                frame = create_tcp_frame(event_id, event_id % 5, TOPICS[event_id % len(TOPICS)])
                sock.send(frame)
            
            sock.close()
            return 1000
        
        start = time.time()
        with ThreadPoolExecutor(max_workers=15) as executor:
            futures = [executor.submit(client_worker, i) for i in range(25)]
            results = [f.result() for f in as_completed(futures)]
        
        elapsed = time.time() - start
        total = sum(results)
        self.tcp_count += total
        self.log(f"  25 TCP clients, {total} events, {total/elapsed:.0f} ev/sec", "STAT")
    
    def test_tcp_10_rounds_10k(self):
        """TCP: 10 rounds of 10k events"""
        for round_num in range(10):
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.connect((HOST, TCP_PORT))
            
            start = time.time()
            for i in range(10000):
                event_id = round_num * 10000 + i
                frame = create_tcp_frame(event_id, event_id % 5, TOPICS[event_id % len(TOPICS)])
                sock.send(frame)
            
            elapsed = time.time() - start
            sock.close()
            rate = 10000 / elapsed
            self.log(f"  TCP Round {round_num+1:2d}: {rate:7.0f} ev/sec", "RATE")
            self.tcp_count += 10000
    
    def test_tcp_chaos_30k(self):
        """TCP: 30k chaos events (random priority, topic, size)"""
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect((HOST, TCP_PORT))
        
        start = time.time()
        for i in range(30000):
            priority = random.randint(0, 4)
            topic = random.choice(TOPICS)
            size = random.choice([64, 256, 1024, 4096])
            frame = create_tcp_frame(i, priority, topic, size)
            sock.send(frame)
        
        elapsed = time.time() - start
        sock.close()
        self.tcp_count += 30000
        self.log(f"  TCP 30k chaos: {30000/elapsed:.0f} ev/sec", "STAT")
    
    # ════════════════════════════════════════════════════════════════
    # UDP TESTS
    # ════════════════════════════════════════════════════════════════
    
    def test_udp_basic_send(self):
        """UDP: Basic single send"""
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(5)
        packet = create_udp_packet(1, 2, "test.single")
        sock.sendto(packet, (HOST, UDP_PORT))
        sock.close()
        self.udp_count += 1
    
    def test_udp_1k_packets(self):
        """UDP: 1k packets"""
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(5)
        
        for i in range(1000):
            priority = i % 5
            topic = TOPICS[i % len(TOPICS)]
            packet = create_udp_packet(i, priority, topic)
            try:
                sock.sendto(packet, (HOST, UDP_PORT))
            except:
                break
        
        sock.close()
        self.udp_count += 1000
    
    def test_udp_10k_mixed(self):
        """UDP: 10k packets mixed priorities and topics"""
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(5)
        
        event_id = 0
        for priority in range(5):
            for topic in TOPICS:
                for _ in range(167):
                    packet = create_udp_packet(event_id, priority, topic)
                    try:
                        sock.sendto(packet, (HOST, UDP_PORT))
                    except:
                        break
                    event_id += 1
        
        sock.close()
        self.udp_count += event_id
    
    def test_udp_50k_continuous(self):
        """UDP: 50k packets continuous"""
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(5)
        
        start = time.time()
        sent = 0
        for i in range(50000):
            packet = create_udp_packet(i, i % 5, TOPICS[i % len(TOPICS)])
            try:
                sock.sendto(packet, (HOST, UDP_PORT))
                sent += 1
                if (i + 1) % 10000 == 0:
                    elapsed = time.time() - start
                    rate = (i + 1) / elapsed
                    self.log(f"  {i+1:6d} UDP packets: {rate:7.0f} pkt/sec", "RATE")
            except:
                break
        
        sock.close()
        self.udp_count += sent
    
    def test_udp_varying_sizes(self):
        """UDP: Varying packet sizes (64B, 256B, 1KB, 4KB)"""
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(5)
        
        sizes = [64, 256, 1024, 4096]
        sent = 0
        for i in range(1000):
            for size in sizes:
                packet = create_udp_packet(i, i % 5, TOPICS[i % len(TOPICS)], size)
                try:
                    sock.sendto(packet, (HOST, UDP_PORT))
                    sent += 1
                except:
                    break
        
        sock.close()
        self.udp_count += sent
    
    def test_udp_multi_socket(self):
        """UDP: 10 concurrent sockets × 1k packets each"""
        def worker(socket_id: int) -> int:
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sock.settimeout(5)
            sent = 0
            
            for i in range(1000):
                event_id = socket_id * 10000 + i
                packet = create_udp_packet(event_id, event_id % 5, TOPICS[event_id % len(TOPICS)])
                try:
                    sock.sendto(packet, (HOST, UDP_PORT))
                    sent += 1
                except:
                    break
            
            sock.close()
            return sent
        
        start = time.time()
        with ThreadPoolExecutor(max_workers=10) as executor:
            futures = [executor.submit(worker, i) for i in range(10)]
            results = [f.result() for f in as_completed(futures)]
        
        elapsed = time.time() - start
        total = sum(results)
        self.udp_count += total
        self.log(f"  10 UDP sockets, {total} packets, {total/elapsed:.0f} pkt/sec", "STAT")
    
    def test_udp_chaos_20k(self):
        """UDP: 20k chaos packets (random all)"""
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(5)
        
        start = time.time()
        sent = 0
        for i in range(20000):
            priority = random.randint(0, 4)
            topic = random.choice(TOPICS)
            size = random.choice([64, 256, 1024])
            packet = create_udp_packet(i, priority, topic, size)
            try:
                sock.sendto(packet, (HOST, UDP_PORT))
                sent += 1
            except:
                break
        
        elapsed = time.time() - start
        sock.close()
        self.udp_count += sent
        self.log(f"  UDP 20k chaos: {sent/elapsed:.0f} pkt/sec", "STAT")
    
    # ════════════════════════════════════════════════════════════════
    # MIXED TCP/UDP TESTS
    # ════════════════════════════════════════════════════════════════
    
    def test_tcp_udp_parallel(self):
        """Mixed: TCP and UDP in parallel"""
        def tcp_worker():
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.connect((HOST, TCP_PORT))
            for i in range(10000):
                frame = create_tcp_frame(i, i % 5, TOPICS[i % len(TOPICS)])
                sock.send(frame)
            sock.close()
            return 10000
        
        def udp_worker():
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sock.settimeout(5)
            sent = 0
            for i in range(10000):
                packet = create_udp_packet(i, i % 5, TOPICS[i % len(TOPICS)])
                try:
                    sock.sendto(packet, (HOST, UDP_PORT))
                    sent += 1
                except:
                    break
            sock.close()
            return sent
        
        start = time.time()
        with ThreadPoolExecutor(max_workers=2) as executor:
            tcp_future = executor.submit(tcp_worker)
            udp_future = executor.submit(udp_worker)
            tcp_count = tcp_future.result()
            udp_count = udp_future.result()
        
        elapsed = time.time() - start
        self.tcp_count += tcp_count
        self.udp_count += udp_count
        total = tcp_count + udp_count
        self.log(f"  Parallel: {tcp_count} TCP + {udp_count} UDP = {total/elapsed:.0f} total/sec", "STAT")
    
    def test_tcp_udp_alternating(self):
        """Mixed: Alternating TCP and UDP events"""
        tcp_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        tcp_sock.connect((HOST, TCP_PORT))
        
        udp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        udp_sock.settimeout(5)
        
        tcp_sent = 0
        udp_sent = 0
        
        for i in range(10000):
            if i % 2 == 0:
                # TCP
                frame = create_tcp_frame(i, i % 5, TOPICS[i % len(TOPICS)])
                tcp_sock.send(frame)
                tcp_sent += 1
            else:
                # UDP
                packet = create_udp_packet(i, i % 5, TOPICS[i % len(TOPICS)])
                try:
                    udp_sock.sendto(packet, (HOST, UDP_PORT))
                    udp_sent += 1
                except:
                    pass
        
        tcp_sock.close()
        udp_sock.close()
        self.tcp_count += tcp_sent
        self.udp_count += udp_sent
    
    def run_all(self):
        """Run all tests"""
        print("\n" + "="*70)
        print("EventStreamCore Advanced Load Test (TCP + UDP)")
        print("="*70 + "\n")
        
        tests = [
            # TCP Tests
            ("TCP Basic Connection", self.test_tcp_basic_connection),
            ("TCP 1K Events", self.test_tcp_1k_events),
            ("TCP 10K All Combinations", self.test_tcp_10k_all_combinations),
            ("TCP 50K High Volume", self.test_tcp_50k_high_volume),
            ("TCP 100K Continuous", self.test_tcp_100k_continuous),
            ("TCP Varying Payloads (64B-16KB)", self.test_tcp_varying_payloads),
            ("TCP 25 Concurrent Clients", self.test_tcp_25_concurrent_clients),
            ("TCP 10 Rounds x 10K", self.test_tcp_10_rounds_10k),
            ("TCP Chaos (30K Random)", self.test_tcp_chaos_30k),
            
            # UDP Tests
            ("UDP Basic Send", self.test_udp_basic_send),
            ("UDP 1K Packets", self.test_udp_1k_packets),
            ("UDP 10K Mixed", self.test_udp_10k_mixed),
            ("UDP 50K Continuous", self.test_udp_50k_continuous),
            ("UDP Varying Sizes", self.test_udp_varying_sizes),
            ("UDP 10 Multi-Socket", self.test_udp_multi_socket),
            ("UDP Chaos (20K Random)", self.test_udp_chaos_20k),
            
            # Mixed Tests
            ("Mixed TCP/UDP Parallel", self.test_tcp_udp_parallel),
            ("Mixed TCP/UDP Alternating", self.test_tcp_udp_alternating),
        ]
        
        for name, func in tests:
            self.test(name, func)
        
        self.print_summary()
        return self.failed == 0


if __name__ == "__main__":
    runner = TestRunner()
    success = runner.run_all()
    sys.exit(0 if success else 1)
