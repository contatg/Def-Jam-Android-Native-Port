package relay

import (
	"bytes"
	"reflect"
	"strings"
	"testing"
)

var testSettings = RoomSettings{Name: "Rm", Public: 1, Mode: 3, Max: 4, Rounds: 2, Pool: []uint8{1, 6, 19}}

// settingsBytes is testSettings on the wire.
var settingsBytes = []byte{2, 'R', 'm', 1, 3, 4, 2, 3, 1, 6, 19}

func TestLobbyEncodeExactBytes(t *testing.T) {
	cases := []struct {
		m    Message
		want []byte
	}{
		{Hello{Mode: ModeLobby, Build: build, Name: "Al"},
			cat(hdr(TypeHello), []byte{2}, make([]byte, 6), build[:], []byte{2, 'A', 'l'})},
		{List{Token: tok, Mode: ModeAny}, cat(hdr(TypeList), tok[:], []byte{0xFF})},
		{Create{Token: tok, Settings: testSettings}, cat(hdr(TypeCreate), tok[:], settingsBytes)},
		{Join{Token: tok, Room: code}, cat(hdr(TypeJoin), tok[:], code[:])},
		{Quick{Token: tok, Mode: 0, Max: 2}, cat(hdr(TypeQuick), tok[:], []byte{0, 2})},
		{SetSettings{Token: tok, Settings: testSettings}, cat(hdr(TypeSettings), tok[:], settingsBytes)},
		{Ready{Token: tok, Ready: 1, Fighter: 42}, cat(hdr(TypeReady), tok[:], []byte{1, 42})},
		{Vote{Token: tok, Choice: 2}, cat(hdr(TypeVote), tok[:], []byte{2})},
		{Leave{Token: tok}, cat(hdr(TypeLeave), tok[:])},
		{Over{Token: tok}, cat(hdr(TypeOver), tok[:])},
		{Rooms{}, cat(hdr(TypeRooms), []byte{0})},
		{Rooms{Rooms: []RoomInfo{{Room: code, Name: "x", Mode: 1, Players: 2, Max: 4, Phase: 1}}},
			cat(hdr(TypeRooms), []byte{1}, code[:], []byte{1, 'x', 1, 2, 4, 1})},
		{RoomState{Room: code, You: 1, Host: 0, Phase: PhaseVoting, Settings: testSettings,
			Candidates: []uint8{6, 19}, Votes: []uint8{1, 0}, MsLeft: 0x3A98,
			Players: []RoomPlayer{{Slot: 0, Name: "A", Ready: 1, Fighter: 7, Voted: 0}, {Slot: 1, Name: "B", Ready: 1, Fighter: 8, Voted: VoteNone}}},
			cat(hdr(TypeRoom), code[:], []byte{1, 0, 1}, settingsBytes, []byte{2, 6, 19, 1, 0, 0x98, 0x3A, 2,
				0, 1, 'A', 1, 7, 0, 1, 1, 'B', 1, 8, 0xFF})},
		{Start{Seed: 0x01020304, Mode: 3, Rounds: 2, Venue: 19, Players: []StartPlayer{{0, 7}, {2, 9}}},
			cat(hdr(TypeStart), []byte{4, 3, 2, 1, 3, 2, 19, 2, 0, 7, 2, 9})},
	}
	for _, c := range cases {
		got, err := Encode(c.m)
		if err != nil {
			t.Fatalf("%T: %v", c.m, err)
		}
		if !bytes.Equal(got, c.want) {
			t.Errorf("%T:\n got  % x\n want % x", c.m, got, c.want)
		}
	}
}

func TestLobbyRoundTripClient(t *testing.T) {
	pool32 := bytes.Repeat([]byte{5}, MaxPool)
	msgs := []Message{
		Hello{Mode: ModeLobby, Build: build, Name: "lobby"},
		List{Token: tok, Mode: 0},
		List{Token: tok, Mode: ModeAny},
		Create{Token: tok, Settings: testSettings},
		Create{Token: tok, Settings: RoomSettings{Name: strings.Repeat("n", MaxNameLen), Max: 2, Pool: pool32}},
		Create{Token: tok, Settings: RoomSettings{Name: "", Max: 2, Pool: []uint8{1}}}, // decodes; the server answers ERROR 4
		Join{Token: tok, Room: code},
		Quick{Token: tok, Mode: 0xFE, Max: 4},
		SetSettings{Token: tok, Settings: testSettings},
		Ready{Token: tok, Ready: 0, Fighter: 0xFF},
		Vote{Token: tok, Choice: 0},
		Leave{Token: tok},
		Over{Token: tok},
	}
	for _, m := range msgs {
		b, err := Encode(m)
		if err != nil {
			t.Fatalf("encode %T: %v", m, err)
		}
		got, err := DecodeClient(b)
		if err != nil {
			t.Fatalf("decode %T: %v (% x)", m, err, b)
		}
		if !reflect.DeepEqual(got, m) {
			t.Errorf("round trip mismatch: got %#v want %#v", got, m)
		}
	}
}

