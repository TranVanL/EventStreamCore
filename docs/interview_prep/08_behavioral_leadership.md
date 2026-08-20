# 08 — Behavioral & Leadership Interview

> File này giúp bạn trả lời câu hỏi behavioral dựa trên project EventStreamCore. Mỗi câu trả lời nên dùng STAR method (Situation, Task, Action, Result).

---

## STAR Template

```
Situation: "Trong EventStreamCore, tôi gặp vấn đề..."
Task: "Tôi cần..."
Action: "Tôi đã làm..."
Result: "Kết quả là..."
```

---

## Q1: "Tell me about a time you had to make a difficult technical trade-off."

**Answer:**

> **Situation:** Khi thiết kế EventBus, tôi phải chọn giữa một lock-free queue cho tất cả priorities hay nhiều queue khác nhau.
>
> **Task:** Cần đảm bảo realtime events không bị block bởi transactional/batch, nhưng cũng không muốn code quá phức tạp.
>
> **Action:** Tôi quyết định dùng 3 queue:
> - Realtime: SPSC ring buffer lock-free.
> - Transactional: deque + mutex (ordered delivery).
> - Batch: deque + mutex (window aggregation).
>
> **Result:** Hot path realtime đạt 125M ops/s, trong khi transactional vẫn đảm bảo ordering. Code rõ ràng, dễ maintain.

---

## Q2: "Tell me about a time you improved performance."

**Answer:**

> **Situation:** Ban đầu MPSC queue có head/tail/size nằm gần nhau, dẫn đến false sharing khi nhiều producer.
>
> **Task:** Cải thiện throughput và giảm latency variance.
>
> **Action:** Thêm `alignas(64)` cho từng atomic field. Sau đó benchmark lại trên nhiều thread.
>
> **Result:** Throughput tăng rõ rệt, latency variance giảm. Bài học: cache-line alignment rất quan trọng trên multi-core.

---

## Q3: "Tell me about a time you had to debug a complex issue."

**Answer:**

> **Situation:** Một stress test thỉnh thoảng crash trong dedup cleanup.
>
> **Task:** Tìm root cause và fix.
>
> **Action:** Tôi chạy với ThreadSanitizer và phát hiện race giữa cleanup thread xóa node và reader thread đang duyệt linked list. Fix tạm thời là dùng `atomic_thread_fence`, nhưng plan đúng là hazard pointers.
>
> **Result:** Crash biến mất trong test. Tôi ghi nhận technical debt và lên kế hoạch implement hazard pointers trong 2.0.

---

## Q4: "How do you handle disagreements with teammates?"

**Answer:**

> **Situation:** Có teammate đề xuất dùng `std::memory_order_seq_cst` everywhere cho "an toàn".
>
> **Task:** Thuyết phục họ dùng weaker ordering đúng chỗ.
>
> **Action:** Tôi chạy benchmark trên ARM board cho thấy `seq_cst` chậm hơn đáng kể. Sau đó review từng operation và giải thích tại sao `release`/`acquire` là đủ. Chúng tôi compromise: dùng explicit ordering với comment giải thích.
>
> **Result:** Code vừa đúng vừa nhanh, team cũng học thêm về memory ordering.

---

## Q5: "Tell me about a time you had to learn something new quickly."

**Answer:**

> **Situation:** JD target yêu cầu QNX/RTOS, nhưng tôi chưa có kinh nghiệm QNX thực tế.
>
> **Task:** Làm cho EventStreamCore portable sang QNX.
>
> **Action:** Tôi đọc QNX SDP docs, tìm hiểu Neutrino message passing, resource manager, và cách cross-compile. Sau đó thiết kế platform abstraction layer với policy-based templates.
>
> **Result:** Engine có thể compile cho QNX (compile-only validation nếu không có hardware). Tôi hiểu sâu hơn về RTOS differences.

---

## Q6: "How do you prioritize features?"

**Answer:**

> Trong EventStreamCore 2.0 roadmap, tôi dùng framework:
> 1. **JD alignment:** Ưu tiên features match JD keywords (QNX, SCHED_FIFO, POSIX IPC).
> 2. **Foundation first:** RT scheduling trước, vì các module sau dựa vào nó.
> 3. **Risk reduction:** Platform abstraction sớm để phát hiện portability issues.
> 4. **Cuttable features:** WebSocket, Prometheus export đánh dấu NICE — cut nếu trễ.
>
> **Evidence:** `MASTER_UPGRADE_PLAN.md` xếp Module A (QNX) và Module B (RT) là HIGHEST PRIORITY.

---

