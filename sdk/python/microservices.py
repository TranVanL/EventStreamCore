#!/usr/bin/env python3
"""
Simple microservice samples for EventStreamCore.

Each "microservice" is a minimal HTTP server that represents
what an observer would call in a real deployment.

Usage:
    python3 microservices.py              # Start all 3 services
    python3 microservices.py realtime     # Start only realtime service
    python3 microservices.py txn          # Start only transactional service  
    python3 microservices.py batch        # Start only batch service
"""

import json
import sys
import threading
from http.server import HTTPServer, BaseHTTPRequestHandler
from datetime import datetime


# ── Realtime Emergency Service (port 9001) ───────────────────────────

class RealtimeHandler(BaseHTTPRequestHandler):
    """Handles alerts from RealtimeProcessor observer.
    
    Routes:
        POST /alert/emergency   → sensor/pressure emergency
        POST /alert/monitor     → sensor/temperature monitoring
        POST /alert/escalate    → dropped event escalation
    """
    
    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length) if length else b""
        
        try:
            data = json.loads(body) if body else {}
        except json.JSONDecodeError:
            data = {"raw": body.decode(errors="replace")}
        
        ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
        
        if self.path == "/alert/emergency":
            print(f"[{ts}] 🚨 EMERGENCY: pressure event_id={data.get('event_id')} "
                  f"topic={data.get('topic')} → dispatching emergency response")
        elif self.path == "/alert/monitor":
            print(f"[{ts}] 📊 MONITOR: temperature event_id={data.get('event_id')} "
                  f"→ updating dashboard")
        elif self.path == "/alert/escalate":
            print(f"[{ts}] ⚠️  ESCALATE: dropped event_id={data.get('event_id')} "
                  f"reason={data.get('reason')}")
        else:
            print(f"[{ts}] ❓ Unknown route: {self.path}")
        
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(json.dumps({"status": "ok"}).encode())
    
    def log_message(self, format, *args):
        pass  # Suppress default logging


# ── Transactional Business Service (port 9002) ──────────────────────

class TransactionalHandler(BaseHTTPRequestHandler):
    """Handles business events from TransactionalProcessor observer.
    
    Routes:
        POST /payment/status    → update payment status
        POST /state/change      → state change notification
        POST /audit/compliance  → compliance forwarding
        POST /txn/failed        → failed transaction logging
    """
    
    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length) if length else b""
        
        try:
            data = json.loads(body) if body else {}
        except json.JSONDecodeError:
            data = {"raw": body.decode(errors="replace")}
        
        ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
        
        if self.path == "/payment/status":
            print(f"[{ts}] 💳 PAYMENT: event_id={data.get('event_id')} "
                  f"→ status updated to COMPLETED")
        elif self.path == "/state/change":
            print(f"[{ts}] 🔄 STATE: event_id={data.get('event_id')} "
                  f"→ state change recorded")
        elif self.path == "/audit/compliance":
            print(f"[{ts}] 📋 AUDIT: event_id={data.get('event_id')} "
                  f"→ forwarded to compliance")
        elif self.path == "/txn/failed":
            print(f"[{ts}] ❌ FAILED: event_id={data.get('event_id')} "
                  f"reason={data.get('reason')}")
        else:
            print(f"[{ts}] ❓ Unknown route: {self.path}")
        
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(json.dumps({"status": "ok"}).encode())
    
    def log_message(self, format, *args):
        pass


# ── Batch Analytics Service (port 9003) ─────────────────────────────

class BatchHandler(BaseHTTPRequestHandler):
    """Handles batch events from BatchProcessor observer.
    
    Routes:
        POST /analytics/ingest  → analytics pipeline ingestion
        POST /warehouse/store   → data warehouse storage
        POST /batch/dropped     → dropped batch logging
    """
    
    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length) if length else b""
        
        try:
            data = json.loads(body) if body else {}
        except json.JSONDecodeError:
            data = {"raw": body.decode(errors="replace")}
        
        ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
        
        if self.path == "/analytics/ingest":
            print(f"[{ts}] 📈 ANALYTICS: event_id={data.get('event_id')} "
                  f"topic={data.get('topic')} → ingested into pipeline")
        elif self.path == "/warehouse/store":
            print(f"[{ts}] 🏪 WAREHOUSE: event_id={data.get('event_id')} "
                  f"topic={data.get('topic')} → stored")
        elif self.path == "/batch/dropped":
            print(f"[{ts}] ⚠️  BATCH DROP: event_id={data.get('event_id')} "
                  f"reason={data.get('reason')}")
        else:
            print(f"[{ts}] ❓ Unknown route: {self.path}")
        
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(json.dumps({"status": "ok"}).encode())
    
    def log_message(self, format, *args):
        pass


def start_server(name, handler_class, port):
    server = HTTPServer(("0.0.0.0", port), handler_class)
    print(f"✅ {name} listening on port {port}")
    server.serve_forever()


def main():
    services = {
        "realtime": ("Realtime Emergency Service",  RealtimeHandler,      9001),
        "txn":      ("Transactional Business Service", TransactionalHandler, 9002),
        "batch":    ("Batch Analytics Service",      BatchHandler,         9003),
    }
    
    # Parse which services to start
    requested = sys.argv[1:] if len(sys.argv) > 1 else list(services.keys())
    
    print("=" * 60)
    print("  EventStreamCore — Sample Microservices (Python)")
    print("=" * 60)
    
    threads = []
    for key in requested:
        if key not in services:
            print(f"Unknown service: {key}. Available: {list(services.keys())}")
            continue
        name, handler, port = services[key]
        t = threading.Thread(target=start_server, args=(name, handler, port), daemon=True)
        t.start()
        threads.append(t)
    
    if not threads:
        print("No services started.")
        return
    
    print(f"\nRunning {len(threads)} service(s). Press Ctrl+C to stop.\n")
    
    try:
        for t in threads:
            t.join()
    except KeyboardInterrupt:
        print("\n\nShutting down microservices...")


if __name__ == "__main__":
    main()
