package relay

import (
	"bytes"
	"testing"
	"time"
)

func privateSettings() RoomSettings {
	return RoomSettings{Name: "Contatg room", Public: 0, Mode: 0, Max: 2, Pool: []uint8{1, 6, 11}}
}
func expectCode(t *testing.T, out []got, want uint8) {
	t.Helper()
	if len(out) != 1 {
		t.Fatalf("want error %d, got %#v", want, out)
	}
	m, ok := out[0].msg.(Error)
	if !ok || m.Code != want {
		t.Fatalf("want error %d, got %#v", want, out)
	}
}
func TestPrivateAdmissionAndDiscovery(t *testing.T) {
	h := newLobby(t, Config{})
	h.enter(pA, "Host")
	h.enter(pB, "Guest")
	h.enter(pC, "Other")
	var key [32]byte
	copy(key[:], bytes.Repeat([]byte{0x71}, 32))
	result := h.send(pA, CreatePrivate{Token: h.tok[pA], Settings: privateSettings(), Credential: key})
	if len(result) == 0 || h.r.clients[pA].room == nil {
		t.Fatal("private room not created")
	}
	rm := h.r.clients[pA].room
	if !rm.protected || rm.passwordHash == key || rm.settings.Public != 0 {
		t.Fatal("bad protection state")
	}
	for _, packet := range h.send(pB, List{Token: h.tok[pB], Mode: ModeAny}) {
		if m, ok := packet.msg.(Rooms); ok && len(m.Rooms) != 0 {
			t.Fatal("private room leaked into public list")
		}
	}
	expectCode(t, h.send(pB, Join{Token: h.tok[pB], Room: rm.code}), ErrPassword)
	expectCode(t, h.send(pB, JoinPrivate{Token: h.tok[pB], Room: rm.code}), ErrPassword)
	expectCode(t, h.send(pD, Hello{Mode: ModeJoin, Name: "Legacy", Build: build, Room: rm.code}), ErrPassword)
	s := rm.settings.clone()
	s.Public = 1
	expectCode(t, h.setSet(pA, s), ErrNotAllowed)
	if len(h.send(pB, JoinPrivate{Token: h.tok[pB], Room: rm.code, Credential: key})) == 0 || h.r.clients[pB].room != rm {
		t.Fatal("correct credential rejected")
	}
	h.setReady(pA, 55)
	h.setReady(pB, 22)
	if rm.phase != PhaseVoting {
		t.Fatal("ready did not reach vote")
	}
	h.castVote(pA, 0)
	h.castVote(pB, 0)
	if rm.phase != PhasePlaying {
		t.Fatal("vote did not reach START")
	}
	for i := 0; i < 10; i++ {
		packets := h.send(pA, ClientData{Token: h.tok[pA], Payload: []byte{byte(i)}})
		if len(packets) != 1 || packets[0].to != pB {
			t.Fatal("private room data did not reach peer")
		}
	}
}
func TestPrivatePasswordRateLimit(t *testing.T) {
	h := newLobby(t, Config{})
	h.enter(pA, "Host")
	h.enter(pB, "Guest")
	var key [32]byte
	key[0] = 1
	h.send(pA, CreatePrivate{Token: h.tok[pA], Settings: privateSettings(), Credential: key})
	room := h.r.clients[pA].room
	for i := 0; i < 5; i++ {
		expectCode(t, h.send(pB, JoinPrivate{Token: h.tok[pB], Room: room.code}), ErrPassword)
	}
	expectCode(t, h.send(pB, JoinPrivate{Token: h.tok[pB], Room: room.code, Credential: key}), ErrRateLimited)
	h.now = h.now.Add(31 * time.Second)
	h.send(pB, JoinPrivate{Token: h.tok[pB], Room: room.code, Credential: key})
	if h.r.clients[pB].room != room {
		t.Fatal("rate limit never reset")
	}
}
func TestPrivateCodecStrict(t *testing.T) {
	var key [32]byte
	key[0] = 7
	for _, m := range []Message{CreatePrivate{Settings: privateSettings(), Credential: key}, JoinPrivate{Room: RoomCode{'A', 'B', 'C', 'D', 'E', 'F'}, Credential: key}} {
		b, e := Encode(m)
		if e != nil {
			t.Fatal(e)
		}
		decoded, e := DecodeClient(b)
		if e != nil || decoded.Type() != m.Type() {
			t.Fatal("roundtrip", e)
		}
		if _, e = DecodeClient(b[:len(b)-1]); e == nil {
			t.Fatal("truncated credential accepted")
		}
		if _, e = DecodeClient(append(b, 0)); e == nil {
			t.Fatal("extra bytes accepted")
		}
	}
	h := newLobby(t, Config{})
	h.enter(pA, "Host")
	expectCode(t, h.send(pA, CreatePrivate{Token: h.tok[pA], Settings: privateSettings()}), ErrPassword)
}

func TestOwnedServerRequiresPrivatePasswords(t *testing.T) {
	h := newLobby(t, Config{RequirePrivatePasswords: true})
	h.enter(pA, "Host")
	expectCode(t, h.mkRoom(pA, privateSettings()), ErrPassword)
	expectCode(t, h.send(pD, Hello{Mode: ModeCreate, Name: "Legacy", Build: build}), ErrNotAllowed)
	var key [32]byte
	key[0] = 7
	h.send(pA, CreatePrivate{Token: h.tok[pA], Settings: privateSettings(), Credential: key})
	if h.r.clients[pA].room == nil || !h.r.clients[pA].room.protected {
		t.Fatal("secure create denied")
	}
}
