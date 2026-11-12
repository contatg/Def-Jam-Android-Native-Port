package relay

import (
	"fmt"
	"net/netip"
	"testing"
	"time"
)

// lobbyHarness adds lobby clients (by address) to the harness.
type lobbyHarness struct {
	*harness
	tok map[netip.AddrPort]Token
}

// scriptedIntn returns the queued values in order, then 0, and records every n.
type scriptedIntn struct {
	vals  []int
	calls []int
}

func (s *scriptedIntn) intn(n int) int {
	s.calls = append(s.calls, n)
	if len(s.vals) == 0 {
		return 0
	}
	v := s.vals[0]
	s.vals = s.vals[1:]
	return v
}

func newLobby(t *testing.T, cfg Config) *lobbyHarness {
	t.Helper()
	if cfg.Intn == nil {
		cfg.Intn = func(int) int { return 0 } // candidates in pool order, ties to the first
	}
	if cfg.HellosPerMinute == 0 {
		cfg.HellosPerMinute = 1000
	}
	return &lobbyHarness{harness: newHarness(t, cfg), tok: map[netip.AddrPort]Token{}}
}

var (
	pA = ap("198.51.100.10:1000")
	pB = ap("198.51.100.11:1000")
	pC = ap("198.51.100.12:1000")
	pD = ap("198.51.100.13:1000")
	pE = ap("198.51.100.14:1000")
)

func (l *lobbyHarness) enter(addr netip.AddrPort, name string) Welcome {
	l.t.Helper()
	res := l.send(addr, Hello{Mode: ModeLobby, Build: build, Name: name})
	if len(res) != 1 {
		l.t.Fatalf("lobby hello: %#v", res)
	}
	w, ok := res[0].msg.(Welcome)
	if !ok || res[0].to != addr || w.Slot != SlotNone || w.Room != (RoomCode{}) || w.Token == (Token{}) {
		l.t.Fatalf("lobby welcome %#v", res[0])
	}
	l.tok[addr] = w.Token
	return w
}

func (l *lobbyHarness) mkRoom(addr netip.AddrPort, s RoomSettings) []got {
	return l.send(addr, Create{Token: l.tok[addr], Settings: s})
}
func (l *lobbyHarness) joinCode(addr netip.AddrPort, c RoomCode) []got {
	return l.send(addr, Join{Token: l.tok[addr], Room: c})
}
func (l *lobbyHarness) quickM(addr netip.AddrPort, mode, max uint8) []got {
	return l.send(addr, Quick{Token: l.tok[addr], Mode: mode, Max: max})
}
func (l *lobbyHarness) setSet(addr netip.AddrPort, s RoomSettings) []got {
	return l.send(addr, SetSettings{Token: l.tok[addr], Settings: s})
}
func (l *lobbyHarness) setReady(addr netip.AddrPort, fighter uint8) []got {
	return l.send(addr, Ready{Token: l.tok[addr], Ready: 1, Fighter: fighter})
}
func (l *lobbyHarness) castVote(addr netip.AddrPort, choice uint8) []got {
	return l.send(addr, Vote{Token: l.tok[addr], Choice: choice})
}
func (l *lobbyHarness) leave(addr netip.AddrPort) []got {
	return l.send(addr, Leave{Token: l.tok[addr]})
}
func (l *lobbyHarness) over(addr netip.AddrPort) []got {
	return l.send(addr, Over{Token: l.tok[addr]})
}
func (l *lobbyHarness) listM(addr netip.AddrPort, mode uint8) Rooms {
	l.t.Helper()
	res := l.send(addr, List{Token: l.tok[addr], Mode: mode})
	if len(res) != 1 || res[0].to != addr {
		l.t.Fatalf("LIST: %#v", res)
	}
	rs, ok := res[0].msg.(Rooms)
	if !ok {
		l.t.Fatalf("LIST answered %#v", res[0].msg)
	}
	return rs
}

// roomOf returns the last ROOM sent to addr in res.
func roomOf(t *testing.T, res []got, addr netip.AddrPort) RoomState {
	t.Helper()
	var st RoomState
	found := false
	for _, g := range res {
		if s, ok := g.msg.(RoomState); ok && g.to == addr {
			st, found = s, true
		}
	}
	if !found {
		t.Fatalf("no ROOM to %v in %#v", addr, res)
	}
	return st
}

// startOf returns the START sent to addr in res.
func startOf(t *testing.T, res []got, addr netip.AddrPort) Start {
	t.Helper()
	for _, g := range res {
		if s, ok := g.msg.(Start); ok && g.to == addr {
			return s
		}
	}
	t.Fatalf("no START to %v in %#v", addr, res)
	return Start{}
}

func countType(res []got, ty MsgType) int {
	n := 0
	for _, g := range res {
		if g.msg.Type() == ty {
			n++
		}
	}
	return n
}

// onlyRoomTo checks res is exactly one ROOM, to addr, and returns it.
func onlyRoomTo(t *testing.T, res []got, addr netip.AddrPort) RoomState {
	t.Helper()
	if len(res) != 1 || res[0].to != addr {
		t.Fatalf("want one ROOM to %v, got %#v", addr, res)
	}
	st, ok := res[0].msg.(RoomState)
	if !ok {
		t.Fatalf("want ROOM, got %#v", res[0].msg)
	}
	return st
}

func settings4(name string, public uint8) RoomSettings {
	return RoomSettings{Name: name, Public: public, Mode: 1, Max: 4, Rounds: 0, Pool: []uint8{1, 6, 11}}
}

