package relay

import (
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/hex"
	"errors"
	"io"
	"net/netip"
	"time"
	"unicode/utf8"
)

// Config holds the limits of the state machine. Zero values take the defaults
// from the specification (see setDefaults).
type Config struct {
	RequirePrivatePasswords bool          // reject passwordless legacy/unlisted room creation
	MaxRooms                int           // rooms open at once (default 512); beyond: ERROR 5
	MaxClients              int           // clients registered at once (default 1024); beyond: ERROR 5
	IdleTimeout             time.Duration // a client silent this long is removed (default 15 s)
	RoomExpiry              time.Duration // a legacy room that never got a second player is deleted after this (default 10 min)
	HellosPerMinute         int           // HELLOs per source IP per sliding minute (default 10); beyond: ERROR 6
	MsgsPerSecond           int           // datagrams per source IP per second (default 400); beyond: silent drop
	MaxTrackedSources       int           // size cap of the per-IP rate-limit table (default 100000)
	VoteTime                time.Duration // length of the venue vote (default 15 s)
	PlayTimeout             time.Duration // a match without every OVER returns to the lobby after this (default 30 min)

	// Ranked enables the ranked queue (RANKED); the Server sets it when it has an account
	// service to check tickets with. Off, RANKED is answered QUEUE refused.
	Ranked bool

	// Rand is the randomness source for room codes, tokens, match seeds and
	// the log salt. Default: crypto/rand.Reader. Tests may inject a
	// deterministic reader.
	Rand io.Reader

	// Intn returns a uniform random integer in [0, n) for n >= 2; it draws the
	// vote candidates and breaks ties. Default: unbiased draws from Rand.
	// Tests may inject a scripted function.
	Intn func(n int) int

	// Logf receives one line per room event. Nil disables event logging.
	Logf func(format string, args ...any)
}

func (c *Config) setDefaults() {
	if c.MaxRooms <= 0 {
		c.MaxRooms = 512
	}
	if c.MaxClients <= 0 {
		c.MaxClients = 1024
	}
	if c.IdleTimeout <= 0 {
		c.IdleTimeout = 15 * time.Second
	}
	if c.RoomExpiry <= 0 {
		c.RoomExpiry = 10 * time.Minute
	}
	if c.HellosPerMinute <= 0 {
		c.HellosPerMinute = 10
	}
	if c.MsgsPerSecond <= 0 {
		c.MsgsPerSecond = 400
	}
	if c.MaxTrackedSources <= 0 {
		c.MaxTrackedSources = 100000
	}
	if c.VoteTime <= 0 {
		c.VoteTime = 15 * time.Second
	}
	if c.PlayTimeout <= 0 {
		c.PlayTimeout = 30 * time.Minute
	}
	if c.Rand == nil {
		c.Rand = rand.Reader
	}
	if c.Intn == nil {
		c.Intn = readerIntn(c.Rand)
	}
	if c.Logf == nil {
		c.Logf = func(string, ...any) {}
	}
}

// Packet is one datagram the caller must send.
type Packet struct {
	To   netip.AddrPort
	Data []byte
}

// Stats are traffic counters since the last TakeStats call.
type Stats struct {
	PacketsIn, BytesIn   uint64 // every datagram received, valid or not
	PacketsOut, BytesOut uint64 // every datagram the relay asked to send
	// Dropped counts received datagrams that caused nothing to be sent:
	// malformed, rate-limited, unknown token after the first ERROR 7, DATA
	// while alone in the room or in the lobby, BYE or LEAVE that nobody had
	// to be told about.
	Dropped uint64
	// Moved counts clients that came back from a new address with their token.
	Moved uint64
}

// client is one registered player.
type client struct {
	passwordFails  int
	passwordWindow time.Time
	addr           netip.AddrPort // source address and port; part of the identity
	token          Token
	slot           uint8 // SlotNone while in the lobby
	name           string
	build          Build
	room           *room // nil while in the lobby
	lastSeen       time.Time
	id             string // anonymised address, for logs only
	welcome        []byte // the encoded WELCOME, re-sent on a repeated HELLO

	// lobby is set once the client has shown it speaks the lobby extension
	// (HELLO mode 2, or any lobby request). Only such clients get ROOM and START.
	lobby   bool
	ready   bool
	fighter uint8 // from the last READY; FighterNone before
	vote    uint8 // candidate index, or VoteNone
	over    bool  // sent OVER for the current match

	queue   *queued // in the ranked queue (ranked.go), nil if not
	account int64   // the account the account service vouched for (ranked), 0 none
}

