# QNX Senior Features

> **Scope:** QNX Neutrino concepts beyond basic message passing: microkernel architecture, Adaptive Partitioning Scheduler (APS), Persistent Publish/Subscribe (PPS), Image File System (IFS), resource manager lifecycle, debugging, and safety certification path.
> **Target audience:** Senior embedded/RTOS candidates who need to defend QNX knowledge at depth.

---

## 1. Why This Module Exists

`PHASE03_QNX_PORTABILITY.md` covers the basics:

- `ChannelCreate` / `MsgSend` / `MsgReceive`
- Resource manager pattern
- Interrupt-to-thread pulse

That is enough for a junior/mid-level QNX conversation, but senior interviews often dig into:

- **Adaptive Partitioning Scheduler** — QNX's unique CPU budgeting.
- **PPS** — persistent publish/subscribe used in QNX CAR.
- **IFS / buildfile** — how a QNX image is constructed.
- **Resource manager lifecycle** — attach, mount, ocb, detach.
- **Safety** — QNX SDP 8.0 and ASIL D.

This module adds those senior-level topics to EventStreamCore.

---

## 2. QNX Microkernel Architecture

### 2.1 procnto

QNX kernel is called **procnto**. It combines:

- Process manager
- Memory manager
- Pathspace manager

Everything else — drivers, file systems, network stack — runs in user space as processes or resource managers.

### 2.2 User-Space Drivers

Because QNX is a microkernel, drivers are user-space processes. A crash in a driver does not crash the kernel.

**Implication for EventStreamCore:** the engine can itself be structured as a resource manager (`/dev/eventstream`) so other processes open/read/write it like a device.

---

## 3. Adaptive Partitioning Scheduler (APS)

### 3.1 Concept

APS guarantees a minimum CPU budget to a set of threads, even when the system is overloaded.

- **Partition**: group of threads with a CPU budget.
- **Budget**: percentage of CPU time reserved.
- **Critical budget**: maximum time a critical thread can run before the system takes action.

### 3.2 API

```cpp
#ifdef __QNX__
#include <sys/sched_aps.h>

struct sched_aps_create_parms create_parms;
memset(&create_parms, 0, sizeof(create_parms));
strcpy(create_parms.name, "eventstream");
create_parms.budget_percent = 30;      // 30% CPU budget
create_parms.critical_budget_ms = 5;   // critical budget

int partition_id;
int ret = sched_aps_create(&partition_id, &create_parms);
if (ret != 0) { /* handle error */ }

// Move current thread into partition
sched_aps_join_partition(0, partition_id);
#endif
```

### 3.3 Use Case in EventStreamCore

| Partition | Budget | Threads |
|-----------|--------|---------|
| `eventstream_rt` | 40% | ingest, dispatcher, realtime processor |
| `eventstream_tx` | 20% | transactional processor |
| `eventstream_bg` | 10% | batch, storage, metrics |
| `system` | 30% | other system tasks |

If a non-critical task goes into a tight loop, the `eventstream_rt` partition still gets its 40%.

### 3.4 Critical Threads

A thread marked critical can borrow from the system partition if its own partition runs out of budget. This prevents critical deadlines from being missed due to budget exhaustion.

---

## 4. Persistent Publish/Subscribe (PPS)

### 4.1 Concept

PPS is a QNX service that provides persistent, hierarchical objects that multiple processes can read/write.

- Publisher writes attributes.
- Subscribers receive notifications on change.
- Objects persist across publisher restarts.

Common in QNX CAR infotainment for vehicle data (speed, gear, HVAC).

### 4.2 API Pattern

```cpp
#ifdef __QNX__
#include <sys/pps.h>

// Open PPS object for writing
int fd = open("/pps/eventstream/metrics", O_RDWR | O_CREAT);
write(fd, "events_ingested::12345\n", ...);

// Subscriber opens with O_RDONLY and uses ionotify/select
#endif
```

### 4.3 EventStreamCore Use

Expose runtime metrics via PPS so QNX CAR dashboards or diagnostic tools can subscribe:

```
/pps/eventstream/metrics
    events_ingested::1234567
    events_dropped::12
    p99_latency_us::18
```

---

## 5. QNX Image File System (IFS)

### 5.1 Buildfile