// room4 creates a room with settings4 hosted by the first address and joins
// the others by code. It returns the room code.
func (l *lobbyHarness) room4(public uint8, addrs ...netip.AddrPort) RoomCode {
	l.t.Helper()
	for i, a := range addrs {
		l.enter(a, fmt.Sprintf("p%d", i))
	}
	code := roomOf(l.t, l.mkRoom(addrs[0], settings4("room", public)), addrs[0]).Room
	for _, a := range addrs[1:] {
		roomOf(l.t, l.joinCode(a, code), a)
	}
	return code
}

// startMatch readies everybody and has them all vote for choice.
func (l *lobbyHarness) startMatch(choice uint8, addrs ...netip.AddrPort) []got {
	l.t.Helper()
	for i, a := range addrs {
		l.setReady(a, uint8(10+i))
	}
	var res []got
	for _, a := range addrs {
		res = l.castVote(a, choice)
	}
	return res
}

func TestLobbyHello(t *testing.T) {
	l := newLobby(t, Config{})
	w := l.enter(pA, "alice")
	if l.r.Clients() != 1 || l.r.Rooms() != 0 {
		t.Fatalf("rooms=%d clients=%d", l.r.Rooms(), l.r.Clients())
	}
	// A repeated HELLO gets the same WELCOME and is not counted.
	res := l.send(pA, Hello{Mode: ModeLobby, Build: build, Name: "alice"})
	if len(res) != 1 || !msgEqual(res[0].msg, w) {
		t.Fatalf("repeat: %#v", res)
	}
	expectOne(t, l.send(pA, Ping{Token: w.Token, Nonce: 5}), pA, Pong{Nonce: 5})
	// DATA from the lobby goes nowhere; requests that need a room get ERROR 9.
	expectNone(t, l.send(pA, ClientData{Token: w.Token, Payload: []byte{1}}))
	expectError(t, l.setReady(pA, 1), pA, ErrRoomState)
	expectError(t, l.castVote(pA, 0), pA, ErrRoomState)
	expectError(t, l.over(pA), pA, ErrRoomState)
	expectError(t, l.setSet(pA, settings4("x", 1)), pA, ErrRoomState)
	// LIST always answers, even with no rooms.
	if rs := l.listM(pA, ModeAny); len(rs.Rooms) != 0 {
		t.Fatalf("rooms %#v", rs)
	}
	// Bad name, rate limit and server full apply as for mode 0/1.
	expectError(t, l.send(pB, Hello{Mode: ModeLobby, Build: build, Name: ""}), pB, ErrBadName)
	full := newLobby(t, Config{MaxClients: 1})
	full.enter(pA, "a")
	expectError(t, full.send(pB, Hello{Mode: ModeLobby, Build: build, Name: "b"}), pB, ErrServerFull)
	// Idle timeout removes a lobby client too.
	l.advance(16 * time.Second)
	if l.r.Clients() != 0 {
		t.Fatalf("lobby client not timed out")
	}
	if !containsLog(l.logs, "lobby entered") || !containsLog(l.logs, "lobby left") {
		t.Errorf("logs %q", l.logs)
	}
}

func TestLobbyCreateAndList(t *testing.T) {
	l := newLobby(t, Config{})
	l.enter(pA, "alice")
	l.enter(pB, "bob")
	l.enter(pC, "carol")

	res := l.mkRoom(pA, settings4("Alice's room", 1))
	st := onlyRoomTo(t, res, pA)
	if !st.Room.Valid() || st.You != 0 || st.Host != 0 || st.Phase != PhaseLobby || st.MsLeft != 0 ||
		len(st.Candidates) != 0 || len(st.Players) != 1 || !st.Settings.equal(settings4("Alice's room", 1)) {
		t.Fatalf("ROOM after CREATE %#v", st)
	}
	if p := st.Players[0]; p.Slot != 0 || p.Name != "alice" || p.Ready != 0 || p.Fighter != FighterNone || p.Voted != VoteNone {
		t.Fatalf("player %#v", p)
	}
	// A private room and a room of another mode.
	private := roomOf(t, l.mkRoom(pB, settings4("hidden", 0)), pB).Room
	other := settings4("other mode", 1)
	other.Mode = 7
	newest := roomOf(t, l.mkRoom(pC, other), pC).Room

	l.enter(pD, "dave")
	rs := l.listM(pD, ModeAny)
	if len(rs.Rooms) != 2 || rs.Rooms[0].Room != newest || rs.Rooms[1].Room != st.Room {
		t.Fatalf("list %#v", rs)
	}
	for _, ri := range rs.Rooms {
		if ri.Room == private {
			t.Fatal("private room listed")
		}
	}
	if want := (RoomInfo{Room: st.Room, Name: "Alice's room", Mode: 1, Players: 1, Max: 4, Phase: 0}); rs.Rooms[1] != want {
		t.Fatalf("entry %#v want %#v", rs.Rooms[1], want)
	}
	if rs := l.listM(pD, 7); len(rs.Rooms) != 1 || rs.Rooms[0].Room != newest {
		t.Fatalf("mode filter %#v", rs)
	}
	if rs := l.listM(pD, 3); len(rs.Rooms) != 0 {
		t.Fatalf("mode 3 %#v", rs)
	}
	// Rooms of another build are not listed.
	otherBuild := build
	otherBuild[0] ^= 1
	res = l.send(pE, Hello{Mode: ModeLobby, Build: otherBuild, Name: "eve"})
	l.tok[pE] = res[0].msg.(Welcome).Token
	if rs := l.listM(pE, ModeAny); len(rs.Rooms) != 0 {
		t.Fatalf("other build sees %#v", rs)
	}
	// A bad name in the settings is ERROR 4.
	expectError(t, l.mkRoom(pD, settings4("", 1)), pD, ErrBadName)
	expectError(t, l.mkRoom(pD, settings4("\xff", 1)), pD, ErrBadName)
	if l.r.Rooms() != 3 {
		t.Fatalf("rooms=%d", l.r.Rooms())
	}
}