// room holds up to MaxSlots clients.
type room struct {
	protected    bool
	passwordSalt [16]byte
	passwordHash [32]byte
	code         RoomCode
	build        Build
	slots        [MaxSlots]*client
	created      time.Time
	seq          uint64 // creation order, for "newest first" and "oldest"
	everFull     bool   // a second player has joined at some point

	// legacy rooms come from HELLO mode 0: two players, joined in slot 1 only,
	// fixed settings, and they expire when no second player ever came.
	legacy   bool
	settings RoomSettings

	phase      uint8
	candidates []uint8   // phases 1 and 2: the venues voted on
	voteEnd    time.Time // phase 1
	playStart  time.Time // phase 2
	start      []byte    // phase 2: the encoded START, re-sent on a late VOTE

	// ranked rooms come from the ranked queue: fixed settings, and their matches are reported
	// to the account service while they hold the two paired accounts.
	ranked         bool
	rankedAccounts [2]int64
}

// source is the rate-limit state of one source IP (IPv6: one /64).
type source struct {
	lastSeen time.Time

	// Fixed one-second window for the per-IP message limit.
	winStart time.Time
	winCount int

	// Ring of the times of the last HellosPerMinute counted HELLOs; the entry
	// at helloNext is the oldest.
	hellos    []time.Time
	helloNext int

	lastErr6 time.Time // ERROR 6 is sent at most once per second per source
	lastErr7 time.Time // ERROR 7 is sent at most once per second per source
}

// Relay is the room/client state machine. It is not safe for concurrent use;
// the Server serialises calls with a mutex. All methods take the current time
// as an argument, so tests can drive it with a fake clock.
type Relay struct {
	cfg     Config
	clients map[netip.AddrPort]*client
	byToken map[Token]*client // the same clients by token, for a client whose address changed
	rooms   map[RoomCode]*room
	sources map[netip.Addr]*source
	salt    [16]byte // per-process salt for anonymised log ids
	stats   Stats
	lastGC  time.Time
	roomSeq uint64

	queue    []*queued // the ranked queue, in arrival order
	lookups  []RankedLookup
	pairings []RankedPairing
}

// NewRelay builds a state machine with the given limits.
func NewRelay(cfg Config) (*Relay, error) {
	cfg.setDefaults()
	r := &Relay{
		cfg:     cfg,
		clients: make(map[netip.AddrPort]*client),
		byToken: make(map[Token]*client),
		rooms:   make(map[RoomCode]*room),
		sources: make(map[netip.Addr]*source),
	}
	if _, err := io.ReadFull(cfg.Rand, r.salt[:]); err != nil {
		return nil, err
	}
	return r, nil
}

// Rooms returns the number of open rooms.
func (r *Relay) Rooms() int { return len(r.rooms) }

// Clients returns the number of registered clients.
func (r *Relay) Clients() int { return len(r.clients) }

// TakeStats returns the counters and resets them.
func (r *Relay) TakeStats() Stats {
	s := r.stats
	r.stats = Stats{}
	return s
}

// Handle processes one received datagram and returns the datagrams to send.
// b is not retained after Handle returns.
//
// Anything malformed is dropped without a reply. Every reply to an address
// that is not a registered client is no larger than the datagram that caused
// it, so the relay cannot be used to amplify traffic.
func (r *Relay) Handle(now time.Time, from netip.AddrPort, b []byte) []Packet {
	r.stats.PacketsIn++
	r.stats.BytesIn += uint64(len(b))

	out := r.handle(now, from, b)
	if len(out) == 0 {
		r.stats.Dropped++
	}
	r.countOut(out)
	return out
}

