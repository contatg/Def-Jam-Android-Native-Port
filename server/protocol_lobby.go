package relay

import (
	"encoding/binary"
	"errors"
)

// Lobby extension (docs/netplay-protocol.md, "Lobby extension (rooms, quick
// match, voting)"). Same header and version byte; message types from 0x10.

// Lobby constants.
const (
	ModeLobby   uint8 = 2    // HELLO mode: register in the lobby, in no room
	SlotNone    uint8 = 0xFF // WELCOME slot of a lobby client
	ModeAny     uint8 = 0xFF // LIST mode filter: every match type
	VoteNone    uint8 = 0xFF // ROOM per-player `voted`: not yet
	FighterNone uint8 = 0xFF // ROOM per-player `fighter` before the first READY (server default)

	MaxSlots      = 4  // players per room, upper bound of Settings.max
	MinPlayers    = 2  // lower bound of Settings.max, and players needed to start a vote
	MaxPool       = 32 // venues in Settings.pool
	MaxCandidates = 3  // venues drawn from the pool for a vote
	MaxListed     = 30 // rooms in one ROOMS reply
	MaxWho        = 40 // players named in one PLAYERS reply
)

// PLAYERS per-player state.
const (
	PlayerLobby   uint8 = 0 // in the hub, in no room
	PlayerRoom    uint8 = 1 // in a room that is not playing
	PlayerPlaying uint8 = 2 // in a room that is playing
)

// Room phases.
const (
	PhaseLobby   uint8 = 0
	PhaseVoting  uint8 = 1
	PhasePlaying uint8 = 2
)

// Lobby message types.
const (
	TypeList     MsgType = 0x10 // client -> server
	TypeRooms    MsgType = 0x11 // server -> client
	TypeCreate   MsgType = 0x12 // client -> server
	TypeJoin     MsgType = 0x13 // client -> server
	TypeQuick    MsgType = 0x14 // client -> server
	TypeSettings MsgType = 0x15 // client -> server
	TypeReady    MsgType = 0x16 // client -> server
	TypeVote     MsgType = 0x17 // client -> server
	TypeLeave    MsgType = 0x18 // client -> server
	TypeRoom     MsgType = 0x19 // server -> client
	TypeStart    MsgType = 0x1A // server -> client
	TypeOver     MsgType = 0x1B // client -> server
	TypeWho      MsgType = 0x1C // client -> server
	TypePlayers  MsgType = 0x1D // server -> client
	TypeRanked   MsgType = 0x1E // client -> server
	TypeQueue    MsgType = 0x1F // server -> client
)

// TicketLen is the size of a ranked ticket: a one-time code from the account service that
// stands for the player's account for a minute, so the long-lived session never travels
// over UDP.
const TicketLen = 16

// QUEUE states.
const (
	QueueChecking  uint8 = 0 // the ticket is being checked with the account service
	QueueSearching uint8 = 1 // in the ranked queue
	QueueRefused   uint8 = 2 // not signed in, the ticket expired, or ranked play is off
	QueueOff       uint8 = 3 // out of the queue (LEAVE)
	QueueFound     uint8 = 4 // paired: the ranked room's ROOM follows
)

// Lobby ERROR codes.
const (
	ErrNotAllowed uint8 = 8 // not the host, or the wrong phase
	ErrRoomState  uint8 = 9 // not in a room, or already in one
)

// UnsafeFighters are fighter.xml IDs that crash the game when loaded into a Battle-mode match
// (Teck, Santos, O.E, Eric Sermon: story figures without a select portrait). FFNY Online builds
// 13 and 14 offered them; the relay plays SafeFighter instead, the same on every PC.
var UnsafeFighters = map[uint8]bool{40: true, 41: true, 52: true, 78: true}

// SafeFighter replaces an unsafe fighter.
const SafeFighter uint8 = 55

func safeFighter(f uint8) uint8 {
	if UnsafeFighters[f] {
		return SafeFighter
	}
	return f
}

// QuickPool is the venue pool of a room created by QUICK (The Foundation, The Terror Dome,
// 125th Street Station).
var QuickPool = []uint8{1, 6, 11}

// UnsafeVenues are venue feIDs left out of every vote: The Pit (19), reported to crash online
// (2026-10-21), until it is tested. A pool of only unsafe venues votes on QuickPool.
var UnsafeVenues = map[uint8]bool{19: true}

