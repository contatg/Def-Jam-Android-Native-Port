package relay

import (
	"context"
	"fmt"
	"net"
	"strings"
	"sync"
	"testing"
	"time"
)

// udpClient is a minimal test client on a real socket.
type udpClient struct {
	t    *testing.T
	conn *net.UDPConn
}

func dialRelay(t *testing.T, addr *net.UDPAddr) *udpClient {
	t.Helper()
	conn, err := net.DialUDP("udp", nil, addr)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { conn.Close() })
	return &udpClient{t: t, conn: conn}
}

func (c *udpClient) send(m Message) {
	c.t.Helper()
	b, err := Encode(m)
	if err != nil {
		c.t.Fatal(err)
	}
	if _, err := c.conn.Write(b); err != nil {
		c.t.Fatal(err)
	}
}

// recv waits up to two seconds for one message.
func (c *udpClient) recv() Message {
	c.t.Helper()
	buf := make([]byte, 2048)
	_ = c.conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	n, err := c.conn.Read(buf)
	if err != nil {
		c.t.Fatalf("recv: %v", err)
	}
	m, err := DecodeServer(buf[:n])
	if err != nil {
		c.t.Fatalf("recv: undecodable % x", buf[:n])
	}
	return m
}

// expectSilence checks that nothing arrives for a short while.
func (c *udpClient) expectSilence(d time.Duration) {
	c.t.Helper()
	buf := make([]byte, 2048)
	_ = c.conn.SetReadDeadline(time.Now().Add(d))
	if n, err := c.conn.Read(buf); err == nil {
		c.t.Fatalf("unexpected datagram % x", buf[:n])
	}
}

type syncLog struct {
	mu    sync.Mutex
	lines []string
}

func (l *syncLog) logf(format string, args ...any) {
	l.mu.Lock()
	defer l.mu.Unlock()
	l.lines = append(l.lines, fmt.Sprintf(format, args...))
}

func (l *syncLog) has(s string) bool {
	l.mu.Lock()
	defer l.mu.Unlock()
	for _, line := range l.lines {
		if strings.Contains(line, s) {
			return true
		}
	}
	return false
}

