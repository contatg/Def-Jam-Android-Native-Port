package relay

import (
	"bytes"
	"crypto/rand"
	"encoding/hex"
	"fmt"
	"io"
	"net/netip"
	"strings"
	"testing"
	"time"
)

// harness drives a Relay with a fake clock and checks the anti-amplification
// rule on every reply.
type harness struct {
	t    *testing.T
	r    *Relay
	now  time.Time
	logs []string
}

func newHarness(t *testing.T, cfg Config) *harness {
	t.Helper()
	h := &harness{t: t, now: time.Date(2026, 1, 1, 12, 0, 0, 0, time.UTC)}
	cfg.Logf = func(format string, args ...any) { h.logs = append(h.logs, fmt.Sprintf(format, args...)) }
	r, err := NewRelay(cfg)
	if err != nil {
		t.Fatal(err)
	}
	h.r = r
	return h
}

func ap(s string) netip.AddrPort { return netip.MustParseAddrPort(s) }

type got struct {
	to  netip.AddrPort
	msg Message
}

// sendRaw feeds one datagram and decodes everything the relay answers.
func (h *harness) sendRaw(from netip.AddrPort, b []byte) []got {
	h.t.Helper()
	out := h.r.Handle(h.now, from, b)
	var res []got
	for _, p := range out {
		if len(p.Data) > MaxDatagram {
			h.t.Fatalf("relay produced a %d-byte datagram", len(p.Data))
		}
		m, err := DecodeServer(p.Data)
		if err != nil {
			h.t.Fatalf("relay produced an undecodable datagram % x: %v", p.Data, err)
		}
		// Replies to the sender may never be larger than the request, except
		// PEER and DATA, which only go to registered clients, and the lobby
		// replies ROOMS, ROOM, START and PLAYERS, which must go to a registered client.
		lobbyReply := m.Type() == TypeRooms || m.Type() == TypeRoom || m.Type() == TypeStart || m.Type() == TypePlayers
		if lobbyReply && h.r.clients[p.To] == nil {
			h.t.Fatalf("%T sent to unregistered %v", m, p.To)
		}
		if p.To == from && m.Type() != TypePeer && m.Type() != TypeData && !lobbyReply && len(p.Data) > len(b) {
			h.t.Fatalf("amplification: %d-byte %T reply to a %d-byte request", len(p.Data), m, len(b))
		}
		res = append(res, got{p.To, m})
	}
	return res
}

func (h *harness) send(from netip.AddrPort, m Message) []got {
	h.t.Helper()
	b, err := Encode(m)
	if err != nil {
		h.t.Fatal(err)
	}
	return h.sendRaw(from, b)
}

// advance moves the clock and runs the timers.
func (h *harness) advance(d time.Duration) []got {
	h.t.Helper()
	h.now = h.now.Add(d)
	var res []got
	for _, p := range h.r.Tick(h.now) {
		m, err := DecodeServer(p.Data)
		if err != nil {
			h.t.Fatal(err)
		}
		res = append(res, got{p.To, m})
	}
	return res
}

func (h *harness) create(from netip.AddrPort, name string) Welcome {
	h.t.Helper()
	res := h.send(from, Hello{Mode: ModeCreate, Build: build, Name: name})
	if len(res) != 1 {
		h.t.Fatalf("create: %d replies %#v", len(res), res)
	}
	w, ok := res[0].msg.(Welcome)
	if !ok || res[0].to != from {
		h.t.Fatalf("create: got %#v", res[0])
	}
	return w
}

func (h *harness) join(from netip.AddrPort, room RoomCode, name string) []got {
	h.t.Helper()
	return h.send(from, Hello{Mode: ModeJoin, Room: room, Build: build, Name: name})
}

// pair creates a full room and returns both WELCOMEs.
func (h *harness) pair(host, guest netip.AddrPort) (Welcome, Welcome) {
	h.t.Helper()
	wh := h.create(host, "host")
	res := h.join(guest, wh.Room, "guest")
	if len(res) != 3 {
		h.t.Fatalf("join: want 3 packets, got %#v", res)
	}
	return wh, res[0].msg.(Welcome)
}

