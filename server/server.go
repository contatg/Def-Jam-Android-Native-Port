package relay

import (
	"context"
	"errors"
	"log"
	"net"
	"sync"
	"time"
)

// ServerConfig configures the UDP server around the state machine.
type ServerConfig struct {
	Relay        Config        // limits of the state machine
	TickInterval time.Duration // how often timeouts are checked (default 1 s)
	LogInterval  time.Duration // summary line period (default 60 s; negative disables)
	Now          func() time.Time
	Logf         func(format string, args ...any) // default log.Printf

	// Accounts checks ranked tickets and takes the ranked pairings (ranked play is off
	// without it).
	Accounts Accounts
}

// Server reads datagrams from one UDP socket and feeds them to a Relay.
// One goroutine reads the socket, another runs the timers; a mutex
// serialises access to the state machine.
type Server struct {
	cfg   ServerConfig
	mu    sync.Mutex
	relay *Relay
}

// NewServer builds a server. Room events are logged through cfg.Logf unless
// cfg.Relay.Logf is set.
func NewServer(cfg ServerConfig) (*Server, error) {
	if cfg.TickInterval <= 0 {
		cfg.TickInterval = time.Second
	}
	if cfg.LogInterval == 0 {
		cfg.LogInterval = time.Minute
	}
	if cfg.Now == nil {
		cfg.Now = time.Now
	}
	if cfg.Logf == nil {
		cfg.Logf = log.Printf
	}
	if cfg.Relay.Logf == nil {
		cfg.Relay.Logf = cfg.Logf
	}
	cfg.Relay.Ranked = cfg.Accounts != nil
	r, err := NewRelay(cfg.Relay)
	if err != nil {
		return nil, err
	}
	return &Server{cfg: cfg, relay: r}, nil
}

// Serve runs until ctx is cancelled (then it closes conn and returns nil) or
// the socket fails. conn is owned by Serve from then on.
func (s *Server) Serve(ctx context.Context, conn *net.UDPConn) error {
	ctx, cancel := context.WithCancel(ctx)
	defer cancel()

	// Closing the socket is what unblocks the read loop on shutdown.
	stop := context.AfterFunc(ctx, func() { conn.Close() })
	defer stop()

	var wg sync.WaitGroup
	wg.Add(1)
	go func() {
		defer wg.Done()
		s.timerLoop(ctx, conn)
	}()
	defer wg.Wait()
	defer cancel() // runs before wg.Wait: stop the timer goroutine first

	// Larger than MaxDatagram so an oversized datagram is seen as such (and
	// dropped by the decoder) rather than silently truncated to a valid size.
	buf := make([]byte, 2048)
	for {
		n, from, err := conn.ReadFromUDPAddrPort(buf)
		if err != nil {
			if ctx.Err() != nil {
				return nil
			}
			if errors.Is(err, net.ErrClosed) {
				return err
			}
			// Transient errors (e.g. ICMP-induced resets on some platforms,
			// oversized datagrams on Windows): keep serving.
			time.Sleep(time.Millisecond)
			continue
		}
		s.mu.Lock()
		out := s.relay.Handle(s.cfg.Now(), from, buf[:n])
		lookups, pairings := s.relay.TakeRankedWork()
		s.mu.Unlock()
		send(conn, out)
		s.rankedWork(ctx, conn, lookups, pairings)
	}
}

// timerLoop runs Tick every TickInterval and logs a summary every LogInterval.
func (s *Server) timerLoop(ctx context.Context, conn *net.UDPConn) {
	tick := time.NewTicker(s.cfg.TickInterval)
	defer tick.Stop()

	var logC <-chan time.Time
	if s.cfg.LogInterval > 0 {
		lt := time.NewTicker(s.cfg.LogInterval)
		defer lt.Stop()
		logC = lt.C
	}
	lastLog := s.cfg.Now()

	for {
		select {
		case <-ctx.Done():
			return
		case <-tick.C:
			s.mu.Lock()
			out := s.relay.Tick(s.cfg.Now())
			lookups, pairings := s.relay.TakeRankedWork()
			s.mu.Unlock()
			send(conn, out)
			s.rankedWork(ctx, conn, lookups, pairings)
		case <-logC:
			now := s.cfg.Now()
			s.mu.Lock()
			st := s.relay.TakeStats()
			rooms, clients := s.relay.Rooms(), s.relay.Clients()
			s.mu.Unlock()
			secs := now.Sub(lastLog).Seconds()
			if secs <= 0 {
				secs = 1
			}
			lastLog = now
			s.cfg.Logf("summary rooms=%d clients=%d in=%.1fpkt/s %.0fB/s out=%.1fpkt/s %.0fB/s dropped=%d",
				rooms, clients,
				float64(st.PacketsIn)/secs, float64(st.BytesIn)/secs,
				float64(st.PacketsOut)/secs, float64(st.BytesOut)/secs,
				st.Dropped)
		}
	}
}

// rankedWork asks the account service about new ranked tickets and tells it of new ranked
// matches, each in its own goroutine, so the socket loop never waits for it.
func (s *Server) rankedWork(ctx context.Context, conn *net.UDPConn, lookups []RankedLookup, pairings []RankedPairing) {
	if s.cfg.Accounts == nil {
		return
	}
	for _, l := range lookups {
		go func() {
			c, cancel := context.WithTimeout(ctx, 5*time.Second)
			defer cancel()
			p, err := s.cfg.Accounts.Redeem(c, l.Ticket)
			if err != nil && !errors.Is(err, ErrTicketRefused) {
				s.cfg.Logf("ranked: account service: %v", err)
			}
			s.mu.Lock()
			out := s.relay.RankedChecked(s.cfg.Now(), l.Token, p, err == nil)
			s.mu.Unlock()
			send(conn, out)
		}()
	}
	for _, p := range pairings {
		go func() {
			c, cancel := context.WithTimeout(ctx, 10*time.Second)
			defer cancel()
			if err := s.cfg.Accounts.Paired(c, p); err != nil {
				s.cfg.Logf("ranked: the pairing of room %s did not reach the account service: %v", p.Room, err)
			}
		}()
	}
}

// Snapshot returns the current number of rooms and clients.
func (s *Server) Snapshot() (rooms, clients int) {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.relay.Rooms(), s.relay.Clients()
}

// send writes the datagrams. UDP send errors are not actionable here (the
// peer may be gone), so they are ignored.
func send(conn *net.UDPConn, out []Packet) {
	for _, p := range out {
		_, _ = conn.WriteToUDPAddrPort(p.Data, p.To)
	}
}

