package relay

import (
	"sort"
	"time"
)

// Ranked play (docs/ranked-design.md): a queue of signed-in players, paired by rating into a
// private One on One room whose players are ready at once, so the room's vote and START follow
// as in any room. The relay never decides who is who: the account service redeems each
// player's ticket (RankedChecked) and is told of every ranked match the relay starts
// (TakeRankedWork), and only results that match such a pairing change ratings.
//
// The state machine does no I/O: the Server takes the lookups and pairings and talks to the
// account service outside the lock.

// The search window: a rating difference of RankedWindowStart at first, growing by
// RankedWindowGrowth every RankedWindowStep of searching, up to RankedWindowMax, so a match
// comes even when few play.
const (
	RankedWindowStart  = 100.0
	RankedWindowGrowth = 50.0
	RankedWindowStep   = 10 * time.Second
	RankedWindowMax    = 1000.0
	RankedRoomName     = "Ranked"
	RankedMode         = 0 // One on One
)

// RankedPlayer is the account behind a ranked ticket, as the account service reports it.
type RankedPlayer struct {
	Account   int64
	Name      string
	Rating    float64
	Deviation float64
}

// RankedLookup is a ticket the Server redeems with the account service; the answer goes to
// RankedChecked with the same token.
type RankedLookup struct {
	Token  Token
	Ticket [TicketLen]byte
}

// RankedSeat is one player of a ranked match.
type RankedSeat struct {
	Slot    uint8
	Account int64
}

// RankedPairing is a ranked match the relay started: the games report it under the room code
// and the START seed ("ABC123-1A2B3C4D", then a match number).
type RankedPairing struct {
	Room  string
	Seed  uint32
	Seats []RankedSeat
}

// queued is one client in the ranked queue.
type queued struct {
	c       *client
	since   time.Time
	ticket  [TicketLen]byte
	fighter uint8
	checked bool // the account service has vouched for the ticket
	player  RankedPlayer
}

// window is the rating difference q accepts after searching until now.
func (q *queued) window(now time.Time) float64 {
	steps := float64(now.Sub(q.since) / RankedWindowStep)
	return min(RankedWindowStart+steps*RankedWindowGrowth, RankedWindowMax)
}

// TakeRankedWork returns the tickets to redeem and the ranked matches to report, and forgets
// them.
func (r *Relay) TakeRankedWork() ([]RankedLookup, []RankedPairing) {
	l, p := r.lookups, r.pairings
	r.lookups, r.pairings = nil, nil
	return l, p
}

func (r *Relay) queueTo(now time.Time, c *client, state uint8) Packet {
	q := Queue{State: state, Searching: uint16(min(r.searching(), 0xFFFF))}
	if c.queue != nil {
		q.Waited = uint16(min(now.Sub(c.queue.since)/time.Second, 0xFFFF))
	}
	return Packet{To: c.addr, Data: mustEncode(q)}
}

func (r *Relay) searching() int {
	n := 0
	for _, q := range r.queue {
		if q.checked {
			n++
		}
	}
	return n
}

// ranked handles RANKED: join the queue (the ticket is checked first), or, as a retry, only
// change the fighter.
func (r *Relay) ranked(now time.Time, c *client, m Ranked, reqLen int) []Packet {
	if !r.cfg.Ranked {
		return []Packet{r.queueTo(now, c, QueueRefused)}
	}
	if c.room != nil {
		return r.errorTo(c.addr, ErrRoomState, reqLen)
	}
	if q := c.queue; q != nil {
		q.fighter = safeFighter(m.Fighter)
		if q.checked {
			return []Packet{r.queueTo(now, c, QueueSearching)}
		}
		return []Packet{r.queueTo(now, c, QueueChecking)}
	}
	q := &queued{c: c, since: now, ticket: m.Ticket, fighter: safeFighter(m.Fighter)}
	c.queue = q
	r.queue = append(r.queue, q)
	r.lookups = append(r.lookups, RankedLookup{Token: c.token, Ticket: m.Ticket})
	r.cfg.Logf("ranked: c=%s queued, checking the ticket", c.id)
	return []Packet{r.queueTo(now, c, QueueChecking)}
}

