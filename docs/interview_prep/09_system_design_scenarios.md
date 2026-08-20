# 09 — System Design Scenarios

> Các scenario mở rộng giúp bạn thể hiện tư duy staff-level: scale, failover, multi-node, security.

---

## Scenario 1: "Scale EventStreamCore 10x. How?"

**Answer:**

> **Bottleneck analysis:**
> 1. Single dispatcher.
> 2. Single realtime processor.
> 3. Storage mutex.
> 4. Network ingest (thread-per-client).
>
> **Scale plan:**
> 1. **Shard by topic:** N dispatcher shards, mỗi shard có MPSC inbound + EventBus riêng.
> 2. **Multiple realtime processors:** Mỗi processor pin to different CPU, consume từ cùng queue hoặc partitioned queue.
> 3. **io_uring ingest:** Một thread xử lý 100k+ connections.
> 4. **Per-thread storage write buffer:** Giảm contention trên storage mutex.
> 5. **NUMA-aware deployment:** Mỗi socket chạy một instance, tránh remote memory.
> 6. **Load balancer phía trước:** Distribute connections đến nhiều EventStreamCore instances.

---

## Scenario 2: "Add guaranteed delivery for transactional events."

**Answer:**

> **Current:** Transactional processor retry 3 lần, rồi drop vào DLQ.
>
> **Improvements:**
> 1. **WAL (Write-Ahead Log):** Ghi event vào log trước khi process. Nếu crash, replay từ log.
> 2. **Ack/Nack:** Producer nhận ack sau khi event persisted.
> 3. **Idempotency key:** Dedup table mở rộng với client-provided idempotency key.
> 4. **Replicated storage:** Dùng Raft hoặc primary-backup cho transactional log.
>
> **Trade-off:** Guaranteed delivery làm tăng latency và complexity. Chỉ áp dụng cho transactional stream.

---

## Scenario 3: "Handle a network partition between ingest and storage."

**Answer:**

> **Detection:**
> - Storage write fail liên tục.
> - Latency histogram tăng đột biến.
> - Health check fail.
>
> **Mitigation:**
> 1. **Backpressure:** Stop accepting new events nếu storage down quá lâu.
> 2. **DLQ overflow handling:** Khi DLQ full, log to local disk hoặc drop với metrics.
> 3. **Local spool:** Ghi event vào local disk khi remote storage unreachable, replay khi recover.
> 4. **Circuit breaker:** Sau N lỗi, ngừng gọi storage một khoảng thời gian.

---

## Scenario 4: "Add multi-tenant isolation."

**Answer:**

> **Approach:**
> 1. **Per-tenant EventBus:** Mỗi tenant có bộ 3 queue riêng.
> 2. **Tenant tag trong Event metadata:** Dispatcher route theo tenant.
> 3. **Resource quota:** CPU time, queue depth, storage size per tenant.
> 4. **Fair scheduling:** Work-stealing hoặc weighted round-robin giữa các tenant.
>
> **Trade-off:** Multi-tenant tăng memory footprint và scheduling complexity.

---

## Scenario 5: "Secure the ingest layer."

**Answer:**

> 1. **TLS:** Terminate TLS ở proxy hoặc embed OpenSSL.
> 2. **Authentication:** API key/token trong frame header hoặc TLS client cert.
> 3. **Authorization:** Topic-level ACL — tenant A chỉ publish đến topic `tenantA/*`.
> 4. **Rate limiting:** Per-connection và per-tenant rate limit.
> 5. **Input validation:** Frame parser reject malformed frames.
> 6. **Sandbox plugins:** Python/Go plugins chạy trong sandbox hoặc separate process.

---

## Scenario 6: "Deploy on Kubernetes as sidecar."

**Answer:**

> **Sidecar pattern:**
> - EventStreamCore chạy trong cùng pod với application.
> - App gửi event qua localhost TCP/Unix socket/shared memory.
> - Sidecar xử lý và forward đến backend.
>
> **Challenges:**
> - CPU pinning khó vì K8s scheduler.
> - Real-time latency khó đảm bảo trên shared node.
> - Solution: Guaranteed QoS, CPU manager static policy, dedicated nodes.
>
> **Evidence:** Roadmap có "Kubernetes sidecar deployment".

---

## Scenario 7: "Add exactly-once semantics."

**Answer:**

> **Exactly-once = at-least-once + idempotency.**
>
> **Implementation:**
> 1. Producer gán unique idempotency key cho mỗi event.
> 2. Dedup table lưu key đã process (TTL 1 giờ hoặc lâu hơn).
> 3. Storage lưu kết quả process theo idempotency key.
> 4. Nếu event duplicate, return stored result.
>
> **Trade-off:** Tăng storage và dedup memory. Phù hợp transactional, không cần cho realtime.

