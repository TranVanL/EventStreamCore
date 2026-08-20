# Phase 8 — Cross-Compilation, Docker, CI

## Mục tiêu

Thiết lập cross-compile cho QNX, ARM64/musl, Docker build environment, và CI pipeline.

---

## 1. CMake Toolchain Files

### ARM64 musl

```cmake
# toolchains/aarch64-linux-musl.cmake
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-musl-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-musl-g++)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
```

### QNX 7.1

```cmake
# toolchains/qnx710.cmake
set(CMAKE_SYSTEM_NAME QNX)
set(CMAKE_SYSTEM_VERSION 7.1)
set(CMAKE_C_COMPILER qcc)
set(CMAKE_CXX_COMPILER q++)
set(CMAKE_CXX_FLAGS "-Vgcc_ntoaarch64le")
set(CMAKE_C_FLAGS "-Vgcc_ntoaarch64le")
```

### Build

```bash
cmake -B build-arm64 -DCMAKE_TOOLCHAIN_FILE=toolchains/aarch64-linux-musl.cmake .
cmake --build build-arm64 -j$(nproc)

cmake -B build-qnx -DCMAKE_TOOLCHAIN_FILE=toolchains/qnx710.cmake .
cmake --build build-qnx -j$(nproc)
```

---

## 2. Docker Build Environment

### Dockerfile

```dockerfile
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y \
    build-essential cmake ninja-build \
    g++-aarch64-linux-gnu \
    qemu-user-static \
    liburing-dev \
    git

WORKDIR /workspace
COPY . .
RUN cmake -B build -G Ninja . && cmake --build build
```

### docker-compose.yml

```yaml
version: '3.8'
services:
  build:
    build: .
    volumes:
      - .:/workspace
  arm64:
    build:
      context: .
      dockerfile: Dockerfile.arm64
    volumes:
      - .:/workspace
```

---

## 3. CI Pipeline (GitHub Actions)

```yaml
name: CI
on: [push, pull_request]

jobs:
  build-linux:
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@v4
      - name: Install deps
        run: sudo apt-get update && sudo apt-get install -y liburing-dev
      - name: Configure
        run: cmake -B build -DCMAKE_BUILD_TYPE=Release
      - name: Build
        run: cmake --build build -j$(nproc)
      - name: Test
        run: ctest --test-dir build --output-on-failure

  build-arm64:
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@v4
      - name: Install cross compiler
        run: sudo apt-get install -y g++-aarch64-linux-gnu
      - name: Cross compile
        run: |
          cmake -B build-arm64 -DCMAKE_TOOLCHAIN_FILE=toolchains/aarch64-linux-musl.cmake
          cmake --build build-arm64 -j$(nproc)

  build-qnx:
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@v4
      - name: Compile only QNX
        run: |
          source /opt/qnx710/qnxsdp-env.sh
          cmake -B build-qnx -DCMAKE_TOOLCHAIN_FILE=toolchains/qnx710.cmake
          cmake --build build-qnx -j$(nproc)
```

---

## 4. Feature Detection

```cmake
include(CheckSymbolExists)
check_symbol_exists(io_uring_setup "liburing.h" HAS_IO_URING)
check_symbol_exists(timerfd_create "sys/timerfd.h" HAS_TIMERFD)
check_symbol_exists(eventfd "sys/eventfd.h" HAS_EVENTFD)

if(HAS_IO_URING)
    target_compile_definitions(eventstream PRIVATE ESC_HAS_IO_URING)
endif()
```

---

## 5. Static Analysis trong CI

```yaml
  static-analysis:
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@v4
      - name: cppcheck
        run: cppcheck --enable=all --error-exitcode=1 src/
      - name: clang-tidy
        run: |
          cmake -B build -DCMAKE_CXX_CLANG_TIDY="clang-tidy"
          cmake --build build -j$(nproc)
```

---

## 6. Interview Q&A

**Q: Cross-compile khác native compile chỗ nào?**
A: Compiler chạy trên host nhưng tạo binary cho target. Cần toolchain file, sysroot, và không chạy test trên host.

**Q: Tại sao dùng Docker cho build?**
A: Reproducible environment, dễ share, tránh "works on my machine".

**Q: CI pipeline nên có những gì?**
A: Build, test, cross-compile, static analysis, sanitizer.

**Q: Làm sao xử lý feature không có trên QNX?**
A: CMake feature detection + `#ifdef` + policy-based abstraction.

---

## 7. References

- CMake Cross Compiling documentation
- GitHub Actions documentation
- Docker multi-stage builds
