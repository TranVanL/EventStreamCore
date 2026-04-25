// Package main provides simple microservice samples for EventStreamCore.
//
// Each processor type has a corresponding HTTP microservice:
//   - Realtime Emergency Service  (port 9001)
//   - Transactional Business Service (port 9002)
//   - Batch Analytics Service (port 9003)
//
// Usage:
//
//	go run microservices.go              # Start all 3 services
//	go run microservices.go realtime     # Start only realtime
//	go run microservices.go txn          # Start only transactional
//	go run microservices.go batch        # Start only batch
package main

import (
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"os"
	"strings"
	"sync"
	"time"
)

type EventPayload struct {
	EventID int    `json:"event_id"`
	Topic   string `json:"topic"`
	Reason  string `json:"reason,omitempty"`
}

func ts() string {
	return time.Now().Format("15:04:05.000")
}

func jsonOK(w http.ResponseWriter) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(200)
	w.Write([]byte(`{"status":"ok"}`))
}

func decode(r *http.Request) EventPayload {
	var p EventPayload
	json.NewDecoder(r.Body).Decode(&p)
	return p
}

// ── Realtime Emergency Service ──────────────────────────────────────

func realtimeService() *http.ServeMux {
	mux := http.NewServeMux()

	mux.HandleFunc("/alert/emergency", func(w http.ResponseWriter, r *http.Request) {
		p := decode(r)
		fmt.Printf("[%s] 🚨 EMERGENCY: event_id=%d topic=%s → dispatching\n", ts(), p.EventID, p.Topic)
		jsonOK(w)
	})

	mux.HandleFunc("/alert/monitor", func(w http.ResponseWriter, r *http.Request) {
		p := decode(r)
		fmt.Printf("[%s] 📊 MONITOR: event_id=%d → updating dashboard\n", ts(), p.EventID)
		jsonOK(w)
	})

	mux.HandleFunc("/alert/escalate", func(w http.ResponseWriter, r *http.Request) {
		p := decode(r)
		fmt.Printf("[%s] ⚠️  ESCALATE: event_id=%d reason=%s\n", ts(), p.EventID, p.Reason)
		jsonOK(w)
	})

	return mux
}

// ── Transactional Business Service ──────────────────────────────────

func transactionalService() *http.ServeMux {
	mux := http.NewServeMux()

	mux.HandleFunc("/payment/status", func(w http.ResponseWriter, r *http.Request) {
		p := decode(r)
		fmt.Printf("[%s] 💳 PAYMENT: event_id=%d → status COMPLETED\n", ts(), p.EventID)
		jsonOK(w)
	})

	mux.HandleFunc("/state/change", func(w http.ResponseWriter, r *http.Request) {
		p := decode(r)
		fmt.Printf("[%s] 🔄 STATE: event_id=%d → recorded\n", ts(), p.EventID)
		jsonOK(w)
	})

	mux.HandleFunc("/audit/compliance", func(w http.ResponseWriter, r *http.Request) {
		p := decode(r)
		fmt.Printf("[%s] 📋 AUDIT: event_id=%d → compliance\n", ts(), p.EventID)
		jsonOK(w)
	})

	mux.HandleFunc("/txn/failed", func(w http.ResponseWriter, r *http.Request) {
		p := decode(r)
		fmt.Printf("[%s] ❌ FAILED: event_id=%d reason=%s\n", ts(), p.EventID, p.Reason)
		jsonOK(w)
	})

	return mux
}

// ── Batch Analytics Service ─────────────────────────────────────────

func batchService() *http.ServeMux {
	mux := http.NewServeMux()

	mux.HandleFunc("/analytics/ingest", func(w http.ResponseWriter, r *http.Request) {
		p := decode(r)
		fmt.Printf("[%s] 📈 ANALYTICS: event_id=%d topic=%s → ingested\n", ts(), p.EventID, p.Topic)
		jsonOK(w)
	})

	mux.HandleFunc("/warehouse/store", func(w http.ResponseWriter, r *http.Request) {
		p := decode(r)
		fmt.Printf("[%s] 🏪 WAREHOUSE: event_id=%d topic=%s → stored\n", ts(), p.EventID, p.Topic)
		jsonOK(w)
	})

	mux.HandleFunc("/batch/dropped", func(w http.ResponseWriter, r *http.Request) {
		p := decode(r)
		fmt.Printf("[%s] ⚠️  BATCH DROP: event_id=%d reason=%s\n", ts(), p.EventID, p.Reason)
		jsonOK(w)
	})

	return mux
}

type service struct {
	name string
	mux  *http.ServeMux
	port int
}

func main() {
	all := map[string]service{
		"realtime": {"Realtime Emergency Service", realtimeService(), 9001},
		"txn":      {"Transactional Business Service", transactionalService(), 9002},
		"batch":    {"Batch Analytics Service", batchService(), 9003},
	}

	requested := os.Args[1:]
	if len(requested) == 0 {
		requested = []string{"realtime", "txn", "batch"}
	}

	fmt.Println(strings.Repeat("=", 60))
	fmt.Println("  EventStreamCore — Sample Microservices (Go)")
	fmt.Println(strings.Repeat("=", 60))

	var wg sync.WaitGroup
	for _, key := range requested {
		svc, ok := all[key]
		if !ok {
			fmt.Printf("Unknown service: %s\n", key)
			continue
		}
		wg.Add(1)
		go func(s service) {
			defer wg.Done()
			fmt.Printf("✅ %s listening on port %d\n", s.name, s.port)
			log.Fatal(http.ListenAndServe(fmt.Sprintf(":%d", s.port), s.mux))
		}(svc)
	}

	fmt.Printf("\nRunning %d service(s). Press Ctrl+C to stop.\n\n", len(requested))
	wg.Wait()
}
