// Command relay is the Def Jam: Fight for NY netplay relay server
// (protocol version 1 with the lobby extension, docs/netplay-protocol.md). It
// puts two to four players into a room and forwards their opaque packets. It
// keeps no state on disk.
//
//	relay -listen :27777 -max-rooms 512 -max-clients 1024 -log-interval 60s
package main

import (
	"context"
	"flag"
	"fmt"
	"log"
	"net"
	"net/http"
	"os"
	"os/signal"
	"path/filepath"
	"regexp"
	"syscall"
	"time"

	"defjam/netplay/relay"
)

// updateName is the only shape of path the update service answers: one plain
// file name in the update folder, never a sub-folder or a dot-dot.
var updateName = regexp.MustCompile(`^/[A-Za-z0-9][A-Za-z0-9._-]{0,95}$`)

// serveUpdates answers GET and HEAD for the files in dir (latest.json, its
// signature and the setups it names) and nothing else. The launcher checks the
// signature itself, so plain HTTP is enough here: the files are public and a
// tampered one is refused by every client.
func serveUpdates(ctx context.Context, listen, dir string) {
	mux := http.NewServeMux()
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet && r.Method != http.MethodHead {
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}
		if !updateName.MatchString(r.URL.Path) {
			http.NotFound(w, r)
			return
		}
		path := filepath.Join(dir, r.URL.Path[1:])
		st, err := os.Stat(path)
		if err != nil || !st.Mode().IsRegular() {
			http.NotFound(w, r)
			return
		}
		w.Header().Set("Cache-Control", "no-cache")
		w.Header().Set("X-Content-Type-Options", "nosniff")
		http.ServeFile(w, r, path)
	})
	srv := &http.Server{
		Addr:              listen,
		Handler:           mux,
		ReadHeaderTimeout: 5 * time.Second,
		ReadTimeout:       10 * time.Second,
		WriteTimeout:      15 * time.Minute, // a setup of ~55 MB on a slow line
		IdleTimeout:       30 * time.Second,
		MaxHeaderBytes:    8 << 10,
	}
	go func() {
		<-ctx.Done()
		shutdown, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		_ = srv.Shutdown(shutdown)
	}()
	log.Printf("relay: update files from %s on http %s", dir, listen)
	if err := srv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		log.Printf("relay: update service stopped: %v", err)
	}
}

func main() {
	listen := flag.String("listen", fmt.Sprintf(":%d", relay.DefaultPort), "UDP address to listen on")
	maxRooms := flag.Int("max-rooms", 512, "maximum number of open rooms")
	maxClients := flag.Int("max-clients", 1024, "maximum number of connected clients")
	logInterval := flag.Duration("log-interval", 60*time.Second, "period of the summary log line (0 disables)")
	updatesListen := flag.String("updates-listen", "", "TCP address for the launcher's update files, e.g. :27778 (empty: off)")
	updatesDir := flag.String("updates-dir", "", "folder holding latest.json, latest.json.sig and the setups")
	accountsURL := flag.String("accounts-internal", "", "the account service's local internal listener for ranked play, e.g. http://127.0.0.1:8091 (empty: ranked off)")
	flag.Parse()

	if *maxRooms < 1 || *maxClients < 2 {
		log.Fatalf("relay: -max-rooms must be >= 1 and -max-clients >= 2")
	}
	li := *logInterval
	if li == 0 {
		li = -1 // ServerConfig: negative disables, zero means default
	}

	addr, err := net.ResolveUDPAddr("udp", *listen)
	if err != nil {
		log.Fatalf("relay: bad -listen %q: %v", *listen, err)
	}
	conn, err := net.ListenUDP("udp", addr)
	if err != nil {
		log.Fatalf("relay: listen: %v", err)
	}
	// Generous kernel buffers absorb bursts; failures here are harmless.
	_ = conn.SetReadBuffer(1 << 20)
	_ = conn.SetWriteBuffer(1 << 20)

	cfg := relay.ServerConfig{
		Relay:       relay.Config{MaxRooms: *maxRooms, MaxClients: *maxClients},
		LogInterval: li,
	}
	if *accountsURL != "" {
		cfg.Accounts = relay.AccountsHTTP{Base: *accountsURL, Client: &http.Client{Timeout: 10 * time.Second}}
		log.Printf("relay: ranked play on, accounts at %s", *accountsURL)
	}
	srv, err := relay.NewServer(cfg)
	if err != nil {
		log.Fatalf("relay: %v", err)
	}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	log.Printf("relay: protocol v%d listening on %s (max-rooms=%d max-clients=%d)",
		relay.Version, conn.LocalAddr(), *maxRooms, *maxClients)
	if *updatesListen != "" && *updatesDir != "" {
		go serveUpdates(ctx, *updatesListen, *updatesDir)
	}
	if err := srv.Serve(ctx, conn); err != nil {
		log.Fatalf("relay: %v", err)
	}
	rooms, clients := srv.Snapshot()
	log.Printf("relay: shut down (dropping %d rooms, %d clients)", rooms, clients)
}