func expectOne(t *testing.T, res []got, to netip.AddrPort, want Message) {
	t.Helper()
	if len(res) != 1 {
		t.Fatalf("want exactly one packet %#v to %v, got %#v", want, to, res)
	}
	if res[0].to != to || !msgEqual(res[0].msg, want) {
		t.Fatalf("got %#v to %v, want %#v to %v", res[0].msg, res[0].to, want, to)
	}
}

func expectError(t *testing.T, res []got, to netip.AddrPort, code uint8) {
	t.Helper()
	if len(res) != 1 {
		t.Fatalf("want ERROR %d, got %#v", code, res)
	}
	e, ok := res[0].msg.(Error)
	if !ok || e.Code != code || res[0].to != to {
		t.Fatalf("want ERROR %d to %v, got %#v to %v", code, to, res[0].msg, res[0].to)
	}
}

func expectNone(t *testing.T, res []got) {
	t.Helper()
	if len(res) != 0 {
		t.Fatalf("want no packets, got %#v", res)
	}
}

func msgEqual(a, b Message) bool {
	ea, _ := Encode(a)
	eb, _ := Encode(b)
	return bytes.Equal(ea, eb)
}

var (
	hostAddr  = ap("198.51.100.1:40000")
	guestAddr = ap("203.0.113.7:50000")
	thirdAddr = ap("192.0.2.33:60000")
)

func TestCreate(t *testing.T) {
	h := newHarness(t, Config{})
	w := h.create(hostAddr, "host")
	if w.Slot != SlotHost {
		t.Errorf("slot %d", w.Slot)
	}
	if !w.Room.Valid() {
		t.Errorf("room code %q not from alphabet", w.Room)
	}
	if w.Token == (Token{}) {
		t.Error("zero token")
	}
	if h.r.Rooms() != 1 || h.r.Clients() != 1 {
		t.Errorf("rooms=%d clients=%d", h.r.Rooms(), h.r.Clients())
	}
	// Room bytes are ignored when creating.
	w2 := h.send(thirdAddr, Hello{Mode: ModeCreate, Room: code, Build: build, Name: "x"})[0].msg.(Welcome)
	if w2.Room == w.Room || w2.Token == w.Token {
		t.Error("second room reused code or token")
	}
	if len(h.logs) != 2 || !strings.Contains(h.logs[0], "room "+w.Room.String()+" created") {
		t.Errorf("logs %q", h.logs)
	}
}

func TestRoomCodesAreRandomAndValid(t *testing.T) {
	h := newHarness(t, Config{MaxRooms: 400, MaxClients: 400, HellosPerMinute: 1000})
	seen := map[RoomCode]bool{}
	for i := 0; i < 300; i++ {
		w := h.create(netip.AddrPortFrom(netip.AddrFrom4([4]byte{10, 0, byte(i >> 8), byte(i)}), 1000), "p")
		if !w.Room.Valid() || seen[w.Room] {
			t.Fatalf("bad or repeated code %q", w.Room)
		}
		seen[w.Room] = true
	}
}

// constReader returns the same byte forever, forcing room code collisions.
type constReader byte

func (c constReader) Read(p []byte) (int, error) {
	for i := range p {
		p[i] = byte(c)
	}
	return len(p), nil
}

func TestRoomCodeCollisionGivesUpSilently(t *testing.T) {
	h := newHarness(t, Config{Rand: constReader(0)})
	w := h.create(hostAddr, "a")
	if w.Room.String() != "AAAAAA" {
		t.Fatalf("room %q", w.Room)
	}
	// Every further draw collides; the HELLO is dropped rather than crashing.
	expectNone(t, h.send(guestAddr, Hello{Mode: ModeCreate, Build: build, Name: "b"}))
	if h.r.Rooms() != 1 || h.r.Clients() != 1 {
		t.Errorf("rooms=%d clients=%d", h.r.Rooms(), h.r.Clients())
	}
}