// RoomSettings is the Settings block of CREATE, SETTINGS and ROOM.
type RoomSettings struct {
	Name   string  // 1..24 bytes of UTF-8 (the server answers ERROR 4 otherwise)
	Public uint8   // 1: listed and found by QUICK; 0: join by code only
	Mode   uint8   // front-end match type; never ModeAny
	Max    uint8   // players, 2..4
	Rounds uint8   // 0 = the game's default
	Pool   []uint8 // 1..32 venue feIDs to vote on
}

func (s RoomSettings) equal(o RoomSettings) bool {
	if s.Name != o.Name || s.Public != o.Public || s.Mode != o.Mode || s.Max != o.Max ||
		s.Rounds != o.Rounds || len(s.Pool) != len(o.Pool) {
		return false
	}
	for i := range s.Pool {
		if s.Pool[i] != o.Pool[i] {
			return false
		}
	}
	return true
}

func (s RoomSettings) clone() RoomSettings {
	s.Pool = append([]uint8(nil), s.Pool...)
	return s
}

// validShape reports whether every field except the name is in range. The
// decoder drops a Settings block that fails it; the name is checked by the
// server (ERROR 4), as for HELLO.
func (s RoomSettings) validShape() bool {
	return s.Public <= 1 && s.Mode != ModeAny && s.Max >= MinPlayers && s.Max <= MaxSlots &&
		len(s.Pool) >= 1 && len(s.Pool) <= MaxPool
}

func (s RoomSettings) append(b []byte) ([]byte, error) {
	b, err := appendString(b, s.Name)
	if err != nil {
		return b, err
	}
	if len(s.Pool) > 255 {
		return b, ErrListTooLong
	}
	b = append(b, s.Public, s.Mode, s.Max, s.Rounds, byte(len(s.Pool)))
	return append(b, s.Pool...), nil
}

// ---- client -> server ----

// List asks for the public rooms (ROOMS). Mode filters by match type; ModeAny for all.
type List struct {
	Token Token
	Mode  uint8
}

// Create makes a room with the given settings and the sender as its host.
type Create struct {
	Token    Token
	Settings RoomSettings
}

// Join enters a room by code.
type Join struct {
	Token Token
	Room  RoomCode
}

// Quick joins the best public room with that mode and max, or creates one.
type Quick struct {
	Token Token
	Mode  uint8 // never ModeAny
	Max   uint8 // 2..4
}

// SetSettings changes the room settings (the host only, in phase 0).
type SetSettings struct {
	Token    Token
	Settings RoomSettings
}

// Ready sets the sender's ready flag and wanted fighter.
type Ready struct {
	Token   Token
	Ready   uint8 // 0 or 1
	Fighter uint8 // fighter.xml ID
}

// Vote picks one of the candidates (0..2).
type Vote struct {
	Token  Token
	Choice uint8
}

// Leave returns the sender to the lobby with the same token.
type Leave struct {
	Token Token
}

// Over says the sender's match has ended.
type Over struct {
	Token Token
}

// Ranked joins the ranked queue (One on One against a player of a similar rating) with the
// fighter to play. Repeated while waiting it changes nothing but the fighter.
type Ranked struct {
	Token   Token
	Ticket  [TicketLen]byte
	Fighter uint8
}

// ---- server -> client ----

// RoomInfo is one entry of ROOMS.
type RoomInfo struct {
	Room    RoomCode
	Name    string
	Mode    uint8
	Players uint8
	Max     uint8
	Phase   uint8
}

// Rooms answers LIST.
type Rooms struct {
	Rooms []RoomInfo // at most MaxListed
}

// RoomPlayer is one player in ROOM.
type RoomPlayer struct {
	Slot    uint8
	Name    string
	Ready   uint8
	Fighter uint8
	Voted   uint8 // candidate index, or VoteNone
}

// RoomState is the ROOM message: the whole room as seen by slot You.
type RoomState struct {
	Room       RoomCode
	You        uint8
	Host       uint8
	Phase      uint8
	Settings   RoomSettings
	Candidates []uint8 // 0..3 venues
	Votes      []uint8 // votes per candidate; same length as Candidates
	MsLeft     uint16  // voting countdown
	Players    []RoomPlayer
}

// StartPlayer is one player in START.
type StartPlayer struct {
	Slot    uint8
	Fighter uint8
}

// Start begins a match.
type Start struct {
	Seed    uint32
	Mode    uint8
	Rounds  uint8
	Venue   uint8
	Players []StartPlayer
}

// Who asks who is online (the hub's player count and list).
type Who struct {
	Token Token
}