// RankedChecked takes the account service's answer for the client with token tok: ok with the
// account, or not (unknown or used ticket, service down).
func (r *Relay) RankedChecked(now time.Time, tok Token, p RankedPlayer, ok bool) []Packet {
	c := r.byToken[tok]
	if c == nil || c.queue == nil || c.queue.checked {
		return nil // gone, or left the queue meanwhile
	}
	if ok {
		for _, o := range r.queue {
			if o.checked && o.player.Account == p.Account {
				ok = false // the same account searching from another game
			}
		}
	}
	if !ok {
		r.dequeue(c)
		r.cfg.Logf("ranked: c=%s refused", c.id)
		out := []Packet{r.queueTo(now, c, QueueRefused)}
		r.countOut(out)
		return out
	}
	c.queue.checked, c.queue.player = true, p
	c.account = p.Account
	r.cfg.Logf("ranked: c=%s searching (account %d, rating %.0f)", c.id, p.Account, p.Rating)
	out := []Packet{r.queueTo(now, c, QueueSearching)}
	r.countOut(out)
	return out
}

// dequeue takes c out of the ranked queue, if it is in it.
func (r *Relay) dequeue(c *client) {
	if c.queue == nil {
		return
	}
	for i, q := range r.queue {
		if q == c.queue {
			r.queue = append(r.queue[:i], r.queue[i+1:]...)
			break
		}
	}
	c.queue = nil
}

// tickRanked pairs the queue (the longest-waiting first, each with the closest rating both
// windows accept) and tells everyone still searching where they stand.
func (r *Relay) tickRanked(now time.Time) []Packet {
	var out []Packet
	list := make([]*queued, 0, len(r.queue))
	for _, q := range r.queue {
		if q.checked {
			list = append(list, q)
		}
	}
	sort.SliceStable(list, func(i, j int) bool { return list[i].since.Before(list[j].since) })
	paired := map[*queued]bool{}
	for i, a := range list {
		if paired[a] {
			continue
		}
		var best *queued
		bestDiff := 0.0
		for _, b := range list[i+1:] {
			if paired[b] || b.c.build != a.c.build || b.player.Account == a.player.Account {
				continue
			}
			diff := a.player.Rating - b.player.Rating
			if diff < 0 {
				diff = -diff
			}
			if diff > min(a.window(now), b.window(now)) {
				continue
			}
			if best == nil || diff < bestDiff {
				best, bestDiff = b, diff
			}
		}
		if best == nil {
			continue
		}
		pkts, ok := r.startRanked(now, a, best)
		if !ok {
			break // no room left on the server: try again on the next tick
		}
		paired[a], paired[best] = true, true
		out = append(out, pkts...)
	}
	for _, q := range r.queue {
		if q.checked {
			out = append(out, r.queueTo(now, q.c, QueueSearching))
		}
	}
	return out
}

// startRanked opens the ranked room for a and b, with both ready, which starts the vote.
func (r *Relay) startRanked(now time.Time, a, b *queued) ([]Packet, bool) {
	if len(r.rooms) >= r.cfg.MaxRooms {
		return nil, false
	}
	rm, err := r.newRoom(now, a.c.build, RoomSettings{Name: RankedRoomName, Public: 0, Mode: RankedMode, Max: 2, Pool: QuickPool}, false)
	if err != nil {
		return nil, false
	}
	rm.ranked = true
	rm.rankedAccounts = [2]int64{a.player.Account, b.player.Account}
	r.dequeue(a.c)
	r.dequeue(b.c)
	rm.place(a.c, SlotHost)
	rm.place(b.c, SlotGuest)
	a.c.ready, a.c.fighter = true, a.fighter
	b.c.ready, b.c.fighter = true, b.fighter
	r.cfg.Logf("ranked: room %s for c=%s (%.0f) and c=%s (%.0f)", rm.code, a.c.id, a.player.Rating, b.c.id, b.player.Rating)
	out := []Packet{r.queueTo(now, a.c, QueueFound), r.queueTo(now, b.c, QueueFound)}
	return append(out, r.joined(now, rm, b.c)...), true
}

// rankedStart records the pairing of a ranked room's START, as long as the room still holds
// exactly the two players the queue paired.
func (r *Relay) rankedStart(rm *room, seed uint32) {
	if !rm.ranked {
		return
	}
	members := rm.members()
	if len(members) != 2 {
		return
	}
	p := RankedPairing{Room: string(rm.code[:]), Seed: seed}
	for _, c := range members {
		if c.account == 0 || (c.account != rm.rankedAccounts[0] && c.account != rm.rankedAccounts[1]) {
			return
		}
		p.Seats = append(p.Seats, RankedSeat{Slot: c.slot, Account: c.account})
	}
	if p.Seats[0].Account == p.Seats[1].Account {
		return
	}
	r.pairings = append(r.pairings, p)
	r.cfg.Logf("ranked: room %s started a ranked match", rm.code)
}