func (r *Relay) handle(now time.Time, from netip.AddrPort, b []byte) []Packet {
	if !from.IsValid() || from.Port() == 0 {
		return nil
	}
	src := r.source(now, from.Addr())
	if src == nil || !src.allowMsg(now, r.cfg.MsgsPerSecond) {
		return nil
	}
	msg, err := DecodeClient(b)
	if err != nil {
		return nil
	}
	switch m := msg.(type) {
	case Hello:
		return r.hello(now, from, src, m, len(b))
	case ClientData:
		c, out := r.auth(now, from, src, m.Token, len(b))
		if c == nil {
			return out
		}
		c.lastSeen = now
		if c.room == nil {
			return nil // in the lobby: nobody to forward to
		}
		// Forwarded to every other member of the room, whatever its phase.
		var data []byte
		for _, o := range c.room.members() {
			if o == c {
				continue
			}
			if data == nil {
				if data, err = Encode(ServerData{From: c.slot, Payload: m.Payload}); err != nil {
					return nil
				}
			}
			out = append(out, Packet{To: o.addr, Data: data})
		}
		return out // empty while alone in the room
	case Ping:
		c, out := r.auth(now, from, src, m.Token, len(b))
		if c == nil {
			return out
		}
		c.lastSeen = now
		return []Packet{{To: from, Data: mustEncode(Pong{Nonce: m.Nonce})}}
	case Bye:
		c, out := r.auth(now, from, src, m.Token, len(b))
		if c == nil {
			return out
		}
		return r.removeClient(now, c, "bye")
	case lobbyRequest:
		c, out := r.auth(now, from, src, m.token(), len(b))
		if c == nil {
			return out
		}
		c.lastSeen = now
		c.lobby = true
		return r.lobbyRequest(now, c, m, len(b))
	}
	return nil
}

// hello handles HELLO: an idempotent re-send, a create, a join, or a lobby entry.
func (r *Relay) hello(now time.Time, from netip.AddrPort, src *source, h Hello, reqLen int) []Packet {
	// A repeated HELLO from an address that already has a token gets the same
	// WELCOME (and the PEER, if the room of two is full, since that may have
	// been lost too). It is not counted against the HELLO rate limit.
	if c := r.clients[from]; c != nil {
		c.lastSeen = now
		out := []Packet{{To: from, Data: c.welcome}}
		if p := c.pairPeer(); p != nil {
			out = append(out, Packet{To: from, Data: mustEncode(Peer{Slot: p.slot, Name: p.name})})
		}
		return out
	}

	if !src.allowHello(now) {
		if now.Sub(src.lastErr6) < time.Second {
			return nil
		}
		src.lastErr6 = now
		return r.errorTo(from, ErrRateLimited, reqLen)
	}
	if !validName(h.Name) {
		return r.errorTo(from, ErrBadName, reqLen)
	}
	if len(r.clients) >= r.cfg.MaxClients {
		return r.errorTo(from, ErrServerFull, reqLen)
	}

	switch h.Mode {
	case ModeCreate:
		if r.cfg.RequirePrivatePasswords {
			return r.errorTo(from, ErrNotAllowed, reqLen)
		}
		if len(r.rooms) >= r.cfg.MaxRooms {
			return r.errorTo(from, ErrServerFull, reqLen)
		}
		// A legacy room: two players, not listed, named after the host, one venue.
		settings := RoomSettings{Name: h.Name, Public: 0, Mode: 0, Max: 2, Rounds: 0, Pool: []uint8{1}}
		rm, err := r.newRoom(now, h.Build, settings, true)
		if err != nil {
			return nil
		}
		c, err := r.newClient(now, from, h, rm, SlotHost)
		if err != nil {
			delete(r.rooms, rm.code)
			return nil
		}
		r.cfg.Logf("room %s created by c=%s", rm.code, c.id)
		return []Packet{{To: from, Data: c.welcome}}

	case ModeJoin:
		rm := r.rooms[h.Room]
		if rm == nil {
			return r.errorTo(from, ErrRoomNotFound, reqLen)
		}
		if rm.protected {
			return r.errorTo(from, ErrPassword, reqLen)
		}
		// In a legacy room HELLO mode 1 joins as slot 1 only: if the guest slot
		// is taken the room is full, even when the host has left (see README).
		slot, code := rm.joinable(h.Build)
		if code != 0 {
			return r.errorTo(from, code, reqLen)
		}
		c, err := r.newClient(now, from, h, rm, slot)
		if err != nil {
			return nil
		}
		return append([]Packet{{To: from, Data: c.welcome}}, r.joined(now, rm, c)...)

	case ModeLobby:
		c, err := r.newClient(now, from, h, nil, SlotNone)
		if err != nil {
			return nil
		}
		c.lobby = true
		r.cfg.Logf("lobby entered by c=%s", c.id)
		return []Packet{{To: from, Data: c.welcome}}
	}
	return nil
}