func TestJoinAndPeer(t *testing.T) {
	h := newHarness(t, Config{})
	wh := h.create(hostAddr, "host")

	// DATA while alone goes nowhere.
	expectNone(t, h.send(hostAddr, ClientData{Token: wh.Token, Payload: []byte("x")}))

	res := h.join(guestAddr, wh.Room, "guest")
	if len(res) != 3 {
		t.Fatalf("join: %#v", res)
	}
	wg, ok := res[0].msg.(Welcome)
	if !ok || res[0].to != guestAddr || wg.Slot != SlotGuest || wg.Room != wh.Room || wg.Token == wh.Token {
		t.Fatalf("guest welcome %#v", res[0])
	}
	if res[1].to != guestAddr || !msgEqual(res[1].msg, Peer{Slot: SlotHost, Name: "host"}) {
		t.Errorf("guest PEER %#v", res[1])
	}
	if res[2].to != hostAddr || !msgEqual(res[2].msg, Peer{Slot: SlotGuest, Name: "guest"}) {
		t.Errorf("host PEER %#v", res[2])
	}

	// DATA both ways, with the sender's slot.
	payload := bytes.Repeat([]byte{0xC3}, MaxPayload)
	expectOne(t, h.send(hostAddr, ClientData{Token: wh.Token, Payload: payload}), guestAddr, ServerData{From: 0, Payload: payload})
	expectOne(t, h.send(guestAddr, ClientData{Token: wg.Token, Payload: []byte{1, 2}}), hostAddr, ServerData{From: 1, Payload: []byte{1, 2}})

	// PING/PONG for both.
	expectOne(t, h.send(hostAddr, Ping{Token: wh.Token, Nonce: 77}), hostAddr, Pong{Nonce: 77})
	expectOne(t, h.send(guestAddr, Ping{Token: wg.Token, Nonce: 78}), guestAddr, Pong{Nonce: 78})
}

func TestJoinErrors(t *testing.T) {
	h := newHarness(t, Config{HellosPerMinute: 100})
	wh, _ := h.pair(hostAddr, guestAddr)

	expectError(t, h.join(thirdAddr, RoomCode{'Z', 'Z', 'Z', 'Z', 'Z', 'Z'}, "x"), thirdAddr, ErrRoomNotFound)
	expectError(t, h.join(thirdAddr, RoomCode{}, "x"), thirdAddr, ErrRoomNotFound)
	expectError(t, h.join(thirdAddr, RoomCode{'a', 'b', 'c', 'd', 'e', 'f'}, "x"), thirdAddr, ErrRoomNotFound)
	expectError(t, h.join(thirdAddr, wh.Room, "x"), thirdAddr, ErrRoomFull)

	// Build mismatch on a fresh room.
	w := h.create(ap("192.0.2.50:1"), "host2")
	other := build
	other[0] ^= 1
	expectError(t, h.send(thirdAddr, Hello{Mode: ModeJoin, Room: w.Room, Build: other, Name: "x"}), thirdAddr, ErrBuildMismatch)

	// Bad names, for create and join.
	for _, name := range []string{"", strings.Repeat("n", MaxNameLen+1), "\xff\xfe", "ok\xc3"} {
		expectError(t, h.send(thirdAddr, Hello{Mode: ModeCreate, Build: build, Name: name}), thirdAddr, ErrBadName)
		expectError(t, h.send(thirdAddr, Hello{Mode: ModeJoin, Room: w.Room, Build: build, Name: name}), thirdAddr, ErrBadName)
	}
	// A 24-byte multi-byte UTF-8 name is fine.
	res := h.send(thirdAddr, Hello{Mode: ModeJoin, Room: w.Room, Build: build, Name: strings.Repeat("ü", 12)})
	if _, ok := res[0].msg.(Welcome); !ok {
		t.Fatalf("24-byte UTF-8 name rejected: %#v", res)
	}
	if h.r.Clients() != 4 || h.r.Rooms() != 2 {
		t.Errorf("errors registered clients: rooms=%d clients=%d", h.r.Rooms(), h.r.Clients())
	}
}