func TestLobbyListLimit(t *testing.T) {
	l := newLobby(t, Config{})
	var codes []RoomCode
	for i := 0; i < 40; i++ {
		a := netip.AddrPortFrom(netip.AddrFrom4([4]byte{10, 9, 0, byte(i)}), 1)
		l.enter(a, "p")
		name := fmt.Sprintf("room %02d with a long name", i)[:MaxNameLen]
		codes = append(codes, roomOf(t, l.mkRoom(a, settings4(name, 1)), a).Room)
	}
	l.enter(pA, "lister")
	rs := l.listM(pA, ModeAny)
	if len(rs.Rooms) != MaxListed {
		t.Fatalf("%d rooms listed", len(rs.Rooms))
	}
	for i, ri := range rs.Rooms {
		if ri.Room != codes[39-i] {
			t.Fatalf("entry %d is %s, want %s (newest first)", i, ri.Room, codes[39-i])
		}
	}
	if b := mustEncode(rs); len(b) > MaxDatagram {
		t.Fatalf("ROOMS is %d bytes", len(b))
	}
}

func TestLobbyJoinByCodeFourSlots(t *testing.T) {
	l := newLobby(t, Config{})
	code := l.room4(0, pA) // private: join by code only
	l.enter(pB, "bob")
	l.enter(pC, "carol")
	l.enter(pD, "dave")
	l.enter(pE, "eve")

	expectError(t, l.joinCode(pB, RoomCode{'Z', 'Z', 'Z', 'Z', 'Z', 'Z'}), pB, ErrRoomNotFound)
	for i, a := range []netip.AddrPort{pB, pC, pD} {
		res := l.joinCode(a, code)
		if len(res) != i+2 || countType(res, TypePeer) != 0 {
			t.Fatalf("join %d: want ROOM to %d members and no PEER, got %#v", i, i+2, res)
		}
		st := roomOf(t, res, a)
		if st.You != uint8(i+1) || st.Host != 0 || len(st.Players) != i+2 {
			t.Fatalf("joiner %d ROOM %#v", i, st)
		}
		if roomOf(t, res, pA).You != 0 {
			t.Fatal("host ROOM has the wrong 'you'")
		}
	}
	expectError(t, l.joinCode(pE, code), pE, ErrRoomFull)
	// Build mismatch.
	l2 := newLobby(t, Config{})
	code2 := l2.room4(1, pA)
	otherBuild := build
	otherBuild[7] ^= 1
	res := l2.send(pB, Hello{Mode: ModeLobby, Build: otherBuild, Name: "b"})
	l2.tok[pB] = res[0].msg.(Welcome).Token
	expectError(t, l2.joinCode(pB, code2), pB, ErrBuildMismatch)
}

func TestLobbyPeerInRoomOfTwo(t *testing.T) {
	l := newLobby(t, Config{})
	l.enter(pA, "alice")
	l.enter(pB, "bob")
	s := settings4("duo", 0)
	s.Max = 2
	code := roomOf(t, l.mkRoom(pA, s), pA).Room
	res := l.joinCode(pB, code)
	if countType(res, TypePeer) != 2 || countType(res, TypeRoom) != 2 {
		t.Fatalf("join of a room of two: %#v", res)
	}
	if !msgEqual(res[0].msg, Peer{Slot: 0, Name: "alice"}) || res[0].to != pB ||
		!msgEqual(res[1].msg, Peer{Slot: 1, Name: "bob"}) || res[1].to != pA {
		t.Fatalf("PEERs %#v", res[:2])
	}
	res = l.leave(pB)
	if len(res) != 2 || !msgEqual(res[0].msg, Peer{Slot: 1}) || res[0].to != pA || roomOf(t, res, pA).Host != 0 {
		t.Fatalf("leave: %#v", res)
	}
}