A QNX image is built from a `.build` file that lists:

- Startup program (`startup-bios`, `startup-xxx`)
- procnto
- Shared libraries
- Drivers and user applications
- File system mounts

Example snippet:

```buildfile
[image=0x100000]
[virtual=x86,bios] .bootstrap = {
    startup-bios
    PATH=/proc/boot:/bin:/usr/bin
    LD_LIBRARY_PATH=/proc/boot:/lib:/usr/lib
    procnto
}

[+script] .script = {
    devc-ser8250 -e -b115200 &
    waitfor /dev/ser1
    display_msg EventStreamCore starting...
    eventstream --config /etc/eventstream.yaml &
}

[eventstream]
eventstream

[libs]
libeventstream.so
libc.so
libcpp.so
```

### 5.2 EventStreamCore Deployment

For a production QNX target, EventStreamCore would be:

1. Cross-compiled with QNX SDP toolchain.
2. Added to the IFS buildfile.
3. Started by an `init` script.
4. Registered as a resource manager at `/dev/eventstream`.

---

## 6. Resource Manager Lifecycle

### 6.1 Full Setup

```cpp
#ifdef __QNX__
#include <sys/iofunc.h>
#include <sys/dispatch.h>

resmgr_attr_t resmgr_attr;
iofunc_attr_t iofunc_attr;
resmgr_connect_funcs_t connect_funcs;
resmgr_io_funcs_t io_funcs;
dispatch_t* dpp;

int eventstream_read(resmgr_context_t* ctp, io_read_t* msg, RESMGR_OCB_T* ocb) {
    // reply with data
    return _RESMGR_NPARTS(0);
}

int eventstream_write(resmgr_context_t* ctp, io_write_t* msg, RESMGR_OCB_T* ocb) {
    // process incoming event
    return _RESMGR_NPARTS(0);
}

int setup_resource_manager() {
    dpp = dispatch_create();
    if (!dpp) return -1;

    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect_funcs,
                     _RESMGR_IO_NFUNCS, &io_funcs);
    io_funcs.read = eventstream_read;
    io_funcs.write = eventstream_write;

    iofunc_attr_init(&iofunc_attr, S_IFCHR | 0666, nullptr, nullptr);

    memset(&resmgr_attr, 0, sizeof(resmgr_attr));
    resmgr_attr.nparts_max = 1;
    resmgr_attr.msg_max_size = 2048;

    int id = resmgr_attach(dpp, &resmgr_attr, "/dev/eventstream",
                           _FTYPE_ANY, _RESMGR_FLAG_BEFORE,
                           &connect_funcs, &io_funcs, &iofunc_attr);
    if (id == -1) return -1;

    return 0;
}

void run_dispatch_loop() {
    while (1) {
        dispatch_context_t* ctp = dispatch_block(dpp);
        if (ctp) dispatch_handler(dpp, ctp);
    }
}
#endif
```

### 6.2 Open Control Block (OCB)

Each `open()` creates an OCB. The resource manager can store per-client state there.

### 6.3 Teardown

```cpp
resmgr_detach(dpp, id, 0);
dispatch_destroy(dpp);
```

---

## 7. QNX Timers

### 7.1 `ClockCycles()`

High-resolution timestamp using the hardware counter.

```cpp
#ifdef __QNX__
#include <sys/neutrino.h>

uint64_t now = ClockCycles();
uint64_t freq = SYSPAGE_ENTRY(qtime)->cycles_per_sec;
double seconds = (double)(now) / freq;
#endif
```

### 7.2 `timer_create()` with `SIGEV_THREAD`

QNX can deliver timer expiry as a thread notification instead of a signal.

```cpp
#ifdef __QNX__
struct sigevent event;
SIGEV_THREAD_INIT(&event, timer_thread_handler, arg, nullptr);

timer_t timerid;
struct itimerspec its = { ... };
timer_create(CLOCK_MONOTONIC, &event, &timerid);
timer_settime(timerid, 0, &its, nullptr);
#endif
```

---

## 8. QNX Interrupt Model

### 8.1 ISR + Pulse