func TestLobbyRoundTripServer(t *testing.T) {
	var many []RoomInfo
	for i := 0; i < MaxListed; i++ {
		many = append(many, RoomInfo{Room: code, Name: strings.Repeat("r", MaxNameLen), Mode: uint8(i), Players: 1, Max: 4})
	}
	msgs := []Message{
		Welcome{Token: tok, Slot: SlotNone},
		Rooms{},
		Rooms{Rooms: many},
		RoomState{Room: code, You: 0, Host: 0, Phase: PhaseLobby, Settings: testSettings,
			Players: []RoomPlayer{{Slot: 0, Name: "über", Fighter: FighterNone, Voted: VoteNone}}},
		RoomState{Room: code, You: 3, Host: 1, Phase: PhasePlaying, Settings: testSettings,
			Candidates: []uint8{1, 6, 19}, Votes: []uint8{0, 2, 0}, MsLeft: 0,
			Players: []RoomPlayer{{1, "a", 1, 1, 1}, {2, "b", 1, 2, 1}, {3, "c", 0, 3, VoteNone}}},
		Start{Seed: 0xFFFFFFFF, Venue: 6, Players: []StartPlayer{{0, 1}, {1, 2}, {2, 3}, {3, 4}}},
		Start{},
		Error{Code: ErrNotAllowed, Text: ErrorText[ErrNotAllowed]},
		Error{Code: ErrRoomState, Text: ErrorText[ErrRoomState]},
	}
	for _, m := range msgs {
		b, err := Encode(m)
		if err != nil {
			t.Fatalf("encode %T: %v", m, err)
		}
		if len(b) > MaxDatagram {
			t.Fatalf("%T is %d bytes", m, len(b))
		}
		got, err := DecodeServer(b)
		if err != nil {
			t.Fatalf("decode %T: %v (% x)", m, err, b)
		}
		if !reflect.DeepEqual(got, m) {
			t.Errorf("round trip mismatch: got %#v want %#v", got, m)
		}
	}
}

func TestLobbyEncodeRejects(t *testing.T) {
	bad := []Message{
		Create{Settings: RoomSettings{Name: strings.Repeat("n", 256)}},
		Create{Settings: RoomSettings{Pool: make([]uint8, 256)}},
		RoomState{Candidates: []uint8{1, 2}, Votes: []uint8{1}},
		RoomState{Players: make([]RoomPlayer, 256)},
		Rooms{Rooms: make([]RoomInfo, 256)},
		Start{Players: make([]StartPlayer, 256)},
	}
	for _, m := range bad {
		if _, err := Encode(m); err == nil {
			t.Errorf("Encode(%#v) accepted invalid message", m)
		}
	}
}

func TestLobbyDecodeClientMalformed(t *testing.T) {
	set := func(public, mode, max uint8, pool ...byte) []byte {
		return cat(hdr(TypeCreate), tok[:], []byte{1, 'n', public, mode, max, 0, byte(len(pool))}, pool)
	}
	create := cat(hdr(TypeCreate), tok[:], settingsBytes)
	cases := map[string][]byte{
		"list short":          cat(hdr(TypeList), tok[:]),
		"list long":           cat(hdr(TypeList), tok[:], []byte{0, 0}),
		"list short token":    cat(hdr(TypeList), tok[:7]),
		"create truncated":    create[:len(create)-1],
		"create trailing":     cat(create, []byte{0}),
		"create name overrun": cat(hdr(TypeCreate), tok[:], []byte{30, 'a', 'b'}),
		"public 2":            set(2, 0, 2, 1),
		"mode 0xFF":           set(0, 0xFF, 2, 1),
		"max 1":               set(0, 0, 1, 1),
		"max 5":               set(0, 0, 5, 1),
		"pool empty":          set(0, 0, 2),
		"pool 33":             set(0, 0, 2, bytes.Repeat([]byte{1}, 33)...),
		"pool count overrun":  cat(hdr(TypeCreate), tok[:], []byte{1, 'n', 0, 0, 2, 0, 3, 1, 2}),
		"settings max 0":      cat(hdr(TypeSettings), tok[:], []byte{1, 'n', 0, 0, 0, 0, 1, 1}),
		"join short":          cat(hdr(TypeJoin), tok[:], code[:5]),
		"join long":           cat(hdr(TypeJoin), tok[:], code[:], []byte{0}),
		"quick mode any":      cat(hdr(TypeQuick), tok[:], []byte{0xFF, 2}),
		"quick max 1":         cat(hdr(TypeQuick), tok[:], []byte{0, 1}),
		"quick max 5":         cat(hdr(TypeQuick), tok[:], []byte{0, 5}),
		"quick short":         cat(hdr(TypeQuick), tok[:], []byte{0}),
		"ready 2":             cat(hdr(TypeReady), tok[:], []byte{2, 0}),
		"ready short":         cat(hdr(TypeReady), tok[:], []byte{1}),
		"vote 3":              cat(hdr(TypeVote), tok[:], []byte{3}),
		"vote 0xFF":           cat(hdr(TypeVote), tok[:], []byte{0xFF}),
		"vote long":           cat(hdr(TypeVote), tok[:], []byte{0, 0}),
		"leave long":          cat(hdr(TypeLeave), tok[:], []byte{0}),
		"over short":          cat(hdr(TypeOver), tok[:4]),
		"type 0x1E":           cat(hdr(0x1E), tok[:]),
		"who long":            cat(hdr(TypeWho), tok[:], []byte{0}),
		"type 0x0F":           cat(hdr(0x0F), tok[:]),
		"server ROOMS":        cat(hdr(TypeRooms), []byte{0}),
		"server START":        cat(hdr(TypeStart), []byte{1, 2, 3, 4, 0, 0, 1, 0}),
	}
	for name, b := range cases {
		if m, err := DecodeClient(b); err == nil {
			t.Errorf("%s: decoded %#v, want error", name, m)
		}
	}
}