---

## Scenario 8: "Handle a bad software update."

**Answer:**

> **Rollback strategy:**
> 1. **Versioned config:** Mỗi config change có version, có thể revert.
> 2. **Canary deployment:** Deploy 5% instances trước, monitor metrics.
> 3. **Health checks:** Nếu p99 latency hoặc drop rate vượt ngưỡng, auto rollback.
> 4. **Binary rollback:** Giữ previous binary, switch nhanh.
> 5. **DLQ inspection:** Sau rollback, replay DLQ nếu cần.

---

## Scenario 9: "Design a monitoring dashboard."

**Answer:**

> **Metrics to expose:**
> - Throughput: events/sec per queue.
> - Latency: p50/p95/p99 per processor.
> - Queue depth: realtime/transactional/batch.
> - Drop rate: total dropped / total ingested.
> - Backpressure level.
> - DLQ size.
> - Connection count (TCP).
>
> **Export:** Prometheus endpoint (`/metrics`) hoặc statsd.
> **Dashboard:** Grafana với alerts cho SLA breach.

---

## Scenario 10: "Port to a new RTOS (e.g., FreeRTOS)."

**Answer:**

> **Steps:**
> 1. Implement `FreeRTOSPlatform` với `Thread`, `Mutex`, `Semaphore`, `Timer`, `Channel`.
> 2. Thay `pthread_*` bằng FreeRTOS APIs (`xTaskCreate`, `xSemaphoreTake`, etc.).
> 3. Thay `epoll` bằng FreeRTOS socket hoặc custom network stack.
> 4. Thay `mmap`/file storage bằng flash-backed storage.
> 5. Điều chỉnh capacity cho memory-constrained device.
> 6. Viết unit tests cho platform mới.
>
> **Trade-off:** FreeRTOS không có process isolation và rich POSIX support, nên một số features phải simplify.

---

## Scenario 11: "Design geo-distributed EventStreamCore."

**Answer:**

> **Requirements:**
> - Low latency ingest ở mỗi region.
> - Cross-region replication cho transactional events.
> - Conflict resolution nếu cùng event xuất hiện ở nhiều region.
>
> **Design:**
> 1. **Local EventStreamCore per region:** Ingest và process locally.
> 2. **Inter-region replication:** Chỉ replicate transactional stream qua dedicated link.
> 3. **Conflict-free replicated data type (CRDT):** Cho metrics/aggregates.
> 4. **Leader for partition:** Mỗi topic partition có leader region.
> 5. **Health monitoring:** Cross-region latency, replication lag.
>
> **Trade-off:** Complexity cao, consistency model cần rõ ràng.

---

## Scenario 12: "Add Change Data Capture (CDC) from a database."

**Answer:**

> **CDC pattern:** Capture database changes và emit events.
>
> **Implementation:**
> 1. **Poll-based:** Query table có `updated_at` hoặc version column.
> 2. **Log-based:** Đọc database WAL (MySQL binlog, PostgreSQL logical replication).
> 3. **Trigger-based:** Database trigger ghi vào change table.
>
> **Integration với EventStreamCore:**
> - Thêm `DatabaseCdcIngestServer` implement `IngestServer`.
> - Map DB row → Event với topic `db/<table>/<operation>`.
> - Dùng transactional queue để đảm bảo ordered delivery.
>
> **Trade-off:** Log-based CDC tốt nhất nhưng phức tạp. Poll-based đơn giản nhưng có delay.

---

## Scenario 13: "Handle schema evolution of events."

**Answer:**

> **Problem:** Event format thay đổi theo thời gian. Consumers cũ không hiểu format mới.
>
> **Solutions:**
> 1. **Versioning:** Thêm `schema_version` vào event header.
> 2. **Backward compatibility:** New fields optional, không xóa old fields.
> 3. **Schema registry:** Central registry quản lý schemas.
> 4. **Avro/Protobuf:** Binary formats hỗ trợ schema evolution.
>
> **EventStreamCore:** Hiện tại dùng custom binary format. Có thể migrate sang Protobuf hoặc FlatBuffers.

---

## Scenario 14: "Design data retention policy."

**Answer:**

