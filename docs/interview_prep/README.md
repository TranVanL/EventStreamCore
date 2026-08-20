# EventStreamCore — Senior Interview Preparation Kit

> **Mục tiêu:** Giúp bạn defend project EventStreamCore ở cấp độ senior/staff engineer, từ foundation đến deep-dive, từ architecture đến behavioral.

## 📚 Cấu trúc bộ tài liệu

| File | Nội dung | Cấp độ |
|------|----------|--------|
| [`01_foundation.md`](01_foundation.md) | Câu hỏi nền tảng: project là gì, tại sao làm, trade-off cơ bản | Junior → Mid |
| [`02_architecture_defense.md`](02_architecture_defense.md) | Defend toàn bộ kiến trúc: ingest → dispatch → event bus → processors → storage | Senior |
| [`03_lock_free_concurrency.md`](03_lock_free_concurrency.md) | Vyukov MPSC, SPSC ring buffer, deduplicator, memory ordering, false sharing | Senior+ |
| [`04_real_time_rtos_qnx.md`](04_real_time_rtos_qnx.md) | SCHED_FIFO, priority inheritance, QNX Neutrino, platform abstraction | Senior/Staff |
| [`05_performance_benchmarking.md`](05_performance_benchmarking.md) | Số liệu, cách benchmark đúng, latency vs throughput, NUMA | Senior |
| [`06_networking_ingest.md`](06_networking_ingest.md) | epoll, TCP/UDP ingest, frame parser, backpressure trên network | Senior |
| [`07_memory_storage.md`](07_memory_storage.md) | Event pool, NUMA, storage engine, DLQ, persistence strategy | Senior |
| [`08_behavioral_leadership.md`](08_behavioral_leadership.md) | Câu hỏi behavioral, cách kể story dựa trên project | Senior/Staff |
| [`09_system_design_scenarios.md`](09_system_design_scenarios.md) | Các scenario mở rộng: scale 10x, failover, multi-node | Staff |
| [`10_quick_reference.md`](10_quick_reference.md) | Cheat sheet: số liệu, file mapping, công thức trả lời nhanh | Mọi cấp độ |

## 🎯 Chiến lược sử dụng

1. **Trước phỏng vấn 1 tuần:** Đọc toàn bộ 1 lượt, đánh dấu chỗ chưa chắc.
2. **Trước phỏng vấn 2-3 ngày:** Tập trung vào file phù hợp với JD (RTOS → file 04, Performance → file 05).
3. **Trước phỏng vấn 1 ngày:** Đọc `10_quick_reference.md` và `08_behavioral_leadership.md`.
4. **Trong phỏng vấn:** Dùng công thức **"Context → Decision → Trade-off → Evidence"** cho mọi câu hỏi.

## 🗣️ Công thức trả lời mọi câu hỏi kỹ thuật

```
1. CONTEXT: "Trong EventStreamCore, vấn đề này xuất hiện ở..."
2. DECISION: "Tôi quyết định dùng..."
3. TRADE-OFF: "Lợi ích là..., nhưng phải đánh đổi..."
4. EVIDENCE: "Benchmark/test cho thấy..."
```

## 🔑 Keywords cần nhắc đến

- Lock-free: Vyukov MPSC, SPSC ring buffer, cache-line alignment
- Real-time: SCHED_FIFO, priority inheritance, robust mutex, cyclictest
- RTOS/QNX: Neutrino message passing, resource manager, policy-based templates
- POSIX: epoll, timerfd, eventfd, POSIX message queue, shared memory
- Performance: sub-microsecond latency, millions ops/sec, NUMA binding
- Hardening: backpressure, DLQ, deduplication, SLA enforcement

## ⚠️ Lưu ý quan trọng

- Mọi con số trong bộ tài liệu đều dựa trên README/benchmark của project. Nếu benchmark trên máy khác, số có thể thay đổi — hãy nói rõ "measured on my dev box".
- Project đang trong quá trình upgrade 2.0 (real-time/QNX). Một số module trong plan chưa implement xong — đó là **roadmap**, không phải bug. Hãy biết phân biệt "đã làm" vs "đang làm".
- Luôn thành thật về những gì chưa hoàn thiện. Senior được đánh giá cao ở khả năng nhận diện gap và plan để fill.

## 🎓 7-Day Intensive Study Plan

### Day 1 — Foundation & Architecture
- Đọc `01_foundation.md` và `02_architecture_defense.md`.
- Vẽ architecture diagram từ đầu không nhìn tài liệu.
- Ghi âm 2-minute pitch, nghe lại.

### Day 2 — Concurrency Deep Dive
- Đọc `03_lock_free_concurrency.md`.
- Viết lại Vyukov push/pop bằng tay.
- Giải thích memory ordering cho từng operation.