func TestLobbyQuick(t *testing.T) {
	l := newLobby(t, Config{})
	for _, a := range []netip.AddrPort{pA, pB, pC, pD, pE} {
		l.enter(a, "q"+a.Addr().String()[len(a.Addr().String())-2:])
	}
	// Nobody waiting: A gets a new public room named "Quick: <A>", with the quick pool.
	st := onlyRoomTo(t, l.quickM(pA, 0, 2), pA)
	want := RoomSettings{Name: "Quick: q10", Public: 1, Mode: 0, Max: 2, Rounds: 0, Pool: QuickPool}
	if st.You != 0 || st.Host != 0 || !st.Settings.equal(want) {
		t.Fatalf("quick-created ROOM %#v", st)
	}
	first := st.Room
	// Same mode, another max: joins A's room all the same (PEER too: it is a room of two).
	res := l.quickM(pB, 0, 3)
	if st := roomOf(t, res, pB); st.Room != first || st.You != 1 || countType(res, TypePeer) != 2 {
		t.Fatalf("quick join %#v", res)
	}
	// A's room is full now: C gets a new room, and D joins C's.
	second := onlyRoomTo(t, l.quickM(pC, 0, 2), pC).Room
	if second == first {
		t.Fatal("joined a full room")
	}
	if st := roomOf(t, l.quickM(pD, 0, 2), pD); st.Room != second {
		t.Fatalf("quick should join C's waiting room, got %s", st.Room)
	}
	if quickName("a-very-long-player-name") != "Quick: a-very-long-playe" || len(quickName("ääääääääääääää")) > MaxNameLen {
		t.Fatalf("quick names: %q", quickName("a-very-long-player-name"))
	}
	// Most players first, then the oldest.
	l2 := newLobby(t, Config{})
	addrs := []netip.AddrPort{pA, pB, pC, pD, pE}
	for _, a := range addrs {
		l2.enter(a, "x")
	}
	old := onlyRoomTo(t, l2.quickM(pA, 1, 4), pA).Room
	newer := roomOf(t, l2.mkRoom(pB, RoomSettings{Name: "n", Public: 1, Mode: 1, Max: 4, Pool: []uint8{1}}), pB).Room
	if st := roomOf(t, l2.quickM(pC, 1, 4), pC); st.Room != old {
		t.Fatalf("tie on players should pick the oldest, got %s", st.Room)
	}
	l2.leave(pA) // old room: C alone; newer: B alone; old is still older
	roomOf(t, l2.joinCode(pD, newer), pD)
	if st := roomOf(t, l2.quickM(pE, 1, 4), pE); st.Room != newer {
		t.Fatalf("quick should pick the room with the most players, got %s", st.Room)
	}
	// Private, voting and other-mode rooms are never quick-matched.
	l3 := newLobby(t, Config{})
	code := l3.room4(0, pA)
	l3.enter(pB, "b")
	if st := onlyRoomTo(t, l3.quickM(pB, 1, 4), pB); st.Room == code {
		t.Fatal("quick joined a private room")
	}
}

func TestLobbySettingsHostOnly(t *testing.T) {
	l := newLobby(t, Config{})
	code := l.room4(1, pA, pB, pC)
	l.setReady(pB, 3)
	s := settings4("renamed", 0)
	s.Rounds = 2

	expectError(t, l.setSet(pB, s), pB, ErrNotAllowed)
	res := l.setSet(pA, s)
	if len(res) != 3 {
		t.Fatalf("SETTINGS: want ROOM to 3 members, got %#v", res)
	}
	st := roomOf(t, res, pB)
	if !st.Settings.equal(s) || st.Room != code {
		t.Fatalf("settings not applied %#v", st)
	}
	for _, p := range st.Players {
		if p.Ready != 0 {
			t.Fatalf("ready not cleared: %#v", st.Players)
		}
	}
	if p := st.Players[1]; p.Fighter != 3 {
		t.Fatalf("fighter forgotten %#v", p)
	}
	// Bad name, and a max below an occupied slot.
	expectError(t, l.setSet(pA, settings4("", 1)), pA, ErrBadName)
	small := s
	small.Max = 2
	expectError(t, l.setSet(pA, small), pA, ErrNotAllowed)
	l.leave(pC)
	if st := roomOf(t, l.setSet(pA, small), pB); st.Settings.Max != 2 {
		t.Fatalf("max 2 not applied %#v", st.Settings)
	}
	// Refused while voting.
	l.setReady(pA, 1)
	l.setReady(pB, 2)
	expectError(t, l.setSet(pA, s), pA, ErrNotAllowed)
}

func TestLobbyReadyVoteStart(t *testing.T) {
	l := newLobby(t, Config{})
	l.room4(1, pA, pB, pC)

	// Not everyone ready: still phase 0.
	res := l.setReady(pA, 11)
	if len(res) != 3 || roomOf(t, res, pC).Phase != PhaseLobby || roomOf(t, res, pC).Players[0].Ready != 1 {
		t.Fatalf("ready A: %#v", res)
	}
	l.setReady(pB, 12)
	res = l.setReady(pC, 13)
	st := roomOf(t, res, pA)
	if st.Phase != PhaseVoting || len(st.Candidates) != 3 || st.MsLeft != 15000 || len(st.Votes) != 3 {
		t.Fatalf("vote did not start: %#v", st)
	}
	if st.Candidates[0] != 1 || st.Candidates[1] != 6 || st.Candidates[2] != 11 {
		t.Fatalf("candidates %v", st.Candidates)
	}
	// READY is refused while voting; a stray OVER changes nothing.
	expectError(t, l.setReady(pA, 1), pA, ErrNotAllowed)
	if st := onlyRoomTo(t, l.over(pA), pA); st.Phase != PhaseVoting {
		t.Fatalf("OVER while voting %#v", st)
	}
	// Votes are tallied and visible; a second vote replaces the first.
	l.castVote(pA, 2)
	res = l.castVote(pA, 1)
	st = roomOf(t, res, pC)
	if st.Votes[1] != 1 || st.Votes[2] != 0 || st.Players[0].Voted != 1 || st.Players[1].Voted != VoteNone {
		t.Fatalf("tally %#v", st)
	}
	l.castVote(pB, 1)
	res = l.castVote(pC, 0) // everyone voted: START at once
	if countType(res, TypeStart) != 3 {
		t.Fatalf("want START to 3, got %#v", res)
	}
	sa, sb, sc := startOf(t, res, pA), startOf(t, res, pB), startOf(t, res, pC)
	if !msgEqual(sa, sb) || !msgEqual(sa, sc) {
		t.Fatalf("STARTs differ: %#v %#v %#v", sa, sb, sc)
	}
	want := Start{Seed: sa.Seed, Mode: 1, Rounds: 0, Venue: 6, Players: []StartPlayer{{0, 11}, {1, 12}, {2, 13}}}
	if !msgEqual(sa, want) {
		t.Fatalf("START %#v want %#v", sa, want)
	}
	if st := roomOf(t, res, pB); st.Phase != PhasePlaying || st.Votes[1] != 2 {
		t.Fatalf("ROOM after START %#v", st)
	}
	// Joining, readying and settings are refused while playing.
	l.enter(pD, "late")
	expectError(t, l.joinCode(pD, st.Room), pD, ErrNotAllowed)
	expectError(t, l.setReady(pA, 1), pA, ErrNotAllowed)
	// A late VOTE (the client lost its START) gets the same START again.
	res = l.castVote(pB, 0)
	if !msgEqual(startOf(t, res, pB), sa) || len(res) != 2 || res[0].to != pB || res[1].to != pB {
		t.Fatalf("START re-send %#v", res)
	}
	// A vote for a candidate that does not exist is refused.
	l2 := newLobby(t, Config{})
	l2.enter(pA, "a")
	l2.enter(pB, "b")
	one := RoomSettings{Name: "one", Public: 1, Mode: 0, Max: 2, Pool: []uint8{4, 4, 4}}
	code := roomOf(t, l2.mkRoom(pA, one), pA).Room
	l2.joinCode(pB, code)
	l2.setReady(pA, 1)
	if st := roomOf(t, l2.setReady(pB, 2), pA); len(st.Candidates) != 1 || st.Candidates[0] != 4 {
		t.Fatalf("duplicate pool entries must give distinct candidates: %v", st.Candidates)
	}
	expectError(t, l2.castVote(pA, 1), pA, ErrNotAllowed)
	expectError(t, l2.castVote(pA, 2), pA, ErrNotAllowed)
}