> **Requirements:**
> - Realtime events: short retention (ví dụ 24h).
> - Transactional events: long retention (ví dụ 90 days).
> - Batch aggregates: indefinite.
>
> **Implementation:**
> 1. **Time-based segmentation:** Mỗi file storage cho một time window.
> 2. **TTL background job:** Xóa files cũ.
> 3. **Tiered storage:** Hot trên SSD, cold trên S3/object storage.
> 4. **Compaction:** Gom nhỏ files, loại bỏ duplicates.
>
> **Trade-off:** Longer retention = more storage cost.

---

## Scenario 15: "Ensure GDPR compliance for event data."

**Answer:**

> **GDPR requirements:**
> - Right to be forgotten.
> - Data minimization.
> - Consent tracking.
>
> **Implementation:**
> 1. **PII tagging:** Mark fields containing PII in schema.
> 2. **Encryption at rest:** Encrypt storage files.
> 3. **Deletion API:** Find all events by user ID và delete/anonymize.
> 4. **Retention limits:** Auto-delete after TTL.
> 5. **Audit log:** Log access to PII events.
>
> **Challenge:** Append-only storage khó xóa. Cần compaction hoặc encrypted per-user key mà có thể revoke.

---

## Scenario 16: "Design disaster recovery."

**Answer:**

> **RTO/RPO:**
> - RTO (Recovery Time Objective): Thời gian để recover.
> - RPO (Recovery Point Objective): Lượng data loss chấp nhận được.
>
> **Strategies:**
> 1. **Backup storage files:** Periodic snapshot.
> 2. **WAL replication:** Replicate transactional log to standby.
> 3. **Multi-AZ deployment:** Chạy ở multiple availability zones.
> 4. **Automated failover:** Health check + leader election.
>
> **EventStreamCore:** Single-node hiện tại cần external replication layer. Future: built-in Raft cho transactional log.

---

## Scenario 17: "Optimize cost for cloud deployment."

**Answer:**

> 1. **Right-size instances:** Không dùng instance quá lớn nếu không cần.
> 2. **Spot instances:** Cho batch processing workloads.
> 3. **Tiered storage:** Cold data lên S3.
> 4. **Compression:** LZ4/Snappy cho payloads.
> 5. **Batch egress:** Gom events trước khi gửi đến downstream.
> 6. **Auto-scaling:** Scale ingest workers theo connection count.
>
> **Trade-off:** Cost vs latency. Realtime path cần dedicated resources.

---

## Scenario 18: "Add backpressure across multiple services."

**Answer:**

> **Problem:** Service A gửi event đến EventStreamCore, nhưng downstream Service B chậm.
>
> **Backpressure propagation:**
> 1. **Queue depth signals:** EventStreamCore expose queue depth metrics.
> 2. **Rate limit producer:** Service A giảm rate khi queue depth cao.
> 3. **Credit-based flow control:** Service B cấp "credits" cho EventStreamCore.
> 4. **Circuit breaker:** Nếu Service B down, stop forwarding và buffer/spool.
>
> **Trade-off:** Tight coupling giữa services. Cần careful design để không cascade failure.

---

## Scenario 19: "Design a canary deployment for EventStreamCore."

**Answer:**

> 1. **Deploy 5% traffic** đến new version.
> 2. **Monitor key metrics:** p99 latency, drop rate, error rate, queue depth.
> 3. **Automatic rollback** nếu metrics vượt ngưỡng.
> 4. **Gradual increase:** 5% → 25% → 50% → 100%.
> 5. **Feature flags:** Bật/tắt new features independently.
>
> **EventStreamCore specifics:**
> - Canary dựa trên topic partition hoặc client subset.
> - DLQ metrics quan trọng để phát hiện processing regression.

---

## Scenario 20: "How would you integrate with Kafka?"

**Answer:**

> **EventStreamCore as Kafka producer:**
> - Process events, rồi gửi đến Kafka cho durable stream/replay.
> - Phù hợp cho transactional events.
>
> **EventStreamCore as Kafka consumer:**
> - Nhận events từ Kafka, process với low latency, forward đến realtime consumers.
>
> **Bridge pattern:**
> - `KafkaIngestServer` implement `IngestServer`.
> - `KafkaOutputObserver` subscribe processed events và produce to Kafka.
>
> **Trade-off:** Thêm dependency và latency. Nhưng có durability và ecosystem benefits.

---

## ✅ Enhanced System Design Checklist

- [ ] Geo-distributed design.
- [ ] CDC integration.
- [ ] Schema evolution.
- [ ] Data retention policy.
- [ ] GDPR compliance.
- [ ] Disaster recovery.
- [ ] Cost optimization.
- [ ] Cross-service backpressure.
- [ ] Canary deployment.
- [ ] Kafka integration.