func TestErrorTextsFitSmallestHello(t *testing.T) {
	minHello := HeaderLen + 1 + RoomCodeLen + BuildLen + 1
	for code, text := range ErrorText {
		if HeaderLen+2+len(text) > minHello {
			t.Errorf("ERROR %d text %q makes the reply larger than the smallest HELLO", code, text)
		}
	}
}

func TestServerFull(t *testing.T) {
	h := newHarness(t, Config{MaxRooms: 2, MaxClients: 3})
	w1 := h.create(ap("192.0.2.1:1"), "a")
	h.create(ap("192.0.2.2:1"), "b")
	expectError(t, h.send(ap("192.0.2.3:1"), Hello{Mode: ModeCreate, Build: build, Name: "c"}), ap("192.0.2.3:1"), ErrServerFull)
	if res := h.join(ap("192.0.2.4:1"), w1.Room, "d"); len(res) != 3 {
		t.Fatalf("join: %#v", res)
	}
	// Three clients now: both create and join are refused.
	expectError(t, h.send(ap("192.0.2.5:1"), Hello{Mode: ModeCreate, Build: build, Name: "e"}), ap("192.0.2.5:1"), ErrServerFull)
	expectError(t, h.join(ap("192.0.2.5:1"), w1.Room, "e"), ap("192.0.2.5:1"), ErrServerFull)
}

func TestRepeatedHelloIsIdempotent(t *testing.T) {
	h := newHarness(t, Config{})
	hello := Hello{Mode: ModeCreate, Build: build, Name: "host"}
	first := h.send(hostAddr, hello)
	// Far more repeats than the HELLO rate limit: they are not counted.
	for i := 0; i < 50; i++ {
		h.now = h.now.Add(500 * time.Millisecond)
		res := h.send(hostAddr, hello)
		if len(res) != 1 || !msgEqual(res[0].msg, first[0].msg) {
			t.Fatalf("repeat %d: %#v, want %#v", i, res, first)
		}
	}
	// Even a HELLO with different contents gets the existing WELCOME.
	res := h.send(hostAddr, Hello{Mode: ModeJoin, Room: code, Build: build, Name: "zzz"})
	if len(res) != 1 || !msgEqual(res[0].msg, first[0].msg) {
		t.Fatalf("different HELLO: %#v", res)
	}
	if h.r.Rooms() != 1 || h.r.Clients() != 1 {
		t.Errorf("rooms=%d clients=%d", h.r.Rooms(), h.r.Clients())
	}

	// After the room fills, a repeated join HELLO re-sends WELCOME and the PEER.
	wh := first[0].msg.(Welcome)
	join := h.join(guestAddr, wh.Room, "guest")
	again := h.join(guestAddr, wh.Room, "guest")
	if len(again) != 2 || !msgEqual(again[0].msg, join[0].msg) || !msgEqual(again[1].msg, Peer{Slot: 0, Name: "host"}) {
		t.Fatalf("repeated join: %#v", again)
	}
	// The repeat also counts as activity for the idle timeout.
	h.advance(10 * time.Second)
	h.send(hostAddr, hello)
	h.send(guestAddr, Hello{Mode: ModeJoin, Room: wh.Room, Build: build, Name: "guest"})
	h.advance(10 * time.Second)
	if h.r.Clients() != 2 {
		t.Errorf("repeated HELLO did not keep clients alive: %d", h.r.Clients())
	}
}

