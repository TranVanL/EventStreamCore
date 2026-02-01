# EventStreamCore Load Testing Suite

Simplified, easy-to-use testing suite with advanced load scenarios.

## 📋 Test Files

### **test_load.py** - Main test file (Run directly!)
- ✅ No pytest needed - runs directly with Python
- ✅ 12 advanced test scenarios
- ✅ 1K → 100K events testing
- ✅ All 5 priority levels (BATCH, LOW, MEDIUM, HIGH, CRITICAL)
- ✅ 12 realistic topics
- ✅ Variable payload sizes (64B → 16KB)
- ✅ Multi-connection stress tests (25 clients)
- ✅ Chaos testing with random parameters
- ✅ Repeated rounds and fairness tests

**Quick Run:**
```bash
# Build first
cmake -G "MinGW Makefiles" -B build -S .
cmake --build build

# Run tests (starts server automatically)
python tests/run_test.py
```

Or run manually:
```bash
# Terminal 1: Start server
.\build\EventStreamCore.exe

# Terminal 2: Run tests
python tests/test_load.py
```

### test_advanced_load.py (Optional - advanced)
- Multi-protocol TCP/UDP
- 100K+ streams
- Latency analysis
- 50 concurrent clients

### test_sustained_load.py (Optional - heavy)
- 500K event streams
- 10 rounds of testing
- Ramp-up scenarios
- Extreme cases

### test_system.py
- System integration tests
- Priority validation
- Connection handling

## 🎯 Test Scenarios in test_load.py

```
1. TCP Basic Connection          - Connectivity test
2. TCP Single Event              - Single message
3. TCP 1K Events                 - 1,000 events, mixed
4. TCP 10K All Combinations      - 5 priorities × 12 topics (60 combos)
5. TCP 50K High Volume           - 50,000 continuous events
6. TCP 100K Continuous           - 100,000 continuous events ⭐
7. TCP Varying Payloads          - 64B, 256B, 1KB, 4KB, 16KB sizes
8. TCP 25 Concurrent Clients     - 25 parallel connections
9. TCP 10 Rounds × 10K           - Repeated test rounds
10. TCP Chaos (30K Random)       - Random everything
11. Priority Fairness (5K each)  - Equal distribution per priority
12. Topics Balanced (1K each)    - Equal distribution per topic
```

## 📊 Expected Results

**Single Connection:**
- 1K-10K events: Fast (< 1 second)
- 50K events: ~5-10 seconds
- 100K events: ~10-20 seconds
- Throughput: 5,000-20,000 events/sec

**Multi-Connection (25 clients):**
- Total: 25,000 events
- Time: 2-5 seconds
- Throughput: 5,000-12,000 events/sec per client

**Chaos Testing (30K random):**
- Mixed payloads, priorities, topics
- Time: 3-8 seconds
- Throughput: 4,000-10,000 events/sec

## 🚀 Usage

### Option 1: Automatic (Recommended)
```bash
python tests/run_test.py
```
Starts server, runs tests, stops server automatically.

### Option 2: Manual
```bash
# Terminal 1
.\build\EventStreamCore.exe

# Terminal 2
python tests/test_load.py
```

### Option 3: Heavy/Advanced Tests
```bash
# Same as above, but also run:
pytest test_advanced_load.py -v -s
pytest test_sustained_load.py -v -s
```
(Requires `pip install pytest`)

## 📈 Output Example

```
============================================================
🧪 EventStreamCore Advanced Load Testing
============================================================

[TEST] Running: TCP Basic Connection...
[✅] ✅ TCP Basic Connection (12.3ms)

[TEST] Running: TCP 100K Continuous...
[⏱]   20000k: 5200 ev/sec
[⏱]   40000k: 5100 ev/sec
[⏱]   60000k: 5300 ev/sec
[⏱]   80000k: 5150 ev/sec
[⏱]   Total: 5250 events/sec
[✅] ✅ TCP 100K Continuous (19034.5ms)

[TEST] Running: TCP 25 Concurrent Clients...
[📊]   25 clients, 25000 events, 8500 ev/sec
[✅] ✅ TCP 25 Concurrent Clients (2934.2ms)

============================================================
📊 SUMMARY: 12 PASS, 0 FAIL (12 total)
🎯 Events processed: 434,000
============================================================
```

## 🐛 Troubleshooting

| Issue | Solution |
|-------|----------|
| "Connection refused" | Port 9000 in use. Check: `netstat -ano \| findstr :9000` |
| "No module named pytest" | Not needed! test_load.py works without pytest |
| Slow throughput | System under load, try again with fewer apps |
| Server won't start | Check `build/EventStreamCore.exe` exists, rebuild if needed |

## 💡 Key Features

✅ **No dependencies** - test_load.py uses only standard library  
✅ **Easy to run** - `python test_load.py` after server starts  
✅ **Clear output** - Shows throughput, latency, event count  
✅ **Advanced scenarios** - 1K to 100K events, multi-protocol, concurrent clients  
✅ **All priorities** - Tests all 5 priority levels  
✅ **Topic variety** - 12 realistic topic patterns  
✅ **Chaos testing** - Random parameters for edge cases  
✅ **Concurrent testing** - Up to 25 parallel clients  

## 📞 Quick Commands

```bash
# View this guide
cat tests/README.md

# Build
cmake --build build

# Run main tests
python tests/test_load.py

# Run with server auto-start
python tests/run_test.py

# Run advanced tests (if pytest installed)
pytest tests/test_advanced_load.py -v -s

# Run sustained load tests (long duration)
pytest tests/test_sustained_load.py -v -s
```

---

**Total Events Tested:** 434,000+ across all scenarios  
**Test Duration:** 2-5 minutes  
**Priorities:** All 5 levels  
**Topics:** 12+ patterns  
**Payloads:** 64B - 16KB  
**Clients:** Up to 25 concurrent  