## Q7: "Tell me about a time you had to deliver under pressure."

**Answer:**

> **Situation:** Chuẩn bị demo project, benchmark MPSC queue không đạt target.
>
> **Task:** Cần cải thiện throughput trong 2 ngày.
>
> **Action:** Tôi profile với `perf`, phát hiện false sharing và allocation bottleneck. Thêm cache-line alignment và tăng pool size. Chạy lại benchmark trên isolated CPU.
>
> **Result:** Demo thành công, throughput đạt 52M ops/s. Tôi cũng document lessons learned.

---

## Q8: "What is your approach to code review?"

**Answer:**

> 1. **Correctness first:** Memory ordering, thread safety, resource leak.
> 2. **Performance second:** Hot path allocation, cache locality, syscall count.
> 3. **Maintainability:** Naming, comments, test coverage.
> 4. **Constructive feedback:** Đề xuất alternative, không chỉ chỉ ra lỗi.
>
> **Example:** Khi review dedup code, tôi tập trung vào ABA safety và cleanup thread safety, vì đó là lĩnh vực dễ sai.

---

## Q9: "How do you balance quality and speed?"

**Answer:**

> 1. **MVP với tests:** Luôn có unit test cho core logic ngay từ đầu.
> 2. **Technical debt tracking:** Ghi rõ những gì cần harden sau (ví dụ: hazard pointers).
> 3. **Benchmark-driven:** Mỗi optimization phải có số đo trước/sau.
> 4. **Incremental delivery:** Mỗi tuần có checkpoint và tag.
>
> **Evidence:** `PLAN_DETAILED.md` yêu cầu "1 meaningful commit mỗi ngày" và "cuối mỗi tuần push + tag".

---

## Q10: "Why should we hire you?"

**Answer template:**

> Tôi có kinh nghiệm xây dựng high-performance systems từ đầu. EventStreamCore cho thấy tôi có thể:
> - Thiết kế lock-free concurrency primitives.
> - Optimize cho latency và throughput.
> - Làm việc với low-level OS APIs (epoll, pthread, POSIX IPC).
> - Plan và execute complex upgrades (RTOS/QNX portability).
> - Balance technical depth với practical delivery.
>
> Tôi cũng học hỏi nhanh và thích solve hard problems.

---

## Q11: "Tell me about a time you mentored someone."

**Answer:**

> **Situation:** Một junior engineer join team và cần làm quen với lock-free code trong EventStreamCore.
>
> **Task:** Giúp họ hiểu memory ordering và review code đúng cách.
>
> **Action:**
> - Tôi tổ chức 3 buổi workshop nhỏ: atomics, false sharing, TSan.
> - Giao họ một task nhỏ: viết unit test cho SPSC ring buffer.
> - Review từng dòng code, giải thích tại sao cần `release`/`acquire`.
> - Khuyến khích họ chạy benchmark và so sánh với `std::queue`.
>
> **Result:** Sau 2 tuần, họ tự viết được test cho MPSC queue và tìm ra một bug nhỏ trong cleanup logic.

---

## Q12: "Tell me about a time you had a conflict with a teammate."

**Answer:**

> **Situation:** Teammate muốn dùng Boost.Lockfree thay vì custom MPSC queue.
>
> **Task:** Quyết định approach phù hợp.
>
> **Action:**
> - Tôi không phản đối ngay. Thay vào đó, tôi đề xuất benchmark cả hai.
> - Chúng tôi viết benchmark fair: cùng số thread, cùng payload, cùng CPU pinning.
> - Kết quả: Boost nhanh hơn ở low contention, custom Vyukov nhanh hơn ở high contention.
> - Quyết định: Giữ custom cho hot path, dùng Boost cho prototype nhanh.
>
> **Result:** Team hài lòng vì quyết định dựa trên data, không phải opinion.

---

## Q13: "Tell me about a time you dealt with ambiguity."

**Answer:**

> **Situation:** JD target yêu cầu "RTOS experience" nhưng không rõ là FreeRTOS, QNX, hay VxWorks.
>
> **Task:** Làm cho project relevant với nhiều RTOS nhất có thể.
>
> **Action:**
> - Tôi nghiên cứu common RTOS patterns: priority inheritance, message passing, static allocation.
> - Thiết kế platform abstraction layer không tightly couple với QNX.
> - Implement Linux path trước, QNX path compile-only, document cách add FreeRTOS.
>
> **Result:** Project cover QNX keywords và cũng dễ extend sang RTOS khác.

---

## Q14: "Tell me about a time you missed a deadline."

**Answer:**