func TestHelloRateLimit(t *testing.T) {
	h := newHarness(t, Config{})
	ipPort := func(port uint16) netip.AddrPort { return netip.AddrPortFrom(netip.MustParseAddr("198.51.100.9"), port) }
	for i := 0; i < 10; i++ {
		h.create(ipPort(uint16(1000+i)), "p")
		h.now = h.now.Add(time.Second)
	}
	// 11th within the minute: ERROR 6, then silence for a second.
	expectError(t, h.send(ipPort(2000), Hello{Mode: ModeCreate, Build: build, Name: "p"}), ipPort(2000), ErrRateLimited)
	expectNone(t, h.send(ipPort(2001), Hello{Mode: ModeCreate, Build: build, Name: "p"}))
	h.now = h.now.Add(time.Second)
	expectError(t, h.send(ipPort(2002), Hello{Mode: ModeCreate, Build: build, Name: "p"}), ipPort(2002), ErrRateLimited)

	// Other IPs are unaffected.
	h.create(thirdAddr, "x")

	// One minute after the first HELLO, one more is admitted.
	h.now = h.now.Add(49 * time.Second) // first HELLO was at t=0; now t=60s
	h.create(ipPort(3000), "p")
	expectError(t, h.send(ipPort(3001), Hello{Mode: ModeCreate, Build: build, Name: "p"}), ipPort(3001), ErrRateLimited)

	// Failed HELLOs (here: room not found) count too.
	h2 := newHarness(t, Config{})
	for i := 0; i < 10; i++ {
		expectError(t, h2.join(ipPort(uint16(i+1)), code, "p"), ipPort(uint16(i+1)), ErrRoomNotFound)
	}
	expectError(t, h2.join(ipPort(99), code, "p"), ipPort(99), ErrRateLimited)
}

func TestHelloRateLimitIPv6Prefix(t *testing.T) {
	h := newHarness(t, Config{HellosPerMinute: 2})
	h.create(ap("[2001:db8:1:2::1]:1000"), "a")
	h.create(ap("[2001:db8:1:2:ffff::9]:1000"), "b")
	// Same /64: limited.
	expectError(t, h.send(ap("[2001:db8:1:2::77]:5"), Hello{Mode: ModeCreate, Build: build, Name: "c"}), ap("[2001:db8:1:2::77]:5"), ErrRateLimited)
	// Different /64: fine.
	h.create(ap("[2001:db8:1:3::1]:1000"), "d")
	// IPv4-mapped IPv6 shares the IPv4 key.
	h.create(ap("192.0.2.200:1"), "e")
	h.create(ap("[::ffff:192.0.2.200]:2"), "f")
	expectError(t, h.send(ap("192.0.2.200:3"), Hello{Mode: ModeCreate, Build: build, Name: "g"}), ap("192.0.2.200:3"), ErrRateLimited)
}

func TestMessageRateLimit(t *testing.T) {
	h := newHarness(t, Config{})
	w := h.create(hostAddr, "host")
	h.now = h.now.Add(time.Second) // fresh one-second window
	ping := Ping{Token: w.Token, Nonce: 1}
	for i := 0; i < 400; i++ {
		if res := h.send(hostAddr, ping); len(res) != 1 {
			t.Fatalf("ping %d dropped", i)
		}
	}
	expectNone(t, h.send(hostAddr, ping))
	// Garbage counts towards the limit as well, and is dropped either way.
	expectNone(t, h.sendRaw(hostAddr, []byte("junk")))
	h.now = h.now.Add(time.Second)
	expectOne(t, h.send(hostAddr, ping), hostAddr, Pong{Nonce: 1})
	// The limit is per IP: another port on the same IP shares it.
	for i := 0; i < 399; i++ {
		h.sendRaw(ap("198.51.100.1:40001"), []byte("x"))
	}
	expectNone(t, h.send(hostAddr, ping))
}

func TestTokenCheck(t *testing.T) {
	h := newHarness(t, Config{})
	wh, wg := h.pair(hostAddr, guestAddr)
	bad := wh.Token
	bad[7] ^= 0xFF

	// Wrong token from a registered address: ERROR 7, not forwarded.
	expectError(t, h.send(hostAddr, ClientData{Token: bad, Payload: []byte("hi")}), hostAddr, ErrUnknownToken)
	// Within a second: ignored.
	expectNone(t, h.send(hostAddr, Ping{Token: bad, Nonce: 1}))
	h.now = h.now.Add(time.Second)
	expectError(t, h.send(hostAddr, Bye{Token: bad}), hostAddr, ErrUnknownToken)
	if h.r.Clients() != 2 {
		t.Fatal("bad BYE removed a client")
	}

	// Another client's token from my address is unknown.
	h.now = h.now.Add(time.Second)
	expectError(t, h.send(hostAddr, Ping{Token: wg.Token, Nonce: 2}), hostAddr, ErrUnknownToken)

	// The ERROR text is dropped when it would make the reply larger than the request.
	h.now = h.now.Add(time.Second)
	res := h.send(thirdAddr, Bye{Token: bad}) // 14-byte request
	expectError(t, res, thirdAddr, ErrUnknownToken)
	if e := res[0].msg.(Error); e.Text != "" {
		t.Errorf("14-byte BYE got text %q", e.Text)
	}
	h.now = h.now.Add(time.Second)
	res = h.send(thirdAddr, ClientData{Token: bad, Payload: make([]byte, 16)})
	if e := res[0].msg.(Error); e.Text != ErrorText[ErrUnknownToken] {
		t.Errorf("large DATA got text %q", e.Text)
	}

	// The real token still works.
	expectOne(t, h.send(hostAddr, Ping{Token: wh.Token, Nonce: 3}), hostAddr, Pong{Nonce: 3})
}

