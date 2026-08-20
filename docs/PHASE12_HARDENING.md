# Phase 12 — Hardening, Sanitizers, Static Analysis, Final Polish

## Mục tiêu

Đảm bảo code chất lượng production: sanitizer, static analysis, benchmark, fuzzing, documentation, release.

---

## 1. Sanitizers

### AddressSanitizer (ASAN)

```bash
cmake -B build-asan -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer" .
cmake --build build-asan -j$(nproc)
ctest --test-dir build-asan --output-on-failure
```

- Phát hiện use-after-free, buffer overflow, memory leak.

### ThreadSanitizer (TSAN)

```bash
cmake -B build-tsan -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" .
```

- Phát hiện data race.

### UndefinedBehaviorSanitizer (UBSAN)

```bash
cmake -B build-ubsan -DCMAKE_CXX_FLAGS="-fsanitize=undefined -fno-omit-frame-pointer" .
```

- Phát hiện signed overflow, shift quá, null dereference.

---

## 2. Static Analysis

### clang-tidy

```yaml
Checks: >
  bugprone-*,
  cppcoreguidelines-*,
  performance-*,
  portability-*,
  concurrency-*,
  modernize-*,
  clang-analyzer-*
```

```bash
clang-tidy src/**/*.cpp -- -Iinclude
```

### cppcheck

```bash
cppcheck --enable=all --suppress=missingIncludeSystem src/
```

### Coverity / SonarQube

- Dùng cho commercial-grade analysis.
- Tích hợp vào CI.

---

## 3. Fuzzing

### libFuzzer

```cpp
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    ProtocolParser parser;
    parser.feed(data, size);
    return 0;
}
```

```bash
clang++ -fsanitize=fuzzer,address fuzz_parser.cpp -o fuzz_parser
./fuzz_parser -max_total_time=300
```

### AFL++

```bash
afl-clang-fast++ parser_fuzz.cpp -o afl_parser
afl-fuzz -i inputs -o outputs ./afl_parser
```

---

## 4. Benchmarking

### Google Benchmark

```cpp
static void BM_MpscQueuePushPop(benchmark::State& state) {
    MpscQueue<int, 1024> q;
    for (auto _ : state) {
        q.push(42);
        q.pop();
    }
}
BENCHMARK(BM_MpscQueuePushPop);
```

### perf

```bash
perf record -g ./benchmark
perf report
```

### cyclictest

```bash
sudo cyclictest -m -S -p 80 -i 200 -l 100000
```

---

## 5. Code Quality Checklist

- [ ] Không có raw `new/delete` trong hot path.
- [ ] Mọi lock-free path đều có memory ordering đúng.
- [ ] Không data race theo TSAN.
- [ ] Không memory leak theo ASAN.
- [ ] Cross-compile Linux/ARM64/QNX thành công.
- [ ] Unit test coverage > 80%.
- [ ] End-to-end demo chạy được.
- [ ] README và architecture docs đầy đủ.

---

## 6. Release Checklist

- [ ] Version bump (semver).
- [ ] CHANGELOG.md.
- [ ] Git tag.
- [ ] Docker image build.
- [ ] GitHub Release với binary artifacts.
- [ ] Benchmark report.

---

## 7. Final Polish

### Formatting

```bash
find src -name '*.cpp' -o -name '*.h' | xargs clang-format -i
```

### Pre-commit hooks

```yaml
# .pre-commit-config.yaml
repos:
  - repo: https://github.com/pre-commit/mirrors-clang-format
    hooks:
      - id: clang-format
  - repo: local
    hooks:
      - id: cppcheck
        entry: cppcheck
        language: system
        files: \.(cpp|h)$
```

---

## 8. Interview Q&A

**Q: ASAN vs TSAN khác nhau?**
A: ASAN phát hiện memory errors, TSAN phát hiện data races.

**Q: Tại sao cần static analysis?**
A: Bắt lỗi sớm, đảm bảo coding standard, tìm bug khó phát hiện bằng test.

**Q: Fuzzing có ích gì?**
A: Tự động tìm input bất thường gây crash hoặc lỗi parser.

**Q: cyclictest đo cái gì?**
A: Đo scheduling latency của real-time thread.

---

## 9. References

- Google Sanitizers documentation
- clang-tidy checks list
- AFL++ documentation
- Google Benchmark
- rt-tests (cyclictest)