func TestLobbySeedsAreFresh(t *testing.T) {
	l := newLobby(t, Config{})
	l.room4(1, pA, pB)
	s1 := startOf(t, l.startMatch(0, pA, pB), pA)
	l.over(pA)
	l.over(pB)
	s2 := startOf(t, l.startMatch(0, pA, pB), pA)
	if s1.Seed == s2.Seed {
		t.Fatalf("same seed %#x twice", s1.Seed)
	}
}

func TestLobbyVoteTimeout(t *testing.T) {
	l := newLobby(t, Config{IdleTimeout: time.Hour})
	l.room4(1, pA, pB, pC)
	l.setReady(pA, 1)
	l.setReady(pB, 2)
	l.setReady(pC, 3)
	l.castVote(pB, 2) // only B votes
	// One ROOM per member every tick, counting down.
	for i := 1; i < 15; i++ {
		res := l.advance(time.Second)
		if len(res) != 3 || countType(res, TypeRoom) != 3 {
			t.Fatalf("tick %d: %#v", i, res)
		}
		if st := roomOf(t, res, pA); st.MsLeft != uint16(15000-1000*i) || st.Phase != PhaseVoting {
			t.Fatalf("tick %d: ms_left %d phase %d", i, st.MsLeft, st.Phase)
		}
	}
	res := l.advance(time.Second) // 15 s
	if countType(res, TypeStart) != 3 || startOf(t, res, pC).Venue != 11 {
		t.Fatalf("timeout START %#v", res)
	}
	if st := roomOf(t, res, pC); st.Phase != PhasePlaying || st.MsLeft != 0 {
		t.Fatalf("after timeout %#v", st)
	}
	// Nobody votes: random among all candidates.
	pick := &scriptedIntn{}
	l2 := newLobby(t, Config{Intn: pick.intn, IdleTimeout: time.Hour})
	l2.room4(1, pA, pB)
	l2.setReady(pA, 1)
	l2.setReady(pB, 1)
	pick.vals = []int{2}
	pick.calls = nil
	res = l2.advance(15 * time.Second)
	if v := startOf(t, res, pA).Venue; v != 11 || len(pick.calls) != 1 || pick.calls[0] != 3 {
		t.Fatalf("no-vote venue %d, intn calls %v", v, pick.calls)
	}
}

func TestLobbyVoteTie(t *testing.T) {
	pick := &scriptedIntn{}
	l := newLobby(t, Config{Intn: pick.intn, IdleTimeout: time.Hour})
	l.enter(pA, "a")
	l.enter(pB, "b")
	l.enter(pC, "c")
	code := roomOf(t, l.mkRoom(pA, RoomSettings{Name: "tie", Public: 1, Mode: 2, Max: 3, Pool: []uint8{5, 6, 7, 8}}), pA).Room
	l.joinCode(pB, code)
	l.joinCode(pC, code)
	// The draw: swap 0 with 0+3 -> [8 6 7 5], keep 1, keep 2 -> candidates 8, 6, 7.
	pick.vals = []int{3, 0, 0}
	l.setReady(pA, 1)
	l.setReady(pB, 2)
	st := roomOf(t, l.setReady(pC, 3), pA)
	if fmt.Sprint(st.Candidates) != "[8 6 7]" || fmt.Sprint(pick.calls) != "[4 3 2]" {
		t.Fatalf("candidates %v, intn calls %v", st.Candidates, pick.calls)
	}
	// A tie: A votes 8, B votes 7, C does not vote.
	l.castVote(pA, 0)
	l.castVote(pB, 2)
	pick.vals = []int{1} // second of the tied [0 2]
	pick.calls = nil
	res := l.advance(15 * time.Second)
	if v := startOf(t, res, pC).Venue; v != 7 || fmt.Sprint(pick.calls) != "[2]" {
		t.Fatalf("tie venue %d, intn calls %v", v, pick.calls)
	}
}

