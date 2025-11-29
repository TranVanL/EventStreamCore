
# EventStream Core

EventStream Core is a high-performance real-time event processing engine
written in modern C++20.  
It supports:

- High-throughput event ingestion  
- Rule engine (AST-based)  
- Plug-in architecture (C++ and Python)  
- Persistent storage  
- Realtime broadcasting  

## Day 1 – Bootstrap

This version includes:

- Project structure
- CMake build system
- Basic main.cpp with logging (spdlog)

## Build

```bash
cmake -S . -B build
cmake --build build -j
