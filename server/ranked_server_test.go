package relay

import (
	"context"
	"encoding/hex"
	"encoding/json"
	"net"
	"net/http"
	"net/http/httptest"
	"sync"
	"testing"
	"time"
)

// fakeAccounts is the account service's internal listener with two tickets.
type fakeAccounts struct {
	mu       sync.Mutex
	tickets  map[string]map[string]any
	pairings []map[string]any
}

func (f *fakeAccounts) handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("POST /internal/ranked/redeem", func(w http.ResponseWriter, r *http.Request) {
		var in struct{ Ticket string }
		json.NewDecoder(r.Body).Decode(&in)
		f.mu.Lock()
		p, ok := f.tickets[in.Ticket]
		delete(f.tickets, in.Ticket) // one use
		f.mu.Unlock()
		if !ok {
			w.WriteHeader(http.StatusNotFound)
			return
		}
		json.NewEncoder(w).Encode(p)
	})
	mux.HandleFunc("POST /internal/ranked/pairing", func(w http.ResponseWriter, r *http.Request) {
		var in map[string]any
		json.NewDecoder(r.Body).Decode(&in)
		f.mu.Lock()
		f.pairings = append(f.pairings, in)
		f.mu.Unlock()
		w.WriteHeader(http.StatusNoContent)
	})
	return mux
}

func TestServerRankedIntegration(t *testing.T) {
	ta, tb := [TicketLen]byte{1}, [TicketLen]byte{2}
	fake := &fakeAccounts{tickets: map[string]map[string]any{
		hex.EncodeToString(ta[:]): {"account": 11, "name": "Alice", "rating": 1500, "deviation": 350},
		hex.EncodeToString(tb[:]): {"account": 22, "name": "Bob", "rating": 1540, "deviation": 350},
	}}
	web := httptest.NewServer(fake.handler())
	defer web.Close()

	conn, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 0})
	if err != nil {
		t.Fatal(err)
	}
	logs := &syncLog{}
	srv, err := NewServer(ServerConfig{LogInterval: -1, Logf: logs.logf, TickInterval: 100 * time.Millisecond,
		Accounts: AccountsHTTP{Base: web.URL}})
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go srv.Serve(ctx, conn)
	addr := conn.LocalAddr().(*net.UDPAddr)

	a, b, c := dialRelay(t, addr), dialRelay(t, addr), dialRelay(t, addr)
	a.send(Hello{Mode: ModeLobby, Build: build, Name: "Alice"})
	wa := a.recv().(Welcome)
	b.send(Hello{Mode: ModeLobby, Build: build, Name: "Bob"})
	wb := b.recv().(Welcome)
	c.send(Hello{Mode: ModeLobby, Build: build, Name: "Mallory"})
	wc := c.recv().(Welcome)

	// A used ticket (Alice's, once she has redeemed it) is refused.
	a.send(Ranked{Token: wa.Token, Ticket: ta, Fighter: 58})
	if q := a.recvType(TypeQueue).(Queue); q.State != QueueChecking {
		t.Fatalf("A queue %#v", q)
	}
	for {
		if q := a.recvType(TypeQueue).(Queue); q.State == QueueSearching {
			break
		}
	}
	c.send(Ranked{Token: wc.Token, Ticket: ta, Fighter: 1})
	for {
		q := c.recvType(TypeQueue).(Queue)
		if q.State == QueueRefused {
			break
		}
		if q.State != QueueChecking {
			t.Fatalf("C queue %#v", q)
		}
	}

	b.send(Ranked{Token: wb.Token, Ticket: tb, Fighter: 40})
	for {
		if q := b.recvType(TypeQueue).(Queue); q.State == QueueFound {
			break
		}
	}
	rb := b.recvType(TypeRoom).(RoomState)
	if rb.Settings.Name != RankedRoomName || rb.Phase != PhaseVoting || len(rb.Players) != 2 {
		t.Fatalf("ranked room %#v", rb)
	}
	a.send(Vote{Token: wa.Token, Choice: 0})
	b.send(Vote{Token: wb.Token, Choice: 0})
	st := a.recvType(TypeStart).(Start)

	deadline := time.Now().Add(3 * time.Second)
	for {
		fake.mu.Lock()
		n := len(fake.pairings)
		var p map[string]any
		if n > 0 {
			p = fake.pairings[0]
		}
		fake.mu.Unlock()
		if n == 1 {
			if p["key"] != PairingKey(string(rb.Room[:]), st.Seed) {
				t.Fatalf("pairing %#v, START seed %08X", p, st.Seed)
			}
			seats, _ := p["seats"].([]any)
			if len(seats) != 2 {
				t.Fatalf("seats %#v", p)
			}
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("no pairing reached the account service")
		}
		time.Sleep(20 * time.Millisecond)
	}
}