// newClient registers a client with a fresh random token, in slot of rm, or
// in the lobby when rm is nil.
func (r *Relay) newClient(now time.Time, from netip.AddrPort, h Hello, rm *room, slot uint8) (*client, error) {
	c := &client{addr: from, slot: SlotNone, name: h.Name, build: h.Build, lastSeen: now, id: r.anon(from),
		fighter: FighterNone, vote: VoteNone}
	if _, err := io.ReadFull(r.cfg.Rand, c.token[:]); err != nil {
		return nil, err
	}
	var code RoomCode
	if rm != nil {
		code = rm.code
		rm.place(c, slot)
	}
	c.welcome = mustEncode(Welcome{Token: c.token, Slot: slot, Room: code})
	r.clients[from] = c
	r.byToken[c.token] = c
	return c, nil
}

// auth finds the client that owns token at address from. A known token from a
// new address moves the client there (a home router that changed its port, a
// laptop that changed networks): the token is the client's secret, so the
// session, its room and a running match go on. Any other mismatch is answered
// with ERROR 7 at most once per second per source.
func (r *Relay) auth(now time.Time, from netip.AddrPort, src *source, tok Token, reqLen int) (*client, []Packet) {
	if c := r.clients[from]; c != nil && subtle.ConstantTimeCompare(c.token[:], tok[:]) == 1 {
		return c, nil
	}
	if c := r.byToken[tok]; c != nil && r.clients[from] == nil {
		delete(r.clients, c.addr)
		r.cfg.Logf("client c=%s moved to a new address", c.id)
		c.addr = from
		r.clients[from] = c
		r.stats.Moved++
		return c, nil
	}
	if now.Sub(src.lastErr7) < time.Second {
		return nil, nil
	}
	src.lastErr7 = now
	return nil, r.errorTo(from, ErrUnknownToken, reqLen)
}

// removeClient unregisters c, taking it out of its room first (see leaveRoom).
func (r *Relay) removeClient(now time.Time, c *client, reason string) []Packet {
	delete(r.clients, c.addr)
	delete(r.byToken, c.token)
	r.dequeue(c)
	if c.room == nil {
		r.cfg.Logf("lobby left by c=%s reason=%s", c.id, reason)
		return nil
	}
	return r.leaveRoom(now, c, reason)
}

// Tick runs the timers: idle clients, legacy rooms that never got a second
// player, the vote countdown, the playing timeout, and garbage collection of
// the per-IP table. Call it about once a second.
func (r *Relay) Tick(now time.Time) []Packet {
	var out []Packet
	for _, c := range r.clients {
		if now.Sub(c.lastSeen) >= r.cfg.IdleTimeout {
			out = append(out, r.removeClient(now, c, "timeout")...)
		}
	}
	for _, rm := range r.rooms {
		out = append(out, r.tickRoom(now, rm)...)
	}
	out = append(out, r.tickRanked(now)...)
	for code, rm := range r.rooms {
		if !rm.legacy || rm.everFull || now.Sub(rm.created) < r.cfg.RoomExpiry {
			continue
		}
		for i, c := range rm.slots {
			if c != nil {
				delete(r.clients, c.addr)
				delete(r.byToken, c.token)
				rm.slots[i] = nil
			}
		}
		delete(r.rooms, code)
		r.cfg.Logf("room %s expired (no second player after %s)", code, r.cfg.RoomExpiry)
	}
	if now.Sub(r.lastGC) >= 10*time.Second {
		r.lastGC = now
		for ip, s := range r.sources {
			// Older than the HELLO window: nothing left to remember.
			if now.Sub(s.lastSeen) > time.Minute+time.Second {
				delete(r.sources, ip)
			}
		}
	}
	r.countOut(out)
	return out
}