func TestLobbyLeaveDuringVote(t *testing.T) {
	l := newLobby(t, Config{})
	l.room4(1, pA, pB, pC)
	l.setReady(pA, 1)
	l.setReady(pB, 2)
	l.setReady(pC, 3)
	l.castVote(pA, 0)
	res := l.leave(pC)
	if len(res) != 2 {
		t.Fatalf("leave: want ROOM to the 2 remaining, got %#v", res)
	}
	st := roomOf(t, res, pA)
	if st.Phase != PhaseLobby || len(st.Candidates) != 0 || len(st.Players) != 2 || st.MsLeft != 0 {
		t.Fatalf("after leave %#v", st)
	}
	for _, p := range st.Players {
		if p.Ready != 0 || p.Voted != VoteNone {
			t.Fatalf("not cleared %#v", p)
		}
	}
	// No timer left over: the next ticks send nothing.
	expectNone(t, l.advance(20*time.Second/2))
	// The leaver is in the lobby with its token and can LIST.
	if rs := l.listM(pC, ModeAny); len(rs.Rooms) != 1 {
		t.Fatalf("list after leave %#v", rs)
	}
	// An idle timeout during the vote has the same effect.
	l2 := newLobby(t, Config{})
	l2.room4(1, pA, pB, pC)
	l2.setReady(pA, 1)
	l2.setReady(pB, 2)
	l2.setReady(pC, 3)
	for i := 0; i < 14; i++ {
		l2.advance(time.Second)
		l2.send(pA, Ping{Token: l2.tok[pA]})
		l2.send(pB, Ping{Token: l2.tok[pB]})
	}
	res = l2.advance(time.Second) // C silent for 15 s; the vote (15 s) would end now too
	if countType(res, TypeStart) != 0 {
		t.Fatalf("started without C: %#v", res)
	}
	if st := roomOf(t, res, pB); st.Phase != PhaseLobby || len(st.Players) != 2 {
		t.Fatalf("after timeout %#v", st)
	}
}

func TestLobbyHostMigration(t *testing.T) {
	l := newLobby(t, Config{})
	code := l.room4(0, pA, pB, pC)
	res := l.leave(pA)
	if len(res) != 2 || countType(res, TypePeer) != 0 {
		t.Fatalf("leave %#v", res)
	}
	if st := roomOf(t, res, pC); st.Host != 1 || st.You != 2 {
		t.Fatalf("host after leave %#v", st)
	}
	// Only the new host may change settings.
	expectError(t, l.setSet(pC, settings4("c", 1)), pC, ErrNotAllowed)
	roomOf(t, l.setSet(pB, settings4("b's now", 1)), pB)
	// A new member takes the lowest free slot and becomes host.
	res = l.joinCode(pA, code)
	if st := roomOf(t, res, pB); st.Host != 0 || st.Players[0].Slot != 0 {
		t.Fatalf("rejoin %#v", st)
	}
	// Last one out deletes the room.
	l.leave(pA)
	l.leave(pB)
	if res := l.leave(pC); len(res) != 0 || l.r.Rooms() != 0 || l.r.Clients() != 3 {
		t.Fatalf("rooms=%d clients=%d res=%#v", l.r.Rooms(), l.r.Clients(), res)
	}
	expectError(t, l.joinCode(pA, code), pA, ErrRoomNotFound)
}

func TestLobbyOverBackToLobby(t *testing.T) {
	l := newLobby(t, Config{IdleTimeout: 2 * time.Minute})
	l.room4(1, pA, pB, pC)
	l.startMatch(0, pA, pB, pC)
	// One OVER: acknowledged to the sender only; still playing.
	if st := onlyRoomTo(t, l.over(pA), pA); st.Phase != PhasePlaying {
		t.Fatalf("after one OVER %#v", st)
	}
	onlyRoomTo(t, l.over(pA), pA) // repeated
	l.over(pB)
	res := l.over(pC)
	if len(res) != 3 {
		t.Fatalf("last OVER: %#v", res)
	}
	st := roomOf(t, res, pB)
	if st.Phase != PhaseLobby || len(st.Candidates) != 0 {
		t.Fatalf("not back in lobby %#v", st)
	}
	for _, p := range st.Players {
		if p.Ready != 0 || p.Voted != VoteNone {
			t.Fatalf("not cleared %#v", p)
		}
	}
	// A late OVER in the lobby changes nothing.
	if st := onlyRoomTo(t, l.over(pA), pA); st.Phase != PhaseLobby {
		t.Fatalf("late OVER %#v", st)
	}
	// Leaving when everybody else has sent OVER ends the match too.
	l.startMatch(0, pA, pB, pC)
	l.over(pA)
	l.over(pB)
	if st := roomOf(t, l.leave(pC), pA); st.Phase != PhaseLobby {
		t.Fatalf("leave with all others over %#v", st)
	}
	// The playing timeout.
	l.startMatch(0, pA, pB)
	l.over(pA)
	for i := 0; i < 29; i++ {
		l.advance(time.Minute)
		l.send(pA, Ping{Token: l.tok[pA]})
		l.send(pB, Ping{Token: l.tok[pB]})
		l.send(pC, Ping{Token: l.tok[pC]})
	}
	if st := onlyRoomTo(t, l.over(pA), pA); st.Phase != PhasePlaying {
		t.Fatal("timed out early")
	}
	res = l.advance(time.Minute)
	if st := roomOf(t, res, pB); st.Phase != PhaseLobby || len(res) != 2 {
		t.Fatalf("play timeout %#v", res)
	}
	if !containsLog(l.logs, "play timeout") || !containsLog(l.logs, "match over") {
		t.Errorf("logs %q", l.logs)
	}
}

