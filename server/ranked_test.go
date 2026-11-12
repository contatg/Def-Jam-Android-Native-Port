package relay

import (
	"net/netip"
	"testing"
	"time"
)

func (l *lobbyHarness) rankedReq(addr netip.AddrPort, ticket byte, fighter uint8) []got {
	var tk [TicketLen]byte
	tk[0] = ticket
	return l.send(addr, Ranked{Token: l.tok[addr], Ticket: tk, Fighter: fighter})
}

// checked answers the ticket lookup of addr the way the account service would.
func (l *lobbyHarness) checked(addr netip.AddrPort, account int64, rating float64, ok bool) []got {
	l.t.Helper()
	var res []got
	for _, p := range l.r.RankedChecked(l.now, l.tok[addr], RankedPlayer{Account: account, Rating: rating, Deviation: 200}, ok) {
		m, err := DecodeServer(p.Data)
		if err != nil {
			l.t.Fatal(err)
		}
		res = append(res, got{p.To, m})
	}
	return res
}

func queueOf(t *testing.T, res []got, addr netip.AddrPort) Queue {
	t.Helper()
	var q Queue
	found := false
	for _, g := range res {
		if m, ok := g.msg.(Queue); ok && g.to == addr {
			q, found = m, true
		}
	}
	if !found {
		t.Fatalf("no QUEUE to %v in %#v", addr, res)
	}
	return q
}

// searching puts addr into the queue with that account and rating.
func (l *lobbyHarness) searching(addr netip.AddrPort, name string, account int64, rating float64, fighter uint8) {
	l.t.Helper()
	l.enter(addr, name)
	if q := queueOf(l.t, l.rankedReq(addr, byte(account), fighter), addr); q.State != QueueChecking {
		l.t.Fatalf("RANKED: %#v", q)
	}
	if q := queueOf(l.t, l.checked(addr, account, rating, true), addr); q.State != QueueSearching {
		l.t.Fatalf("checked: %#v", q)
	}
}

func TestRankedOff(t *testing.T) {
	l := newLobby(t, Config{})
	l.enter(pA, "a")
	if q := queueOf(t, l.rankedReq(pA, 1, 5), pA); q.State != QueueRefused {
		t.Fatalf("ranked off: %#v", q)
	}
	if lk, _ := l.r.TakeRankedWork(); len(lk) != 0 {
		t.Fatalf("lookups while off: %v", lk)
	}
}

func TestRankedPairAndStart(t *testing.T) {
	l := newLobby(t, Config{Ranked: true})
	l.enter(pA, "a")
	l.rankedReq(pA, 7, 58)
	// A retry while the ticket is checked asks nothing new; it may change the fighter.
	if q := queueOf(t, l.rankedReq(pA, 7, 59), pA); q.State != QueueChecking {
		t.Fatalf("retry: %#v", q)
	}
	lk, _ := l.r.TakeRankedWork()
	if len(lk) != 1 || lk[0].Token != l.tok[pA] || lk[0].Ticket[0] != 7 {
		t.Fatalf("lookups %#v", lk)
	}
	l.checked(pA, 11, 1500, true)
	l.searching(pB, "b", 22, 1560, 42)
	l.r.TakeRankedWork()

	res := l.advance(time.Second)
	if queueOf(t, res, pA).State != QueueFound || queueOf(t, res, pB).State != QueueFound {
		t.Fatalf("not paired: %#v", res)
	}
	st := roomOf(t, res, pB)
	if st.Phase != PhaseVoting || st.Settings.Name != RankedRoomName || st.Settings.Public != 0 || st.Settings.Mode != RankedMode ||
		st.Settings.Max != 2 || len(st.Players) != 2 || st.Players[0].Fighter != 59 || st.Players[1].Fighter != 42 {
		t.Fatalf("ranked room %#v", st)
	}
	if countType(l.advance(time.Second), TypeQueue) != 0 {
		t.Fatal("QUEUE after pairing")
	}
	// The host may not change a ranked room's rules.
	expectError(t, l.setSet(pA, settings4("x", 1)), pA, ErrNotAllowed)

	l.castVote(pA, 0)
	res = l.castVote(pB, 0)
	start := startOf(t, res, pA)
	_, pairings := l.r.TakeRankedWork()
	want := RankedPairing{Room: string(st.Room[:]), Seed: start.Seed, Seats: []RankedSeat{{0, 11}, {1, 22}}}
	if len(pairings) != 1 || pairings[0].Room != want.Room || pairings[0].Seed != want.Seed ||
		len(pairings[0].Seats) != 2 || pairings[0].Seats[0] != want.Seats[0] || pairings[0].Seats[1] != want.Seats[1] {
		t.Fatalf("pairings %#v want %#v", pairings, want)
	}

	// A rematch in the same room is ranked too (the account service limits repeats).
	l.over(pA)
	l.over(pB)
	l.setReady(pA, 59)
	l.setReady(pB, 42)
	l.castVote(pA, 0)
	l.castVote(pB, 0)
	if _, p := l.r.TakeRankedWork(); len(p) != 1 {
		t.Fatalf("rematch pairing %#v", p)
	}

	// Once someone else is in the room, its matches are not ranked.
	l.over(pA)
	l.over(pB)
	l.leave(pB)
	l.enter(pC, "c")
	roomOf(t, l.joinCode(pC, st.Room), pC)
	l.setReady(pA, 1)
	l.setReady(pC, 2)
	l.castVote(pA, 0)
	l.castVote(pC, 0)
	if _, p := l.r.TakeRankedWork(); len(p) != 0 {
		t.Fatalf("a stranger's match was ranked: %#v", p)
	}
}