func TestLobbyDecodeServerMalformed(t *testing.T) {
	room := mustEncode(RoomState{Room: code, Settings: testSettings, Candidates: []uint8{1}, Votes: []uint8{0},
		Players: []RoomPlayer{{Slot: 0, Name: "a", Voted: 0}}})
	withByte := func(b []byte, i int, v byte) []byte {
		c := append([]byte(nil), b...)
		c[i] = v
		return c
	}
	// Offsets in room: header 6, code 6, you, host, phase (14), settings 11 (from 15),
	// candidates count at 26.
	cases := map[string][]byte{
		"rooms empty body":   hdr(TypeRooms),
		"rooms count 31":     cat(hdr(TypeRooms), []byte{31}),
		"rooms truncated":    cat(hdr(TypeRooms), []byte{1}, code[:], []byte{1, 'x', 0, 1, 2}),
		"rooms phase 3":      cat(hdr(TypeRooms), []byte{1}, code[:], []byte{1, 'x', 0, 1, 2, 3}),
		"room truncated":     room[:len(room)-1],
		"room trailing":      cat(room, []byte{0}),
		"room phase 3":       withByte(room, 14, 3),
		"room max 9":         withByte(room, 15+5, 9),
		"room 4 candidates":  withByte(room, 26, 4),
		"room voted 1 of 1":  withByte(room, len(room)-1, 1),
		"room ready 2":       withByte(room, len(room)-3, 2),
		"room slot 4":        withByte(room, len(room)-6, 4),
		"start short":        cat(hdr(TypeStart), []byte{1, 2, 3, 4, 0, 0, 1}),
		"start 5 players":    cat(hdr(TypeStart), []byte{1, 2, 3, 4, 0, 0, 1, 5}, make([]byte, 10)),
		"start player short": cat(hdr(TypeStart), []byte{1, 2, 3, 4, 0, 0, 1, 1, 0}),
		"client list":        cat(hdr(TypeList), tok[:], []byte{0}),
	}
	for name, b := range cases {
		if m, err := DecodeServer(b); err == nil {
			t.Errorf("%s: decoded %#v, want error", name, m)
		}
	}
}

// FuzzDecodeLobby checks that neither decoder panics on lobby-shaped input and
// that whatever they accept re-encodes to the same bytes.
func FuzzDecodeLobby(f *testing.F) {
	for _, m := range []Message{
		Hello{Mode: ModeLobby, Build: build, Name: "fuzz"},
		List{Token: tok, Mode: 1},
		Create{Token: tok, Settings: testSettings},
		Join{Token: tok, Room: code},
		Quick{Token: tok, Mode: 0, Max: 3},
		SetSettings{Token: tok, Settings: testSettings},
		Ready{Token: tok, Ready: 1, Fighter: 3},
		Vote{Token: tok, Choice: 1},
		Leave{Token: tok},
		Over{Token: tok},
		Rooms{Rooms: []RoomInfo{{Room: code, Name: "r", Max: 2}}},
		RoomState{Room: code, Settings: testSettings, Candidates: []uint8{1, 6}, Votes: []uint8{1, 1},
			Players: []RoomPlayer{{0, "a", 1, 2, 0}, {1, "b", 1, 3, 1}}},
		Start{Seed: 9, Venue: 6, Players: []StartPlayer{{0, 1}, {1, 2}}},
	} {
		f.Add(mustEncode(m))
	}
	f.Fuzz(func(t *testing.T, b []byte) {
		for _, decode := range []func([]byte) (Message, error){DecodeClient, DecodeServer} {
			m, err := decode(b)
			if err != nil {
				continue
			}
			re, err := Encode(m)
			if err != nil {
				t.Fatalf("re-encode of accepted %#v: %v", m, err)
			}
			if !bytes.Equal(re, b) {
				t.Fatalf("re-encode differs:\n in  % x\n out % x", b, re)
			}
		}
	})
}