// PlayerInfo is one player in PLAYERS.
type PlayerInfo struct {
	Name  string
	State uint8 // PlayerLobby, PlayerRoom or PlayerPlaying
}

// Players answers WHO: everyone connected, the rooms, and up to MaxWho players of the
// asker's build who use the lobby, those in the hub first.
type Players struct {
	Online uint16
	Rooms  uint16
	List   []PlayerInfo
}

// Queue is the sender's place in the ranked queue: its state, how long it has searched and
// how many are searching.
type Queue struct {
	State     uint8
	Waited    uint16 // seconds
	Searching uint16
}

func (Ranked) Type() MsgType      { return TypeRanked }
func (Queue) Type() MsgType       { return TypeQueue }
func (Who) Type() MsgType         { return TypeWho }
func (Players) Type() MsgType     { return TypePlayers }
func (List) Type() MsgType        { return TypeList }
func (Create) Type() MsgType      { return TypeCreate }
func (Join) Type() MsgType        { return TypeJoin }
func (Quick) Type() MsgType       { return TypeQuick }
func (SetSettings) Type() MsgType { return TypeSettings }
func (Ready) Type() MsgType       { return TypeReady }
func (Vote) Type() MsgType        { return TypeVote }
func (Leave) Type() MsgType       { return TypeLeave }
func (Over) Type() MsgType        { return TypeOver }
func (Rooms) Type() MsgType       { return TypeRooms }
func (RoomState) Type() MsgType   { return TypeRoom }
func (Start) Type() MsgType       { return TypeStart }

// lobbyRequest is a client-to-server lobby message; all carry a token.
type lobbyRequest interface {
	Message
	token() Token
}

func (m List) token() Token        { return m.Token }
func (m Create) token() Token      { return m.Token }
func (m Join) token() Token        { return m.Token }
func (m Quick) token() Token       { return m.Token }
func (m SetSettings) token() Token { return m.Token }
func (m Ready) token() Token       { return m.Token }
func (m Vote) token() Token        { return m.Token }
func (m Leave) token() Token       { return m.Token }
func (m Over) token() Token        { return m.Token }
func (m Who) token() Token         { return m.Token }
func (m Ranked) token() Token      { return m.Token }

// More encoding errors.
var (
	ErrListTooLong = errors.New("list longer than 255 entries")
	ErrVoteCount   = errors.New("ROOM needs one vote count per candidate")
)

func (m List) appendBody(b []byte) ([]byte, error) {
	return append(append(b, m.Token[:]...), m.Mode), nil
}

func (m Create) appendBody(b []byte) ([]byte, error) {
	return m.Settings.append(append(b, m.Token[:]...))
}

func (m Join) appendBody(b []byte) ([]byte, error) {
	return append(append(b, m.Token[:]...), m.Room[:]...), nil
}

func (m Quick) appendBody(b []byte) ([]byte, error) {
	return append(append(b, m.Token[:]...), m.Mode, m.Max), nil
}

func (m SetSettings) appendBody(b []byte) ([]byte, error) {
	return m.Settings.append(append(b, m.Token[:]...))
}

func (m Ready) appendBody(b []byte) ([]byte, error) {
	return append(append(b, m.Token[:]...), m.Ready, m.Fighter), nil
}

func (m Vote) appendBody(b []byte) ([]byte, error) {
	return append(append(b, m.Token[:]...), m.Choice), nil
}

func (m Leave) appendBody(b []byte) ([]byte, error) { return append(b, m.Token[:]...), nil }
func (m Over) appendBody(b []byte) ([]byte, error)  { return append(b, m.Token[:]...), nil }
func (m Who) appendBody(b []byte) ([]byte, error)   { return append(b, m.Token[:]...), nil }

func (m Ranked) appendBody(b []byte) ([]byte, error) {
	return append(append(append(b, m.Token[:]...), m.Ticket[:]...), m.Fighter), nil
}

func (m Queue) appendBody(b []byte) ([]byte, error) {
	b = append(b, m.State)
	b = binary.LittleEndian.AppendUint16(b, m.Waited)
	return binary.LittleEndian.AppendUint16(b, m.Searching), nil
}

func (m Players) appendBody(b []byte) ([]byte, error) {
	if len(m.List) > 255 {
		return b, ErrListTooLong
	}
	b = binary.LittleEndian.AppendUint16(b, m.Online)
	b = binary.LittleEndian.AppendUint16(b, m.Rooms)
	b = append(b, byte(len(m.List)))
	for _, p := range m.List {
		var err error
		if b, err = appendString(b, p.Name); err != nil {
			return b, err
		}
		b = append(b, p.State)
	}
	return b, nil
}