// ---- helpers ----

func (r *Relay) countOut(out []Packet) {
	for _, p := range out {
		r.stats.PacketsOut++
		r.stats.BytesOut += uint64(len(p.Data))
	}
}

// errorTo builds an ERROR reply. The text is left empty if the reply would
// otherwise be larger than the request (anti-amplification).
func (r *Relay) errorTo(to netip.AddrPort, code uint8, reqLen int) []Packet {
	text := ErrorText[code]
	if HeaderLen+2+len(text) > reqLen {
		text = ""
	}
	return []Packet{{To: to, Data: mustEncode(Error{Code: code, Text: text})}}
}

// source returns the rate-limit state for ip, creating it if needed. It
// returns nil when the table is full (the datagram is then dropped).
func (r *Relay) source(now time.Time, ip netip.Addr) *source {
	key := sourceKey(ip)
	s := r.sources[key]
	if s == nil {
		if len(r.sources) >= r.cfg.MaxTrackedSources {
			return nil
		}
		s = &source{hellos: make([]time.Time, r.cfg.HellosPerMinute)}
		r.sources[key] = s
	}
	s.lastSeen = now
	return s
}

// sourceKey maps an address to its rate-limit key: the IPv4 address, or the
// /64 prefix for IPv6 (one IPv6 host usually owns a whole /64).
func sourceKey(ip netip.Addr) netip.Addr {
	ip = ip.Unmap()
	if ip.Is6() {
		if p, err := ip.Prefix(64); err == nil {
			return p.Addr()
		}
	}
	return ip
}

// allowMsg counts one datagram in a fixed one-second window.
func (s *source) allowMsg(now time.Time, perSecond int) bool {
	if now.Sub(s.winStart) >= time.Second || now.Before(s.winStart) {
		s.winStart = now
		s.winCount = 0
	}
	s.winCount++
	return s.winCount <= perSecond
}

// allowHello admits a HELLO if fewer than len(s.hellos) were admitted in the
// last minute, and records it.
func (s *source) allowHello(now time.Time) bool {
	oldest := s.hellos[s.helloNext]
	if !oldest.IsZero() && now.Sub(oldest) < time.Minute {
		return false
	}
	s.hellos[s.helloNext] = now
	s.helloNext = (s.helloNext + 1) % len(s.hellos)
	return true
}

// validName: 1..24 bytes of valid UTF-8.
func validName(name string) bool {
	return len(name) >= 1 && len(name) <= MaxNameLen && utf8.ValidString(name)
}

var errNoRoomCode = errors.New("could not find a free room code")

// newRoomCode draws a random unused room code from RoomAlphabet.
func (r *Relay) newRoomCode() (RoomCode, error) {
	var code RoomCode
	for attempt := 0; attempt < 16; attempt++ {
		var raw [RoomCodeLen]byte
		if _, err := io.ReadFull(r.cfg.Rand, raw[:]); err != nil {
			return code, err
		}
		for i, b := range raw {
			code[i] = RoomAlphabet[b&31] // 32 symbols: no modulo bias
		}
		if _, taken := r.rooms[code]; !taken {
			return code, nil
		}
	}
	return code, errNoRoomCode
}

// newRoom opens a room with a fresh code. The caller places the first client.
func (r *Relay) newRoom(now time.Time, b Build, s RoomSettings, legacy bool) (*room, error) {
	code, err := r.newRoomCode()
	if err != nil {
		return nil, err
	}
	r.roomSeq++
	rm := &room{code: code, build: b, created: now, seq: r.roomSeq, legacy: legacy, settings: s.clone()}
	r.rooms[code] = rm
	return rm, nil
}

// anon returns a short salted hash of the address for logs, so log lines can
// be correlated without recording who played.
func (r *Relay) anon(a netip.AddrPort) string {
	h := sha256.New()
	h.Write(r.salt[:])
	h.Write([]byte(a.String()))
	return hex.EncodeToString(h.Sum(nil)[:4])
}

// mustEncode encodes a message whose fields the relay itself controls.
func mustEncode(m Message) []byte {
	b, err := Encode(m)
	if err != nil {
		panic("relay: encode " + err.Error())
	}
	return b
}
