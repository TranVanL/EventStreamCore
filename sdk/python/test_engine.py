#!/usr/bin/env python3
"""
Test script for EventStreamCore Python SDK.

This demonstrates how the C++ engine is embedded in Python code:
- The Engine class loads libesccore.dll via ctypes
- C functions are called to initialize the engine
- Python callbacks are registered to receive processed events
- The engine runs its full C++ pipeline in the background
"""

import os
import time
import threading
from esccore import Engine, Priority

# Set the library path (adjust if needed)
lib_path = os.path.join(os.path.dirname(__file__), 'esccore', 'libesccore.dll')
os.environ['ESCCORE_LIB'] = lib_path

def event_callback(event, user_data):
    """Callback function called when the engine processes an event."""
    print(f"Received event: topic='{event.topic.decode()}', "
          f"body='{bytes(event.body[:event.body_len]).decode()}'")
    return 0  # Continue receiving

def main():
    print("Initializing EventStreamCore engine...")

    # Create engine instance - this loads the C++ DLL
    engine = Engine()

    # Initialize with config
    engine.init("config/config.yaml")

    # Subscribe to events with a callback
    engine.subscribe("sensor/", event_callback)

    print("Engine initialized and subscribed to 'sensor/' topics.")
    print("The C++ engine is now running in the background.")
    print("Send events via TCP to 127.0.0.1:9000 to test.")

    # Keep the script running
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("Shutting down...")
        engine.shutdown()

if __name__ == "__main__":
    main()