// A client whose address changes (a router's new port, another network) keeps its
// session: its token moves it, its room and the forwarding follow, the old address
// is forgotten, and a token can never take over an address another client holds.
func TestClientMovesAddress(t *testing.T) {
	h := newHarness(t, Config{})
	wh, wg := h.pair(hostAddr, guestAddr)
	moved := ap("198.51.100.1:40002")
	expectOne(t, h.send(moved, ClientData{Token: wh.Token, Payload: []byte("hi")}), guestAddr,
		ServerData{From: 0, Payload: []byte("hi")})
	expectOne(t, h.send(guestAddr, ClientData{Token: wg.Token, Payload: []byte("yo")}), moved,
		ServerData{From: 1, Payload: []byte("yo")})
	expectOne(t, h.send(moved, Ping{Token: wh.Token, Nonce: 9}), moved, Pong{Nonce: 9})
	if h.r.Clients() != 2 || h.r.stats.Moved != 1 {
		t.Fatalf("clients %d, moved %d", h.r.Clients(), h.r.stats.Moved)
	}
	// The guest's token from the host's (now registered) new address is refused.
	h.now = h.now.Add(time.Second)
	expectError(t, h.send(moved, Ping{Token: wg.Token, Nonce: 1}), moved, ErrUnknownToken)
	// BYE from the new address removes the client.
	h.send(moved, Bye{Token: wh.Token})
	if h.r.Clients() != 1 {
		t.Fatalf("clients after BYE %d", h.r.Clients())
	}
	// A removed client's token moves nothing.
	h.now = h.now.Add(time.Second)
	expectError(t, h.send(thirdAddr, Ping{Token: wh.Token, Nonce: 2}), thirdAddr, ErrUnknownToken)
}

func TestIdleTimeout(t *testing.T) {
	h := newHarness(t, Config{})
	wh, wg := h.pair(hostAddr, guestAddr)
	// The host pings every second, the guest goes silent.
	for i := 0; i < 14; i++ {
		expectNone(t, h.advance(time.Second))
		h.send(hostAddr, Ping{Token: wh.Token})
	}
	res := h.advance(time.Second) // 15 s since the guest's last message
	expectOne(t, res, hostAddr, Peer{Slot: SlotGuest, Name: ""})
	if h.r.Clients() != 1 || h.r.Rooms() != 1 {
		t.Fatalf("rooms=%d clients=%d", h.r.Rooms(), h.r.Clients())
	}
	expectError(t, h.send(guestAddr, Ping{Token: wg.Token}), guestAddr, ErrUnknownToken)

	// Now the host goes silent too: removed, and the empty room is deleted.
	h.advance(13 * time.Second) // host's last message was 14 s ago
	if h.r.Clients() != 1 {
		t.Fatal("host removed early")
	}
	expectNone(t, h.advance(time.Second))
	if h.r.Clients() != 0 || h.r.Rooms() != 0 {
		t.Fatalf("rooms=%d clients=%d", h.r.Rooms(), h.r.Clients())
	}
	if !containsLog(h.logs, "reason=timeout") || !containsLog(h.logs, "closed (empty)") {
		t.Errorf("logs %q", h.logs)
	}
}