func TestServerIntegration(t *testing.T) {
	conn, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 0})
	if err != nil {
		t.Fatal(err)
	}
	logs := &syncLog{}
	srv, err := NewServer(ServerConfig{LogInterval: 100 * time.Millisecond, Logf: logs.logf})
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- srv.Serve(ctx, conn) }()
	addr := conn.LocalAddr().(*net.UDPAddr)

	a := dialRelay(t, addr)
	b := dialRelay(t, addr)

	// Garbage is never answered.
	if _, err := a.conn.Write([]byte("DJNP\x01\x05garbage")); err != nil {
		t.Fatal(err)
	}
	a.expectSilence(200 * time.Millisecond)

	// create
	a.send(Hello{Mode: ModeCreate, Build: build, Name: "Alice"})
	wa, ok := a.recv().(Welcome)
	if !ok || wa.Slot != SlotHost || !wa.Room.Valid() {
		t.Fatalf("A welcome %#v", wa)
	}

	// join -> WELCOME + PEER to B, PEER to A
	b.send(Hello{Mode: ModeJoin, Room: wa.Room, Build: build, Name: "Bob"})
	wb, ok := b.recv().(Welcome)
	if !ok || wb.Slot != SlotGuest || wb.Room != wa.Room {
		t.Fatalf("B welcome %#v", wb)
	}
	if p := b.recv(); !msgEqual(p, Peer{Slot: SlotHost, Name: "Alice"}) {
		t.Fatalf("B peer %#v", p)
	}
	if p := a.recv(); !msgEqual(p, Peer{Slot: SlotGuest, Name: "Bob"}) {
		t.Fatalf("A peer %#v", p)
	}

	// DATA both ways
	a.send(ClientData{Token: wa.Token, Payload: []byte("from A")})
	if m := b.recv(); !msgEqual(m, ServerData{From: SlotHost, Payload: []byte("from A")}) {
		t.Fatalf("B got %#v", m)
	}
	b.send(ClientData{Token: wb.Token, Payload: []byte("from B")})
	if m := a.recv(); !msgEqual(m, ServerData{From: SlotGuest, Payload: []byte("from B")}) {
		t.Fatalf("A got %#v", m)
	}

	// PING/PONG
	a.send(Ping{Token: wa.Token, Nonce: 0xCAFEBABE})
	if m := a.recv(); !msgEqual(m, Pong{Nonce: 0xCAFEBABE}) {
		t.Fatalf("A got %#v", m)
	}
	b.send(Ping{Token: wb.Token, Nonce: 7})
	if m := b.recv(); !msgEqual(m, Pong{Nonce: 7}) {
		t.Fatalf("B got %#v", m)
	}

	// BYE -> peer-left
	b.send(Bye{Token: wb.Token})
	if m := a.recv(); !msgEqual(m, Peer{Slot: SlotGuest, Name: ""}) {
		t.Fatalf("A got %#v after BYE", m)
	}
	b.expectSilence(100 * time.Millisecond)

	// B's token is dead now.
	b.send(Ping{Token: wb.Token, Nonce: 8})
	if e, ok := b.recv().(Error); !ok || e.Code != ErrUnknownToken {
		t.Fatalf("B got %#v", e)
	}

	if rooms, clients := srv.Snapshot(); rooms != 1 || clients != 1 {
		t.Errorf("rooms=%d clients=%d", rooms, clients)
	}
	a.send(Bye{Token: wa.Token})

	// Wait for a summary line, then shut down.
	deadline := time.Now().Add(2 * time.Second)
	for !logs.has("summary rooms=") && time.Now().Before(deadline) {
		time.Sleep(20 * time.Millisecond)
	}
	cancel()
	select {
	case err := <-done:
		if err != nil {
			t.Fatalf("Serve: %v", err)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("Serve did not return after cancel")
	}

	for _, want := range []string{"created", "joined", "peer left", "closed (empty)", "summary rooms="} {
		if !logs.has(want) {
			t.Errorf("no %q log line in %q", want, logs.lines)
		}
	}
	if logs.has("Alice") || logs.has("127.0.0.1") {
		t.Errorf("log leaks names or addresses: %q", logs.lines)
	}
}

// recvType waits for the next message of type ty, skipping others (ROOM
// broadcasts interleave with everything in the lobby).
func (c *udpClient) recvType(ty MsgType) Message {
	c.t.Helper()
	deadline := time.Now().Add(3 * time.Second)
	for time.Now().Before(deadline) {
		if m := c.recv(); m.Type() == ty {
			return m
		}
	}
	c.t.Fatalf("no message of type %#x", ty)
	return nil
}