func TestLobbyDataFanOut(t *testing.T) {
	l := newLobby(t, Config{})
	l.room4(0, pA, pB, pC, pD)
	l.startMatch(0, pA, pB, pC, pD)
	res := l.send(pB, ClientData{Token: l.tok[pB], Payload: []byte("hit")})
	if len(res) != 3 {
		t.Fatalf("fan-out %#v", res)
	}
	seen := map[netip.AddrPort]bool{}
	for _, g := range res {
		if !msgEqual(g.msg, ServerData{From: 1, Payload: []byte("hit")}) || g.to == pB {
			t.Fatalf("got %#v to %v", g.msg, g.to)
		}
		seen[g.to] = true
	}
	if !seen[pA] || !seen[pC] || !seen[pD] {
		t.Fatalf("recipients %v", seen)
	}
	// Nothing crosses into another room.
	l2 := newLobby(t, Config{})
	l2.room4(0, pA, pB)
	l2.enter(pC, "c")
	roomOf(t, l2.mkRoom(pC, settings4("other", 0)), pC)
	expectOne(t, l2.send(pA, ClientData{Token: l2.tok[pA], Payload: []byte{1}}), pB, ServerData{From: 0, Payload: []byte{1}})
	expectNone(t, l2.send(pC, ClientData{Token: l2.tok[pC], Payload: []byte{1}}))
}

// Requests are idempotent, since clients retry over UDP.
func TestLobbyIdempotent(t *testing.T) {
	l := newLobby(t, Config{})
	l.enter(pA, "alice")
	l.enter(pB, "bob")
	l.enter(pC, "carol")
	code := roomOf(t, l.mkRoom(pA, settings4("one", 1)), pA).Room

	// 1. CREATE, QUICK and JOIN of the same room from a member: a fresh ROOM, nothing else.
	if st := onlyRoomTo(t, l.mkRoom(pA, settings4("one", 1)), pA); st.Room != code || st.You != 0 {
		t.Fatalf("CREATE retry %#v", st)
	}
	if st := onlyRoomTo(t, l.quickM(pA, 1, 4), pA); st.Room != code {
		t.Fatalf("QUICK in a room %#v", st)
	}
	if st := onlyRoomTo(t, l.joinCode(pA, code), pA); st.Room != code {
		t.Fatalf("JOIN of own room %#v", st)
	}
	if l.r.Rooms() != 1 {
		t.Fatalf("%d rooms after retries", l.r.Rooms())
	}
	roomOf(t, l.joinCode(pB, code), pB)
	if st := onlyRoomTo(t, l.joinCode(pB, code), pB); st.You != 1 || len(st.Players) != 2 {
		t.Fatalf("JOIN retry %#v", st)
	}
	// ... but JOIN of a different room while in one is ERROR 9.
	other := roomOf(t, l.mkRoom(pC, settings4("two", 1)), pC).Room
	expectError(t, l.joinCode(pB, other), pB, ErrRoomState)

	// 2. READY, VOTE and SETTINGS that change nothing: ROOM to the sender only.
	l.setReady(pB, 5)
	if st := onlyRoomTo(t, l.setReady(pB, 5), pB); st.Players[1].Ready != 1 {
		t.Fatalf("READY retry %#v", st)
	}
	if st := onlyRoomTo(t, l.setSet(pA, settings4("one", 1)), pA); st.Players[1].Ready != 1 {
		t.Fatalf("unchanged SETTINGS cleared ready: %#v", st)
	}
	l.setReady(pA, 4) // both ready: voting
	l.castVote(pA, 1)
	if st := onlyRoomTo(t, l.castVote(pA, 1), pA); st.Phase != PhaseVoting || st.Votes[1] != 1 {
		t.Fatalf("VOTE retry %#v", st)
	}

	// 3. LEAVE has no reply for the leaver; a LEAVE from the lobby is ignored.
	res := l.leave(pB)
	for _, g := range res {
		if g.to == pB {
			t.Fatalf("leaver got %#v", g.msg)
		}
	}
	expectNone(t, l.leave(pB))
	expectNone(t, l.leave(pB))
	if l.r.Clients() != 3 {
		t.Fatal("LEAVE unregistered a client")
	}
}