func TestRoomExpiry(t *testing.T) {
	h := newHarness(t, Config{})
	w := h.create(hostAddr, "lonely")
	for i := 0; i < 599; i++ {
		h.advance(time.Second)
		h.send(hostAddr, Ping{Token: w.Token})
	}
	if h.r.Rooms() != 1 {
		t.Fatal("room expired early")
	}
	h.advance(time.Second) // 10 minutes
	if h.r.Rooms() != 0 || h.r.Clients() != 0 {
		t.Fatalf("rooms=%d clients=%d after 10 min", h.r.Rooms(), h.r.Clients())
	}
	if !containsLog(h.logs, "expired") {
		t.Errorf("logs %q", h.logs)
	}
	h.now = h.now.Add(2 * time.Second)
	expectError(t, h.send(hostAddr, Ping{Token: w.Token}), hostAddr, ErrUnknownToken)

	// A room that did get a second player does not expire.
	h2 := newHarness(t, Config{})
	wh, wg := h2.pair(hostAddr, guestAddr)
	h2.send(guestAddr, Bye{Token: wg.Token})
	for i := 0; i < 700; i++ {
		h2.advance(time.Second)
		h2.send(hostAddr, Ping{Token: wh.Token})
	}
	if h2.r.Rooms() != 1 {
		t.Fatal("room that had a guest expired")
	}
}

func TestByeAndRejoin(t *testing.T) {
	h := newHarness(t, Config{})
	wh, wg := h.pair(hostAddr, guestAddr)

	// Guest leaves: host is told, room stays, BYE itself gets no reply.
	expectOne(t, h.send(guestAddr, Bye{Token: wg.Token}), hostAddr, Peer{Slot: SlotGuest, Name: ""})
	if h.r.Rooms() != 1 || h.r.Clients() != 1 {
		t.Fatalf("rooms=%d clients=%d", h.r.Rooms(), h.r.Clients())
	}
	// A new guest can take slot 1.
	res := h.join(thirdAddr, wh.Room, "third")
	if len(res) != 3 || res[0].msg.(Welcome).Slot != SlotGuest {
		t.Fatalf("rejoin %#v", res)
	}
	if res[2].to != hostAddr || !msgEqual(res[2].msg, Peer{Slot: SlotGuest, Name: "third"}) {
		t.Fatalf("host not told about new guest: %#v", res[2])
	}
	w3 := res[0].msg.(Welcome)

	// Host leaves: the guest is told; the room cannot be joined (slot 1 taken).
	expectOne(t, h.send(hostAddr, Bye{Token: wh.Token}), thirdAddr, Peer{Slot: SlotHost, Name: ""})
	expectError(t, h.join(ap("192.0.2.99:9"), wh.Room, "late"), ap("192.0.2.99:9"), ErrRoomFull)
	// DATA from the lone guest goes nowhere.
	expectNone(t, h.send(thirdAddr, ClientData{Token: w3.Token, Payload: []byte{1}}))

	// Last one out: room deleted at once.
	expectNone(t, h.send(thirdAddr, Bye{Token: w3.Token}))
	if h.r.Rooms() != 0 || h.r.Clients() != 0 {
		t.Fatalf("rooms=%d clients=%d", h.r.Rooms(), h.r.Clients())
	}
	// The address can start over.
	h.create(hostAddr, "again")
}

func TestDataStaysInRoom(t *testing.T) {
	h := newHarness(t, Config{})
	a0, a1 := ap("10.1.0.1:1"), ap("10.1.0.2:1")
	b0, b1 := ap("10.2.0.1:1"), ap("10.2.0.2:1")
	wa0, wa1 := h.pair(a0, a1)
	wb0, _ := h.pair(b0, b1)
	if wa0.Room == wb0.Room {
		t.Fatal("same room")
	}
	expectOne(t, h.send(a0, ClientData{Token: wa0.Token, Payload: []byte("A")}), a1, ServerData{From: 0, Payload: []byte("A")})
	expectOne(t, h.send(a1, ClientData{Token: wa1.Token, Payload: []byte("B")}), a0, ServerData{From: 1, Payload: []byte("B")})
	expectOne(t, h.send(b0, ClientData{Token: wb0.Token, Payload: []byte("C")}), b1, ServerData{From: 0, Payload: []byte("C")})
	// A token from room A used from room B's address is rejected, not forwarded.
	expectError(t, h.send(b1, ClientData{Token: wa0.Token, Payload: []byte("X")}), b1, ErrUnknownToken)
}