func TestServerLobbyIntegration(t *testing.T) {
	conn, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 0})
	if err != nil {
		t.Fatal(err)
	}
	logs := &syncLog{}
	srv, err := NewServer(ServerConfig{LogInterval: -1, Logf: logs.logf})
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	done := make(chan error, 1)
	go func() { done <- srv.Serve(ctx, conn) }()
	addr := conn.LocalAddr().(*net.UDPAddr)

	a := dialRelay(t, addr)
	b := dialRelay(t, addr)

	// Both enter the lobby.
	a.send(Hello{Mode: ModeLobby, Build: build, Name: "Alice"})
	wa, ok := a.recv().(Welcome)
	if !ok || wa.Slot != SlotNone || wa.Room != (RoomCode{}) {
		t.Fatalf("A welcome %#v", wa)
	}
	b.send(Hello{Mode: ModeLobby, Build: build, Name: "Bob"})
	wb := b.recv().(Welcome)

	// A creates a public room; B lists and finds it, then joins.
	a.send(Create{Token: wa.Token, Settings: RoomSettings{Name: "Alice's", Public: 1, Mode: 0, Max: 4, Pool: []uint8{1, 6, 11}}})
	ra := a.recvType(TypeRoom).(RoomState)
	if ra.You != 0 || ra.Host != 0 || len(ra.Players) != 1 {
		t.Fatalf("A ROOM %#v", ra)
	}
	b.send(List{Token: wb.Token, Mode: ModeAny})
	rs := b.recvType(TypeRooms).(Rooms)
	if len(rs.Rooms) != 1 || rs.Rooms[0].Room != ra.Room || rs.Rooms[0].Name != "Alice's" || rs.Rooms[0].Players != 1 {
		t.Fatalf("ROOMS %#v", rs)
	}
	b.send(Join{Token: wb.Token, Room: ra.Room})
	if rb := b.recvType(TypeRoom).(RoomState); rb.You != 1 || len(rb.Players) != 2 {
		t.Fatalf("B ROOM %#v", rb)
	}
	if ra = a.recvType(TypeRoom).(RoomState); len(ra.Players) != 2 {
		t.Fatalf("A ROOM after join %#v", ra)
	}

	// Ready, vote, START.
	a.send(Ready{Token: wa.Token, Ready: 1, Fighter: 4})
	b.send(Ready{Token: wb.Token, Ready: 1, Fighter: 5})
	for {
		if st := a.recvType(TypeRoom).(RoomState); st.Phase == PhaseVoting {
			if len(st.Candidates) != 3 || st.MsLeft == 0 {
				t.Fatalf("voting ROOM %#v", st)
			}
			break
		}
	}
	a.send(Vote{Token: wa.Token, Choice: 2})
	b.send(Vote{Token: wb.Token, Choice: 2})
	sa := a.recvType(TypeStart).(Start)
	sb := b.recvType(TypeStart).(Start)
	if !msgEqual(sa, sb) || len(sa.Players) != 2 || sa.Players[0] != (StartPlayer{0, 4}) || sa.Players[1] != (StartPlayer{1, 5}) {
		t.Fatalf("START %#v / %#v", sa, sb)
	}

	// DATA, then OVER from both returns the room to the lobby.
	a.send(ClientData{Token: wa.Token, Payload: []byte("frame")})
	if m := b.recvType(TypeData); !msgEqual(m, ServerData{From: 0, Payload: []byte("frame")}) {
		t.Fatalf("B got %#v", m)
	}
	a.send(Over{Token: wa.Token})
	b.send(Over{Token: wb.Token})
	for {
		if st := b.recvType(TypeRoom).(RoomState); st.Phase == PhaseLobby {
			break
		}
	}

	// B leaves (back to the lobby), then both say BYE.
	b.send(Leave{Token: wb.Token})
	for {
		if st := a.recvType(TypeRoom).(RoomState); len(st.Players) == 1 {
			break
		}
	}
	b.send(Bye{Token: wb.Token})
	a.send(Bye{Token: wa.Token})
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		if rooms, clients := srv.Snapshot(); rooms == 0 && clients == 0 {
			break
		}
		time.Sleep(10 * time.Millisecond)
	}
	if rooms, clients := srv.Snapshot(); rooms != 0 || clients != 0 {
		t.Errorf("rooms=%d clients=%d at the end", rooms, clients)
	}
	cancel()
	if err := <-done; err != nil {
		t.Fatalf("Serve: %v", err)
	}
	for _, want := range []string{"lobby entered", "created", "joined", "voting", "started", "back to lobby", "closed (empty)"} {
		if !logs.has(want) {
			t.Errorf("no %q log line in %q", want, logs.lines)
		}
	}
	if logs.has("Alice") || logs.has("127.0.0.1") {
		t.Errorf("log leaks names or addresses: %q", logs.lines)
	}
}