func TestLobbyLegacyInterop(t *testing.T) {
	l := newLobby(t, Config{})
	// A v1 host: HELLO mode 0. Its room is not listed.
	wh := l.create(hostAddr, "v1host")
	l.enter(pA, "lobby")
	if rs := l.listM(pA, ModeAny); len(rs.Rooms) != 0 {
		t.Fatalf("legacy room listed %#v", rs)
	}
	// A lobby client joins it by code: PEER both ways, ROOM to the lobby client only.
	res := l.joinCode(pA, wh.Room)
	if len(res) != 3 || !msgEqual(res[0].msg, Peer{Slot: 0, Name: "v1host"}) || res[0].to != pA ||
		!msgEqual(res[1].msg, Peer{Slot: 1, Name: "lobby"}) || res[1].to != hostAddr {
		t.Fatalf("join legacy %#v", res)
	}
	st := roomOf(t, res, pA)
	want := RoomSettings{Name: "v1host", Public: 0, Mode: 0, Max: 2, Rounds: 0, Pool: []uint8{1}}
	if st.You != 1 || st.Host != 0 || !st.Settings.equal(want) {
		t.Fatalf("legacy ROOM %#v", st)
	}
	// DATA both ways as in v1; the v1 host never sees a lobby message.
	expectOne(t, l.send(hostAddr, ClientData{Token: wh.Token, Payload: []byte{9}}), pA, ServerData{From: 0, Payload: []byte{9}})
	expectOne(t, l.send(pA, ClientData{Token: l.tok[pA], Payload: []byte{8}}), hostAddr, ServerData{From: 1, Payload: []byte{8}})
	onlyRoomTo(t, l.setReady(pA, 3), pA)
	// The v1 host leaves: PEER (empty name) and ROOM for the lobby client.
	res = l.send(hostAddr, Bye{Token: wh.Token})
	if len(res) != 2 || !msgEqual(res[0].msg, Peer{Slot: 0}) || res[0].to != pA || roomOf(t, res, pA).Host != 1 {
		t.Fatalf("v1 host BYE %#v", res)
	}
	// Legacy settings cannot be changed, even by a lobby host.
	expectError(t, l.setSet(pA, settings4("x", 1)), pA, ErrNotAllowed)
	// HELLO mode 1 into a room made by CREATE uses the lowest free slot.
	l.enter(pB, "b")
	code := roomOf(t, l.mkRoom(pB, settings4("new", 1)), pB).Room
	res = l.send(guestAddr, Hello{Mode: ModeJoin, Room: code, Build: build, Name: "v1guest"})
	if w, ok := res[0].msg.(Welcome); !ok || w.Slot != 1 || w.Room != code || len(res) != 2 || roomOf(t, res, pB).Players[1].Name != "v1guest" {
		t.Fatalf("HELLO mode 1 into a lobby room %#v", res)
	}
}

func (l *lobbyHarness) who(addr netip.AddrPort) Players {
	l.t.Helper()
	res := l.send(addr, Who{Token: l.tok[addr]})
	if len(res) != 1 || res[0].to != addr {
		l.t.Fatalf("WHO: %#v", res)
	}
	p, ok := res[0].msg.(Players)
	if !ok {
		l.t.Fatalf("WHO answered %#v", res[0].msg)
	}
	return p
}

func TestLobbyWho(t *testing.T) {
	l := newLobby(t, Config{})
	l.enter(pA, "zed")
	l.enter(pB, "bob")
	l.enter(pC, "carol")
	l.room4(1, pB, pC)
	p := l.who(pA)
	if p.Online != 3 || p.Rooms != 1 || len(p.List) != 3 {
		t.Fatalf("WHO: %+v", p)
	}
	// the hub first, then the rooms; by name within each
	want := []PlayerInfo{{"zed", PlayerLobby}, {"bob", PlayerRoom}, {"carol", PlayerRoom}}
	for i, w := range want {
		if p.List[i] != w {
			t.Fatalf("entry %d is %+v, want %+v", i, p.List[i], w)
		}
	}
	if b := mustEncode(p); len(b) > MaxDatagram {
		t.Fatalf("PLAYERS is %d bytes", len(b))
	}
}

func TestLobbyWhoLimit(t *testing.T) {
	l := newLobby(t, Config{})
	for i := 0; i < 60; i++ {
		a := netip.AddrPortFrom(netip.AddrFrom4([4]byte{10, 8, 0, byte(i)}), 1)
		l.enter(a, fmt.Sprintf("player number %02d long", i))
	}
	p := l.who(netip.AddrPortFrom(netip.AddrFrom4([4]byte{10, 8, 0, 0}), 1))
	if p.Online != 60 || len(p.List) != MaxWho {
		t.Fatalf("WHO: online %d, %d listed", p.Online, len(p.List))
	}
	if b := mustEncode(p); len(b) > MaxDatagram {
		t.Fatalf("PLAYERS is %d bytes", len(b))
	}
}

func TestLobbyUnsafeFighterReplaced(t *testing.T) {
	l := newLobby(t, Config{})
	l.room4(1, pA, pB)
	l.setReady(pA, 40) // Teck: crashes the game's match load
	res := l.setReady(pB, 12)
	st := roomOf(t, res, pA)
	if st.Players[0].Fighter != SafeFighter || st.Players[1].Fighter != 12 {
		t.Fatalf("fighters %+v", st.Players)
	}
	l.castVote(pA, 0)
	start := startOf(t, l.castVote(pB, 0), pA)
	if start.Players[0].Fighter != SafeFighter {
		t.Fatalf("START %+v", start)
	}
}

func TestLobbyUnsafeVenueLeftOut(t *testing.T) {
	l := newLobby(t, Config{})
	l.enter(pA, "a")
	l.enter(pB, "b")
	pit := RoomSettings{Name: "pit", Public: 1, Mode: 0, Max: 2, Pool: []uint8{19, 4}}
	code := roomOf(t, l.mkRoom(pA, pit), pA).Room
	l.joinCode(pB, code)
	l.setReady(pA, 1)
	st := roomOf(t, l.setReady(pB, 2), pA)
	if len(st.Candidates) != 1 || st.Candidates[0] != 4 {
		t.Fatalf("candidates %v", st.Candidates)
	}
	// Only unsafe venues: the quick-match pool instead.
	l.over(pA)
	l.leave(pA)
	l.leave(pB)
	l.enter(pC, "c")
	l.enter(pD, "d")
	only := RoomSettings{Name: "only", Public: 1, Mode: 0, Max: 2, Pool: []uint8{19}}
	code = roomOf(t, l.mkRoom(pC, only), pC).Room
	l.joinCode(pD, code)
	l.setReady(pC, 1)
	st = roomOf(t, l.setReady(pD, 2), pC)
	if len(st.Candidates) != 3 || st.Candidates[0] != 1 || st.Candidates[1] != 6 || st.Candidates[2] != 11 {
		t.Fatalf("candidates %v", st.Candidates)
	}
}