func TestGarbageGetsNoReply(t *testing.T) {
	h := newHarness(t, Config{MsgsPerSecond: 1 << 30})
	h.pair(hostAddr, guestAddr)
	inputs := [][]byte{
		nil, {0}, []byte("DJNP"), []byte("DJNP\x01"), []byte("DJNP\x01\x01"),
		[]byte("GET / HTTP/1.1\r\n\r\n"), make([]byte, 1201), make([]byte, 4000),
		cat([]byte("DJNP\x02\x05"), tok[:], []byte{1, 2, 3, 4}),
		cat(hdr(TypePong), []byte{1, 2, 3, 4}),
	}
	buf := make([]byte, 64)
	for i := 0; i < 5000; i++ {
		n := int(buf[0])%60 + 1
		_, _ = io.ReadFull(rand.Reader, buf[:n])
		b := append([]byte(nil), buf[:n]...)
		if i%2 == 0 && n >= 6 {
			copy(b, "DJNP\x01") // valid header prefix, random rest
		}
		inputs = append(inputs, b)
	}
	before := h.r.Clients()
	_ = h.r.TakeStats()
	replies := 0
	for _, b := range inputs {
		for _, g := range h.sendRaw(thirdAddr, b) {
			// Random bytes can form a valid PING/DATA/BYE with an unknown token
			// (ERROR 7, rate-limited) or a valid HELLO; nothing else may answer.
			switch g.msg.(type) {
			case Error, Welcome:
				replies++
			default:
				t.Fatalf("garbage % x answered with %#v", b, g.msg)
			}
		}
	}
	if replies > 50 {
		t.Errorf("%d replies to random input", replies)
	}
	if h.r.Clients() < before {
		t.Error("garbage removed a client")
	}
	if st := h.r.TakeStats(); st.Dropped < uint64(len(inputs)-replies) || st.PacketsIn != uint64(len(inputs)) {
		t.Errorf("stats %+v for %d inputs", st, len(inputs))
	}
}

func TestLogsHaveNoSecrets(t *testing.T) {
	h := newHarness(t, Config{})
	wh, wg := h.pair(hostAddr, guestAddr)
	h.send(guestAddr, Bye{Token: wg.Token})
	h.advance(16 * time.Second)
	all := strings.Join(h.logs, "\n")
	for _, secret := range []string{
		"198.51.100.1", "203.0.113.7", "host", "guest",
		hex.EncodeToString(wh.Token[:]), hex.EncodeToString(wg.Token[:]),
	} {
		if strings.Contains(all, secret) {
			t.Errorf("log contains %q:\n%s", secret, all)
		}
	}
	for _, want := range []string{"created", "joined", "peer left", "closed"} {
		if !strings.Contains(all, want) {
			t.Errorf("log lacks %q:\n%s", want, all)
		}
	}
}

func TestSourceTable(t *testing.T) {
	h := newHarness(t, Config{MaxTrackedSources: 2})
	h.create(ap("192.0.2.1:1"), "a")
	h.create(ap("192.0.2.2:1"), "b")
	// Table full: a third IP is dropped silently.
	expectNone(t, h.send(ap("192.0.2.3:1"), Hello{Mode: ModeCreate, Build: build, Name: "c"}))
	// Idle entries are collected after a minute, freeing space.
	h.r.clients = map[netip.AddrPort]*client{} // forget the clients so nothing refreshes the table
	h.r.rooms = map[RoomCode]*room{}
	h.advance(62 * time.Second)
	if len(h.r.sources) != 0 {
		t.Fatalf("%d sources left after GC", len(h.r.sources))
	}
	h.create(ap("192.0.2.3:1"), "c")
}

func containsLog(logs []string, s string) bool {
	for _, l := range logs {
		if strings.Contains(l, s) {
			return true
		}
	}
	return false
}

