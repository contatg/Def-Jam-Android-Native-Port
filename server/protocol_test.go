package relay

import (
	"bytes"
	"reflect"
	"strings"
	"testing"
)

func hdr(t MsgType) []byte { return []byte{'D', 'J', 'N', 'P', Version, byte(t)} }

func cat(parts ...[]byte) []byte {
	var b []byte
	for _, p := range parts {
		b = append(b, p...)
	}
	return b
}

var (
	tok   = Token{1, 2, 3, 4, 5, 6, 7, 8}
	code  = RoomCode{'A', 'B', 'C', '2', '3', '4'}
	build = Build{9, 9, 9, 9, 8, 8, 8, 8}
)

func TestEncodeExactBytes(t *testing.T) {
	// Spot-check the byte layout against the specification tables.
	cases := []struct {
		m    Message
		want []byte
	}{
		{Hello{Mode: ModeJoin, Room: code, Build: build, Name: "Al"},
			cat(hdr(TypeHello), []byte{1}, code[:], build[:], []byte{2, 'A', 'l'})},
		{ClientData{Token: tok, Payload: []byte{0xAA, 0xBB}},
			cat(hdr(TypeData), tok[:], []byte{0xAA, 0xBB})},
		{Ping{Token: tok, Nonce: 0x01020304},
			cat(hdr(TypePing), tok[:], []byte{4, 3, 2, 1})},
		{Bye{Token: tok}, cat(hdr(TypeBye), tok[:])},
		{Welcome{Token: tok, Slot: 1, Room: code}, cat(hdr(TypeWelcome), tok[:], []byte{1}, code[:])},
		{Peer{Slot: 0, Name: "Bo"}, cat(hdr(TypePeer), []byte{0, 2, 'B', 'o'})},
		{Peer{Slot: 1}, cat(hdr(TypePeer), []byte{1, 0})},
		{ServerData{From: 1, Payload: []byte{7}}, cat(hdr(TypeData), []byte{1, 7})},
		{Pong{Nonce: 0xA0B0C0D0}, cat(hdr(TypePong), []byte{0xD0, 0xC0, 0xB0, 0xA0})},
		{Error{Code: 7, Text: "x"}, cat(hdr(TypeError), []byte{7, 1, 'x'})},
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

func TestRoundTripClient(t *testing.T) {
	msgs := []Message{
		Hello{Mode: ModeCreate, Build: build, Name: "player one"},
		Hello{Mode: ModeJoin, Room: code, Build: build, Name: strings.Repeat("x", MaxNameLen)},
		Hello{Mode: ModeJoin, Room: code, Build: build, Name: ""}, // decodes; the server answers ERROR 4
		ClientData{Token: tok, Payload: []byte{1}},
		ClientData{Token: tok, Payload: bytes.Repeat([]byte{0x5A}, MaxPayload)},
		Ping{Token: tok, Nonce: 0xFFFFFFFF},
		Bye{Token: tok},
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

func TestRoundTripServer(t *testing.T) {
	msgs := []Message{
		Welcome{Token: tok, Slot: SlotGuest, Room: code},
		Peer{Slot: SlotHost, Name: "über"},
		Peer{Slot: SlotGuest, Name: ""},
		ServerData{From: 0, Payload: []byte("hello")},
		ServerData{From: 1, Payload: bytes.Repeat([]byte{1}, MaxPayload)},
		Pong{Nonce: 42},
		Error{Code: ErrRoomFull, Text: "room full"},
		Error{Code: ErrUnknownToken, Text: ""},
	}
	for _, m := range msgs {
		b, err := Encode(m)
		if err != nil {
			t.Fatalf("encode %T: %v", m, err)
		}
		got, err := DecodeServer(b)
		if err != nil {
			t.Fatalf("decode %T: %v", m, err)
		}
		if !reflect.DeepEqual(got, m) {
			t.Errorf("round trip mismatch: got %#v want %#v", got, m)
		}
	}
}

func TestEncodeRejects(t *testing.T) {
	bad := []Message{
		ClientData{Token: tok},
		ClientData{Token: tok, Payload: make([]byte, MaxPayload+1)},
		ServerData{Payload: nil},
		Hello{Name: strings.Repeat("n", 256)},
		Error{Text: strings.Repeat("e", 256)},
	}
	for _, m := range bad {
		if _, err := Encode(m); err == nil {
			t.Errorf("Encode(%T) accepted invalid message", m)
		}
	}
}

func TestDecodeClientMalformed(t *testing.T) {
	hello := cat(hdr(TypeHello), []byte{0}, make([]byte, 6), build[:], []byte{3, 'a', 'b', 'c'})
	ping := cat(hdr(TypePing), tok[:], []byte{1, 2, 3, 4})
	cases := map[string][]byte{
		"empty":            {},
		"short header":     []byte("DJNP\x01"),
		"header only":      hdr(TypeHello),
		"bad magic":        cat([]byte("DJNQ\x01\x05"), tok[:], []byte{1, 2, 3, 4}),
		"bad version 0":    cat([]byte("DJNP\x00\x05"), tok[:], []byte{1, 2, 3, 4}),
		"bad version 2":    cat([]byte("DJNP\x02\x05"), tok[:], []byte{1, 2, 3, 4}),
		"type 0":           cat(hdr(0), tok[:]),
		"type 9":           cat(hdr(9), tok[:]),
		"server WELCOME":   cat(hdr(TypeWelcome), tok[:], []byte{0}, code[:]),
		"server PONG":      cat(hdr(TypePong), []byte{1, 2, 3, 4}),
		"server PEER":      cat(hdr(TypePeer), []byte{0, 0}),
		"server ERROR":     cat(hdr(TypeError), []byte{7, 0}),
		"hello mode 3":     cat(hdr(TypeHello), []byte{3}, make([]byte, 6), build[:], []byte{1, 'a'}),
		"hello no name":    cat(hdr(TypeHello), []byte{0}, make([]byte, 6), build[:]),
		"hello short name": hello[:len(hello)-1],
		"hello long name":  append(append([]byte(nil), hello...), 'd'),
		"hello truncated":  hello[:10],
		"data no payload":  cat(hdr(TypeData), tok[:]),
		"data short token": cat(hdr(TypeData), tok[:5]),
		"data too big":     cat(hdr(TypeData), tok[:], make([]byte, MaxPayload+1)),
		"ping short":       ping[:len(ping)-1],
		"ping long":        append(append([]byte(nil), ping...), 0),
		"bye short":        cat(hdr(TypeBye), tok[:7]),
		"bye long":         cat(hdr(TypeBye), tok[:], []byte{0}),
		"over 1200":        cat(hdr(TypeData), tok[:], make([]byte, MaxDatagram)),
	}
	for name, b := range cases {
		if m, err := DecodeClient(b); err == nil {
			t.Errorf("%s: decoded %#v, want error", name, m)
		}
	}
}

func TestDecodeServerMalformed(t *testing.T) {
	cases := map[string][]byte{
		"welcome short":  cat(hdr(TypeWelcome), tok[:], []byte{0}, code[:5]),
		"peer no len":    cat(hdr(TypePeer), []byte{0}),
		"peer bad len":   cat(hdr(TypePeer), []byte{0, 3, 'a'}),
		"data empty":     cat(hdr(TypeData), []byte{0}),
		"pong short":     cat(hdr(TypePong), []byte{1, 2, 3}),
		"error bad len":  cat(hdr(TypeError), []byte{1, 5, 'a'}),
		"client hello":   cat(hdr(TypeHello), []byte{0}, make([]byte, 6), build[:], []byte{1, 'a'}),
		"client ping":    cat(hdr(TypePing), tok[:], []byte{1, 2, 3, 4}),
		"wrong magic":    cat([]byte("XXXX\x01\x06"), []byte{1, 2, 3, 4}),
		"wrong version":  cat([]byte("DJNP\x07\x06"), []byte{1, 2, 3, 4}),
		"too large data": cat(hdr(TypeData), []byte{0}, make([]byte, MaxPayload+1)),
	}
	for name, b := range cases {
		if m, err := DecodeServer(b); err == nil {
			t.Errorf("%s: decoded %#v, want error", name, m)
		}
	}
}

func TestRoomCode(t *testing.T) {
	if len(RoomAlphabet) != 32 {
		t.Fatalf("alphabet has %d symbols", len(RoomAlphabet))
	}
	for i := 0; i < 256; i++ {
		ch := byte(i)
		want := strings.IndexByte(RoomAlphabet, ch) >= 0
		if inAlphabet(ch) != want {
			t.Errorf("inAlphabet(%q) = %v, want %v", ch, !want, want)
		}
	}
	c, err := ParseRoomCode("HJK234")
	if err != nil || !c.Valid() || c.String() != "HJK234" {
		t.Errorf("ParseRoomCode: %v %v %q", err, c.Valid(), c)
	}
	for _, s := range []string{"ABCDE0", "abcdef", "ABCDEI", "ABCDEO", "ABCD1F"} {
		c, _ := ParseRoomCode(s)
		if c.Valid() {
			t.Errorf("%q should be invalid", s)
		}
	}
	if _, err := ParseRoomCode("ABC"); err == nil {
		t.Error("short code accepted")
	}
}

// FuzzDecodeClient checks the decoder never panics and that whatever it
// accepts re-encodes to the same bytes.
func FuzzDecodeClient(f *testing.F) {
	for _, m := range []Message{
		Hello{Mode: ModeJoin, Room: code, Build: build, Name: "fuzz"},
		ClientData{Token: tok, Payload: []byte{1, 2, 3}},
		Ping{Token: tok, Nonce: 5},
		Bye{Token: tok},
	} {
		b, _ := Encode(m)
		f.Add(b)
	}
	f.Add([]byte("DJNP"))
	f.Fuzz(func(t *testing.T, b []byte) {
		m, err := DecodeClient(b)
		if err != nil {
			return
		}
		re, err := Encode(m)
		if err != nil {
			t.Fatalf("re-encode of accepted %#v: %v", m, err)
		}
		if !bytes.Equal(re, b) {
			t.Fatalf("re-encode differs:\n in  % x\n out % x", b, re)
		}
	})
}