### Day 3 — Real-Time & RTOS
- Đọc `04_real_time_rtos_qnx.md`.
- Vẽ priority inversion diagram.
- Luyện giải thích QNX message passing.

### Day 4 — Performance & Networking
- Đọc `05_performance_benchmarking.md` và `06_networking_ingest.md`.
- Luyện interpret benchmark numbers.
- So sánh epoll vs io_uring.

### Day 5 — Memory & Storage
- Đọc `07_memory_storage.md`.
- Tính memory footprint cho 1M events queued.
- Thảo luận hazard pointers vs RCU.

### Day 6 — Behavioral & System Design
- Đọc `08_behavioral_leadership.md` và `09_system_design_scenarios.md`.
- Chuẩn bị 5 câu chuyện STAR.
- Luyện scale 10x trong 5 phút.

### Day 7 — Quick Review
- Đọc `10_quick_reference.md`.
- Mock interview với bạn bè hoặc tự ghi âm.
- Ngủ sớm.

---

## 🎯 Mapping JD Keywords to Files

| JD Keyword | File | Question Type |
|------------|------|---------------|
| C++11/14/17 | 01, 03, 07 | Language features, RAII, atomics |
| Multithreading | 03, 04 | Lock-free, mutex, condition variables |
| POSIX/Linux | 04, 06 | epoll, pthread, signals, IPC |
| RTOS/QNX | 04 | Message passing, resource manager |
| Real-time | 04, 05 | SCHED_FIFO, priority inheritance, cyclictest |
| Networking | 06 | TCP/UDP, epoll, io_uring, CAN |
| Performance | 05, 07 | Benchmarking, NUMA, cache locality |
| Embedded | 04, 07 | Cross-compile, memory constraints |
| System design | 09 | Scale, failover, consistency |
| Leadership | 08 | Behavioral, prioritization |

---

## 🛡️ How to Handle "I Don't Know"

**Đừng nói "I don't know" và dừng lại.** Thay vào đó:

1. **Clarify:** "That's a great question. To make sure I answer in the right context, are you asking about the current implementation or the 2.0 roadmap?"
2. **Think aloud:** "I haven't implemented X yet, but here's how I would approach it..."
3. **Connect to known:** "It's similar to Y in EventStreamCore, where we did Z. For X, I would..."
4. **Be honest:** "That's a gap in my current knowledge. I would research [specific area] and prototype before committing to a design."

**Example:**
> "I haven't tuned a PREEMPT_RT kernel hands-on, but I understand the principles: it makes most kernel code preemptible to reduce scheduling latency. In EventStreamCore, I would validate it with cyclictest and compare jitter against a standard kernel."

---

## 🎭 Interview Simulation Tactics

### For Phone Screen (30-45 min)
- Pitch project in 2 minutes.
- Expect 3-4 foundation questions.
- Mention numbers naturally.

### For Technical Round (60 min)
- Deep dive into 1-2 areas matching JD.
- Draw diagrams.
- Be ready to write pseudocode.

### For System Design Round (45-60 min)
- Start with requirements.
- Discuss trade-offs explicitly.
- Mention EventStreamCore as a building block, not the whole solution.

### For Behavioral Round (45 min)
- Use STAR for every answer.
- Tie stories to project impact.
- Show growth and learning.

---

## 🚨 Common Interview Traps & How to Avoid

| Trap | Why It's Dangerous | How to Avoid |
|------|-------------------|--------------|
| "My project is perfect" | Senior engineers expected to know limitations | Always mention trade-offs and roadmap |
| Over-claiming roadmap | Might be asked to implement on whiteboard | Clearly separate "done" vs "planned" |
| Ignoring failure modes | Shows lack of production experience | Discuss DLQ, backpressure, crashes |
| Only throughput, no latency | Misses tail latency concern | Always mention p99 and jitter |
| seq_cst everywhere | Shows shallow memory ordering knowledge | Explain explicit ordering choices |

---

## 🔗 Cross-Reference Guide

- **Hot path:** 01 → 02 → 03 → 05
- **RTOS/QNX defense:** 01 → 02 → 04 → 09
- **Performance deep dive:** 03 → 05 → 07
- **Production hardening:** 02 → 06 → 07 → 09
- **Behavioral storytelling:** 08 + any technical file

---

## 💡 Pro Tips from Senior Interviewers

1. **Lead with the "why" before the "what".** Interviewers care more about your reasoning than memorized facts.
2. **Use concrete numbers.** "125M ops/s" is more convincing than "very fast."
3. **Admit limitations confidently.** "We haven't implemented X yet, but the plan is Y" shows maturity.
4. **Ask clarifying questions.** It shows you think before answering.
5. **Summarize at the end.** "So to summarize, we chose X because of Y, accepting trade-off Z."
