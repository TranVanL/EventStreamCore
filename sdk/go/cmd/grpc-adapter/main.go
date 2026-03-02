// gRPC adapter for EventStreamCore.
//
// Exposes the engine over gRPC so any gRPC client (Java, Rust, C#, …)
// can push events and query metrics without linking libesccore directly.
//
// Usage:
//
//	go run ./cmd/grpc-adapter -lib ../../build/libesccore.so -port 50051
package main

import (
	"flag"
	"fmt"
	"log"
	"net"
	"net/http"
	"os"
	"os/signal"
	"syscall"

	esc "github.com/eventstreamcore/sdk-go/esccore"
)

func main() {
	libPath := flag.String("lib", "libesccore.so", "Path to libesccore.so")
	config := flag.String("config", "config/config.yaml", "Path to config YAML")
	grpcPort := flag.Int("port", 50051, "gRPC listen port")
	httpPort := flag.Int("http-port", 8080, "HTTP health/metrics port")
	flag.Parse()

	// Load engine
	engine, err := esc.New(*libPath)
	if err != nil {
		log.Fatalf("Failed to load engine: %v", err)
	}
	if err := engine.Init(*config); err != nil {
		log.Fatalf("Failed to init engine: %v", err)
	}
	defer engine.Shutdown()

	log.Printf("Engine loaded from %s", *libPath)

	// HTTP health + metrics
	go func() {
		mux := http.NewServeMux()

		mux.HandleFunc("/health", func(w http.ResponseWriter, r *http.Request) {
			h, err := engine.Health()
			if err != nil {
				http.Error(w, err.Error(), 500)
				return
			}
			if !h.IsReady {
				w.WriteHeader(503)
			}
			fmt.Fprintf(w, `{"alive":%t,"ready":%t,"backpressure":%d}`,
				h.IsAlive, h.IsReady, h.BackpressureLevel)
		})

		mux.HandleFunc("/metrics", func(w http.ResponseWriter, r *http.Request) {
			text, err := engine.MetricsPrometheus()
			if err != nil {
				http.Error(w, err.Error(), 500)
				return
			}
			w.Header().Set("Content-Type", "text/plain; version=0.0.4")
			w.Write([]byte(text))
		})

		addr := fmt.Sprintf(":%d", *httpPort)
		log.Printf("HTTP health/metrics on %s", addr)
		if err := http.ListenAndServe(addr, mux); err != nil {
			log.Printf("HTTP server error: %v", err)
		}
	}()

	// gRPC placeholder (would use generated proto stubs)
	lis, err := net.Listen("tcp", fmt.Sprintf(":%d", *grpcPort))
	if err != nil {
		log.Fatalf("Failed to listen on port %d: %v", *grpcPort, err)
	}
	log.Printf("gRPC listening on :%d (stub — add proto service)", *grpcPort)

	// Wait for shutdown signal
	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM)
	<-sigCh

	lis.Close()
	log.Println("Shutting down")
}
