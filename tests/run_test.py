#!/usr/bin/env python3
"""
Quick Start Guide - Run tests ngay
"""

import subprocess
import sys
import time
import os
from pathlib import Path

PROJECT_ROOT = Path(__file__).parent.parent
EXECUTABLE = PROJECT_ROOT / "build" / "EventStreamCore.exe"


def main():
    print("\n" + "="*70)
    print("🚀 EventStreamCore Advanced Load Test")
    print("="*70)
    
    # Check if executable exists
    if not EXECUTABLE.exists():
        print(f"❌ Executable not found: {EXECUTABLE}")
        print("\nBuild project first:")
        print("  cmake -G \"MinGW Makefiles\" -B build -S .")
        print("  cmake --build build")
        return 1
    
    # Start server
    print("\n📌 Starting server...")
    server = subprocess.Popen(
        [str(EXECUTABLE)],
        cwd=str(PROJECT_ROOT),
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL
    )
    
    time.sleep(2)  # Wait for server to start
    
    if server.poll() is not None:
        print("❌ Server failed to start")
        return 1
    
    print("✅ Server started (PID: {})".format(server.pid))
    
    # Run tests
    print("\n📊 Running tests...\n")
    
    try:
        result = subprocess.run(
            [sys.executable, str(Path(__file__).parent / "test_load.py")],
            cwd=str(PROJECT_ROOT)
        )
        
        return result.returncode
    
    finally:
        print("\n🛑 Stopping server...")
        server.terminate()
        try:
            server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()


if __name__ == "__main__":
    sys.exit(main())