```cpp
#ifdef __QNX__
const struct sigevent* isr(void* area, int id) {
    struct sigevent* event = static_cast<struct sigevent*>(area);
    return event;  // sends pulse to thread
}

struct sigevent event;
SIGEV_PULSE_INIT(&event, coid, SIGEV_PULSE_PRIO_INHERIT, _PULSE_CODE_MINAVAIL, 0);

int id = InterruptAttach(irq, isr, &event, sizeof(event), 0);
#endif
```

### 8.2 EventStreamCore Interrupt Stub

`QnxInterrupt` provides a placeholder that can be wired to a hardware interrupt source. On Linux it is a no-op compile stub.

---

## 9. QNX Debugging

| Tool | Purpose |
|------|---------|
| `pidin` | List processes and threads |
| `sloginfo` | System log |
| `tracelogger` | Kernel event trace |
| `qconn` + `gdb` | Remote debugging |
| `dumper` | Crash dump |

EventStreamCore logs to `sloginfo` on QNX via `slogf()` and to `spdlog` on Linux.

---

## 10. QNX Safety Certification

### 10.1 QNX SDP 8.0

- Targets **ASIL D** functional safety.
- Includes safety manual and evidence package.
- Supports QNX Hypervisor for mixed-criticality systems.

### 10.2 EventStreamCore Safety Stance

EventStreamCore does not claim ISO 26262 certification. It documents:

- Static analysis with clang-tidy / cppcheck.
- No-malloc hot path.
- Bounded queues.
- Watchdog + deadline monitor.
- Fail-safe state transitions.

These are steps toward a safety argument, not a certified product.

---

## 11. QNX vs Linux Summary

| Aspect | Linux + PREEMPT_RT | QNX Neutrino |
|--------|-------------------|--------------|
| Kernel | Monolithic + RT patch | Microkernel |
| Drivers | In-kernel | User-space resource managers |
| IPC | Sockets, pipes, POSIX MQ | Message passing (Channel/ConnectAttach) |
| CPU budgeting | `cgroups` / `sched_deadline` | Adaptive Partitioning Scheduler |
| Persistent pub/sub | D-Bus, MQTT | PPS |
| Image build | Rootfs tarball | IFS buildfile |
| Interrupt latency | ~10–100 µs | ~1–10 µs |
| Safety path | Limited | QNX SDP 8.0 → ASIL D |

---

## 12. Interview Q&A

**Q: What is QNX Adaptive Partitioning and why use it?**

> A: APS reserves a CPU budget for a group of threads. Even if the rest of the system is overloaded, the partition still gets its guaranteed share. In EventStreamCore I would put realtime ingest/processing in a partition with ~40% budget so that background batch work cannot starve it.

**Q: How does QNX PPS differ from Linux D-Bus?**

> A: PPS is a file-system-like persistent publish/subscribe service. Objects live as files under `/pps/...`; publishers write attributes, subscribers get notifications. It is simpler and more deterministic than D-Bus, which is common in automotive QNX CAR systems.

**Q: Walk through a QNX resource manager lifecycle.**

> A: Create a dispatch context, initialize iofunc and resmgr attributes, attach a path like `/dev/eventstream`, define read/write/open/close handlers, then run `dispatch_block`/`dispatch_handler`. Each `open()` creates an OCB for per-client state. Cleanup calls `resmgr_detach` and `dispatch_destroy`.

**Q: What is in a QNX IFS buildfile?**

> A: The buildfile lists the startup program, procnto, shared libraries, drivers, scripts, and applications that make up the boot image. EventStreamCore would appear as an application entry started by an init script.

**Q: How do you measure high-resolution time on QNX?**

> A: Use `ClockCycles()` to read the hardware counter and divide by `SYSPAGE_ENTRY(qtime)->cycles_per_sec` to get seconds. For periodic timers, `timer_create` with `SIGEV_THREAD` delivers expiry in a dedicated thread.

**Q: What is the QNX interrupt model?**

> A: The ISR runs in kernel space and must be very short. It returns a `sigevent` that sends a pulse to a user-space thread, which does the real work. This keeps interrupt latency low and deterministic.

---

## 13. References

- QNX SDP 7.1 / 8.0 Documentation
- QNX Neutrino RTOS System Architecture
- QNX Adaptive Partitioning User's Guide
- QNX Persistent Publish/Subscribe Developer's Guide
- QNX Image File System Developer's Guide
