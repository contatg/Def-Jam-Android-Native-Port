// contatg Android room extension. Original relay: FFNY Online (MIT), see LICENSE.
package relay

import (
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"time"
)

const (
	TypeCreatePrivate MsgType = 0x20
	TypeJoinPrivate   MsgType = 0x21
	ErrPassword       uint8   = 10
)

// Credential is a client-side PBKDF2 result, never the player's plain password.
// UDP is not encrypted: a credential is sensitive, must not be logged or placed
// in room listings/GitHub. This is admission control, not encrypted transport.
type CreatePrivate struct {
	Token      Token
	Settings   RoomSettings
	Credential [32]byte
}
type JoinPrivate struct {
	Token      Token
	Room       RoomCode
	Credential [32]byte
}

func (CreatePrivate) Type() MsgType  { return TypeCreatePrivate }
func (JoinPrivate) Type() MsgType    { return TypeJoinPrivate }
func (m CreatePrivate) token() Token { return m.Token }
func (m JoinPrivate) token() Token   { return m.Token }
func (m CreatePrivate) appendBody(b []byte) ([]byte, error) {
	b, e := m.Settings.append(append(b, m.Token[:]...))
	if e != nil {
		return nil, e
	}
	return append(b, m.Credential[:]...), nil
}
func (m JoinPrivate) appendBody(b []byte) ([]byte, error) {
	return append(append(append(b, m.Token[:]...), m.Room[:]...), m.Credential[:]...), nil
}
func passwordHash(salt [16]byte, key [32]byte) [32]byte {
	var input [48]byte
	copy(input[:16], salt[:])
	copy(input[16:], key[:])
	return sha256.Sum256(input[:])
}
func (r *Relay) createPrivate(now time.Time, c *client, m CreatePrivate, reqLen int) []Packet {
	if c.room != nil {
		return r.alreadyInRoom(now, c)
	}
	if m.Settings.Public != 0 || m.Credential == ([32]byte{}) {
		return r.errorTo(c.addr, ErrPassword, reqLen)
	}
	var salt [16]byte
	if _, e := rand.Read(salt[:]); e != nil {
		return nil
	}
	out := r.create(now, c, m.Settings, reqLen)
	if c.room != nil {
		c.room.protected = true
		c.room.passwordSalt = salt
		c.room.passwordHash = passwordHash(salt, m.Credential)
	}
	return out
}
func (r *Relay) joinPrivate(now time.Time, c *client, m JoinPrivate, reqLen int) []Packet {
	if c.room != nil {
		if c.room.code != m.Room {
			return r.errorTo(c.addr, ErrRoomState, reqLen)
		}
		return []Packet{r.roomTo(now, c)}
	}
	rm := r.rooms[m.Room]
	if rm == nil {
		return r.errorTo(c.addr, ErrRoomNotFound, reqLen)
	}
	if now.Sub(c.passwordWindow) >= 30*time.Second {
		c.passwordWindow = now
		c.passwordFails = 0
	}
	if c.passwordFails >= 5 {
		return r.errorTo(c.addr, ErrRateLimited, reqLen)
	}
	if rm.protected {
		got := passwordHash(rm.passwordSalt, m.Credential)
		if subtle.ConstantTimeCompare(got[:], rm.passwordHash[:]) != 1 {
			c.passwordFails++
			return r.errorTo(c.addr, ErrPassword, reqLen)
		}
	}
	slot, code := rm.joinable(c.build)
	if code != 0 {
		return r.errorTo(c.addr, code, reqLen)
	}
	c.passwordFails = 0
	rm.place(c, slot)
	return r.joined(now, rm, c)
}