func (m Rooms) appendBody(b []byte) ([]byte, error) {
	if len(m.Rooms) > 255 {
		return b, ErrListTooLong
	}
	b = append(b, byte(len(m.Rooms)))
	for _, r := range m.Rooms {
		var err error
		b = append(b, r.Room[:]...)
		if b, err = appendString(b, r.Name); err != nil {
			return b, err
		}
		b = append(b, r.Mode, r.Players, r.Max, r.Phase)
	}
	return b, nil
}

func (m RoomState) appendBody(b []byte) ([]byte, error) {
	b = append(b, m.Room[:]...)
	b = append(b, m.You, m.Host, m.Phase)
	b, err := m.Settings.append(b)
	if err != nil {
		return b, err
	}
	if len(m.Candidates) > 255 || len(m.Players) > 255 {
		return b, ErrListTooLong
	}
	if len(m.Votes) != len(m.Candidates) {
		return b, ErrVoteCount
	}
	b = append(b, byte(len(m.Candidates)))
	b = append(b, m.Candidates...)
	b = append(b, m.Votes...)
	b = binary.LittleEndian.AppendUint16(b, m.MsLeft)
	b = append(b, byte(len(m.Players)))
	for _, p := range m.Players {
		b = append(b, p.Slot)
		if b, err = appendString(b, p.Name); err != nil {
			return b, err
		}
		b = append(b, p.Ready, p.Fighter, p.Voted)
	}
	return b, nil
}

func (m Start) appendBody(b []byte) ([]byte, error) {
	if len(m.Players) > 255 {
		return b, ErrListTooLong
	}
	b = binary.LittleEndian.AppendUint32(b, m.Seed)
	b = append(b, m.Mode, m.Rounds, m.Venue, byte(len(m.Players)))
	for _, p := range m.Players {
		b = append(b, p.Slot, p.Fighter)
	}
	return b, nil
}

// ---- decoding ----

// cursor reads fields front to back; any read past the end clears ok.
type cursor struct {
	b  []byte
	ok bool
}

func newCursor(b []byte) *cursor { return &cursor{b: b, ok: true} }

func (c *cursor) take(n int) []byte {
	if !c.ok || n > len(c.b) {
		c.ok = false
		return nil
	}
	v := c.b[:n]
	c.b = c.b[n:]
	return v
}

func (c *cursor) u8() uint8 {
	v := c.take(1)
	if !c.ok {
		return 0
	}
	return v[0]
}

func (c *cursor) u16() uint16 {
	v := c.take(2)
	if !c.ok {
		return 0
	}
	return binary.LittleEndian.Uint16(v)
}

func (c *cursor) u32() uint32 {
	v := c.take(4)
	if !c.ok {
		return 0
	}
	return binary.LittleEndian.Uint32(v)
}

// list reads n bytes into a fresh slice (nil when n is 0).
func (c *cursor) list(n int) []uint8 { return append([]uint8(nil), c.take(n)...) }

func (c *cursor) str() string { return string(c.take(int(c.u8()))) }

func (c *cursor) token() (t Token) {
	copy(t[:], c.take(TokenLen))
	return t
}

func (c *cursor) room() (r RoomCode) {
	copy(r[:], c.take(RoomCodeLen))
	return r
}

// end reports whether every read succeeded and nothing is left over.
func (c *cursor) end() bool { return c.ok && len(c.b) == 0 }

func (c *cursor) settings() RoomSettings {
	var s RoomSettings
	s.Name = c.str()
	s.Public = c.u8()
	s.Mode = c.u8()
	s.Max = c.u8()
	s.Rounds = c.u8()
	s.Pool = c.list(int(c.u8()))
	if !s.validShape() {
		c.ok = false
	}
	return s
}