> **Situation:** Tôi plan hoàn thành io_uring ingest server trong 1 tuần nhưng mất 2 tuần do kernel version differences.
>
> **Task:** Vẫn deliver overall milestone.
>
> **Action:**
> - Tôi communicate sớm với stakeholder về delay.
> - Tách io_uring thành "nice to have", tập trung vào epoll hardening trước.
> - Ghi lại lessons learned về feature detection.
>
> **Result:** Milestone chính vẫn đúng hạn. io_uring được reschedule vào sprint sau.

---

## Q15: "How do you handle receiving critical feedback?"

**Answer:**

> **Situation:** Trong code review, senior engineer chỉ ra rằng dedup cleanup không an toàn khi concurrent reader.
>
> **Task:** Tiếp nhận feedback và cải thiện.
>
> **Action:**
> - Tôi cảm ơn và hỏi rõ hơn về scenario cụ thể.
> - Tôi viết một stress test để reproduce race condition.
> - Sau khi confirm, tôi implement temporary fix và plan hazard pointers.
>
> **Result:** Code an toàn hơn. Tôi cũng học được cách review lock-free code kỹ hơn.

---

## Q16: "Tell me about a time you had to influence without authority."

**Answer:**

> **Situation:** Tôi muốn team adopt ThreadSanitizer trong CI nhưng không phải manager.
>
> **Task:** Thuyết phục team.
>
> **Action:**
> - Tôi chạy TSan trên codebase và tìm 2 real bugs.
> - Tạo demo showing TSan catch race trong 5 minutes.
> - Estimate cost: CI chậm thêm 10 phút nhưng giảm debug time.
> - Đề xuất chạy TSan trên nightly build trước, sau đó merge vào PR.
>
> **Result:** Team đồng ý thêm TSan vào nightly CI.

---

## Q17: "How do you stay updated with technology?"

**Answer:**

> 1. **Reading:** cppreference, papers (Vyukov queue, hazard pointers), conference talks (CppCon).
> 2. **Hands-on:** Implement algorithms từ papers trong side project.
> 3. **Community:** Reddit r/cpp, LLVM discourse, local meetups.
> 4. **Applied learning:** Khi học QNX, tôi áp dụng ngay vào EventStreamCore.
>
> **Example:** Tôi đọc về hazard pointers từ paper của Maged Michael, sau đó plan integrate vào project.

---

## Q18: "Tell me about a time you failed."

**Answer:**

> **Situation:** Tôi optimize MPSC queue bằng cách bỏ `size_` counter để giảm false sharing, nhưng làm capacity check không chính xác.
>
> **Task:** Fix và học bài học.
>
> **Action:**
> - Stress test phát hiện queue vượt quá capacity trong high contention.
> - Tôi revert change và thiết kế lại: giữ `size_` nhưng `alignas(64)` riêng.
> - Viết regression test.
>
> **Result:** Performance vẫn cải thiện và correctness được bảo toàn. Tôi học được: đừng optimize đến mức phá vỡ invariant.

---

## Q19: "How do you approach technical debt?"

**Answer:**

> 1. **Track:** Ghi rõ debt trong comments hoặc issue tracker.
> 2. **Prioritize:** Debt nào ảnh hưởng correctness/safety trước.
> 3. **Pay incrementally:** Mỗi sprint dành 10-20% capacity.
> 4. **Prevent:** Viết test trước khi refactor.
>
> **EventStreamCore example:** Dedup cleanup chưa an toàn → track as debt, plan hazard pointers trong 2.0.

---

## Q20: "Describe your ideal team culture."

**Answer:**

> 1. **Psychological safety:** Dám admit mistakes và ask questions.
> 2. **Data-driven decisions:** Benchmark và metrics thay vì opinion.
> 3. **Ownership:** Mỗi người responsible end-to-end cho feature.
> 4. **Continuous learning:** Share papers, tools, lessons learned.
> 5. **Pragmatism:** Balance perfect design với shipping.
>
> **Evidence:** Trong EventStreamCore, tôi maintain `docs/lessons.md` mỗi tuần để share what I learned.

---

## ✅ Enhanced Behavioral Checklist

- [ ] Câu chuyện mentorship.
- [ ] Câu chuyện conflict resolution.
- [ ] Câu chuyện dealing with ambiguity.
- [ ] Câu chuyện missed deadline.
- [ ] Câu chuyện receiving feedback.
- [ ] Câu chuyện influence without authority.
- [ ] Câu trả lời về staying updated.
- [ ] Câu chuyện failure.
- [ ] Approach to technical debt.
- [ ] Ideal team culture.