func TestRankedWindowWidens(t *testing.T) {
	l := newLobby(t, Config{Ranked: true})
	l.searching(pA, "a", 1, 1500, 1)
	l.searching(pB, "b", 2, 1790, 2)
	// 290 apart: the window is 100 at first, 250 after 30 s, 300 after 40 s.
	for i := 0; i < 39; i++ {
		l.send(pA, Ping{Token: l.tok[pA]}) // the games keep their sessions alive
		l.send(pB, Ping{Token: l.tok[pB]})
		res := l.advance(time.Second)
		if countType(res, TypeRoom) != 0 {
			t.Fatalf("paired after %d s", i+1)
		}
		if q := queueOf(t, res, pA); q.State != QueueSearching || q.Searching != 2 || q.Waited != uint16(i+1) {
			t.Fatalf("status %#v", q)
		}
	}
	if res := l.advance(time.Second); countType(res, TypeRoom) != 2 {
		t.Fatalf("not paired at 40 s: %#v", res)
	}
}

func TestRankedPicksTheClosest(t *testing.T) {
	l := newLobby(t, Config{Ranked: true})
	l.searching(pA, "a", 1, 1500, 1)
	l.searching(pB, "b", 2, 1590, 2)
	l.searching(pC, "c", 3, 1520, 3)
	res := l.advance(time.Second)
	if roomOf(t, res, pA).Room != roomOf(t, res, pC).Room || queueOf(t, res, pB).State != QueueSearching {
		t.Fatalf("A should meet C: %#v", res)
	}
}

func TestRankedRefusals(t *testing.T) {
	l := newLobby(t, Config{Ranked: true})
	l.enter(pA, "a")
	l.rankedReq(pA, 1, 1)
	if q := queueOf(t, l.checked(pA, 0, 0, false), pA); q.State != QueueRefused {
		t.Fatalf("bad ticket: %#v", q)
	}
	if res := l.checked(pA, 5, 1500, true); len(res) != 0 {
		t.Fatalf("a late answer after the refusal: %#v", res)
	}
	// The same account from a second game.
	l.searching(pB, "b", 9, 1500, 1)
	l.enter(pC, "c")
	l.rankedReq(pC, 9, 1)
	if q := queueOf(t, l.checked(pC, 9, 1500, true), pC); q.State != QueueRefused {
		t.Fatalf("same account twice: %#v", q)
	}
	// In a room: not now.
	l.enter(pD, "d")
	l.mkRoom(pD, settings4("r", 1))
	expectError(t, l.rankedReq(pD, 4, 1), pD, ErrRoomState)
}

func TestRankedLeaveQueue(t *testing.T) {
	l := newLobby(t, Config{Ranked: true})
	l.searching(pA, "a", 1, 1500, 1)
	if q := queueOf(t, l.leave(pA), pA); q.State != QueueOff {
		t.Fatalf("leave: %#v", q)
	}
	if len(l.leave(pA)) != 0 {
		t.Fatal("a second LEAVE answered")
	}
	// QUICK takes a searching player out of the queue.
	l.searching(pB, "b", 2, 1500, 1)
	l.quickM(pB, 0, 2)
	l.searching(pC, "c", 3, 1500, 1)
	if res := l.advance(time.Second); countType(res, TypeRoom) != 0 || queueOf(t, res, pC).Searching != 1 {
		t.Fatalf("B was still queued: %#v", res)
	}
	// A player who goes silent leaves the queue with the server.
	l.advance(20 * time.Second)
	if len(l.r.queue) != 0 {
		t.Fatalf("queue %d after timeouts", len(l.r.queue))
	}
}

func TestRankedCodec(t *testing.T) {
	var tk [TicketLen]byte
	for i := range tk {
		tk[i] = byte(i + 1)
	}
	m := Ranked{Token: Token{1, 2, 3, 4, 5, 6, 7, 8}, Ticket: tk, Fighter: 70}
	b, err := Encode(m)
	if err != nil {
		t.Fatal(err)
	}
	back, err := DecodeClient(b)
	if err != nil || back != m {
		t.Fatalf("RANKED round trip %#v %v", back, err)
	}
	if _, err := DecodeClient(b[:len(b)-1]); err == nil {
		t.Fatal("short RANKED accepted")
	}
	q := Queue{State: QueueSearching, Waited: 300, Searching: 4}
	b, _ = Encode(q)
	if back, err := DecodeServer(b); err != nil || back != q {
		t.Fatalf("QUEUE round trip %#v %v", back, err)
	}
	b[HeaderLen] = 9
	if _, err := DecodeServer(b); err == nil {
		t.Fatal("QUEUE state 9 accepted")
	}
}