// decodeLobbyClient parses the body of a client-to-server lobby message.
// Every field is range-checked here except Settings.name (ERROR 4).
func decodeLobbyClient(t MsgType, body []byte) (Message, error) {
	c := newCursor(body)
	tok := c.token()
	var m Message
	switch t {
	case TypeCreatePrivate:
		s := c.settings()
		var key [32]byte
		copy(key[:], c.take(32))
		m = CreatePrivate{Token: tok, Settings: s, Credential: key}
	case TypeJoinPrivate:
		room := c.room()
		var key [32]byte
		copy(key[:], c.take(32))
		m = JoinPrivate{Token: tok, Room: room, Credential: key}
	case TypeList:
		m = List{Token: tok, Mode: c.u8()}
	case TypeCreate:
		m = Create{Token: tok, Settings: c.settings()}
	case TypeJoin:
		m = Join{Token: tok, Room: c.room()}
	case TypeQuick:
		q := Quick{Token: tok, Mode: c.u8(), Max: c.u8()}
		if q.Mode == ModeAny || q.Max < MinPlayers || q.Max > MaxSlots {
			return nil, ErrMalformed
		}
		m = q
	case TypeSettings:
		m = SetSettings{Token: tok, Settings: c.settings()}
	case TypeReady:
		r := Ready{Token: tok, Ready: c.u8(), Fighter: c.u8()}
		if r.Ready > 1 {
			return nil, ErrMalformed
		}
		m = r
	case TypeVote:
		v := Vote{Token: tok, Choice: c.u8()}
		if v.Choice >= MaxCandidates {
			return nil, ErrMalformed
		}
		m = v
	case TypeLeave:
		m = Leave{Token: tok}
	case TypeOver:
		m = Over{Token: tok}
	case TypeWho:
		m = Who{Token: tok}
	case TypeRanked:
		rk := Ranked{Token: tok}
		copy(rk.Ticket[:], c.take(TicketLen))
		rk.Fighter = c.u8()
		m = rk
	default:
		return nil, ErrMalformed
	}
	if !c.end() {
		return nil, ErrMalformed
	}
	return m, nil
}

// decodeLobbyServer parses the body of a server-to-client lobby message.
func decodeLobbyServer(t MsgType, body []byte) (Message, error) {
	c := newCursor(body)
	var m Message
	switch t {
	case TypeQueue:
		q := Queue{State: c.u8(), Waited: c.u16(), Searching: c.u16()}
		if q.State > QueueFound {
			return nil, ErrMalformed
		}
		m = q
	case TypePlayers:
		p := Players{Online: c.u16(), Rooms: c.u16()}
		n := int(c.u8())
		if n > MaxWho {
			return nil, ErrMalformed
		}
		for i := 0; i < n && c.ok; i++ {
			pi := PlayerInfo{Name: c.str(), State: c.u8()}
			if pi.State > PlayerPlaying {
				return nil, ErrMalformed
			}
			p.List = append(p.List, pi)
		}
		m = p
	case TypeRooms:
		n := int(c.u8())
		if n > MaxListed {
			return nil, ErrMalformed
		}
		var rs Rooms
		for i := 0; i < n && c.ok; i++ {
			ri := RoomInfo{Room: c.room(), Name: c.str(), Mode: c.u8(), Players: c.u8(), Max: c.u8(), Phase: c.u8()}
			if ri.Phase > PhasePlaying {
				return nil, ErrMalformed
			}
			rs.Rooms = append(rs.Rooms, ri)
		}
		m = rs
	case TypeRoom:
		st := RoomState{Room: c.room(), You: c.u8(), Host: c.u8(), Phase: c.u8()}
		st.Settings = c.settings()
		nc := int(c.u8())
		if nc > MaxCandidates || st.Phase > PhasePlaying {
			return nil, ErrMalformed
		}
		st.Candidates = c.list(nc)
		st.Votes = c.list(nc)
		st.MsLeft = c.u16()
		np := int(c.u8())
		if np > MaxSlots {
			return nil, ErrMalformed
		}
		for i := 0; i < np && c.ok; i++ {
			p := RoomPlayer{Slot: c.u8(), Name: c.str(), Ready: c.u8(), Fighter: c.u8(), Voted: c.u8()}
			if p.Slot >= MaxSlots || p.Ready > 1 || (p.Voted != VoteNone && int(p.Voted) >= nc) {
				return nil, ErrMalformed
			}
			st.Players = append(st.Players, p)
		}
		m = st
	case TypeStart:
		s := Start{Seed: c.u32(), Mode: c.u8(), Rounds: c.u8(), Venue: c.u8()}
		np := int(c.u8())
		if np > MaxSlots {
			return nil, ErrMalformed
		}
		for i := 0; i < np && c.ok; i++ {
			s.Players = append(s.Players, StartPlayer{Slot: c.u8(), Fighter: c.u8()})
		}
		m = s
	default:
		return nil, ErrMalformed
	}
	if !c.end() {
		return nil, ErrMalformed
	}
	return m, nil
}
