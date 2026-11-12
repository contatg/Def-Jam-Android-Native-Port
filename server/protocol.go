// Package relay implements the Def Jam: Fight for NY netplay relay, protocol
// version 1 with the lobby extension, as specified in docs/netplay-protocol.md.
//
// The package has three layers:
//
//   - protocol.go, protocol_lobby.go: encoding and decoding of the wire
//     messages (no state).
//   - relay.go, lobby.go: the room/client state machine. It never touches a
//     socket and takes the current time as an argument, so it is fully testable.
//   - server.go:   the UDP loop that feeds datagrams to the state machine and
//     sends what it returns.
package relay

import (
	"encoding/binary"
	"errors"
	"fmt"
)

// Wire constants from the specification.
const (
	Magic       = "DJNP" // 0x44 0x4A 0x4E 0x50
	Version     = 1      // protocol version byte
	HeaderLen   = 6      // magic(4) + version(1) + type(1)
	MaxDatagram = 1200   // largest datagram either side may send
	MaxPayload  = 1150   // largest DATA payload
	MaxNameLen  = 24     // HELLO name length is 1..24 bytes
	TokenLen    = 8
	RoomCodeLen = 6
	BuildLen    = 8
	DefaultPort = 27777

	// RoomAlphabet is the set of characters a room code is made of (no 0/O, 1/I).
	// It has exactly 32 symbols, so a random byte masked with 31 picks one without bias.
	RoomAlphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"
)

// MsgType is the message type byte at offset 5 of every message.
type MsgType uint8

// Message types. DATA (0x04) exists in both directions with different bodies.
const (
	TypeHello   MsgType = 0x01 // client -> server
	TypeWelcome MsgType = 0x02 // server -> client
	TypePeer    MsgType = 0x03 // server -> client
	TypeData    MsgType = 0x04 // both directions
	TypePing    MsgType = 0x05 // client -> server
	TypePong    MsgType = 0x06 // server -> client
	TypeBye     MsgType = 0x07 // client -> server
	TypeError   MsgType = 0x08 // server -> client
)

// HELLO modes (ModeLobby = 2 is in protocol_lobby.go).
const (
	ModeCreate uint8 = 0
	ModeJoin   uint8 = 1
)

// Slots inside a room.
const (
	SlotHost  uint8 = 0
	SlotGuest uint8 = 1
)

// ERROR codes.
const (
	ErrRoomNotFound  uint8 = 1
	ErrRoomFull      uint8 = 2
	ErrBuildMismatch uint8 = 3
	ErrBadName       uint8 = 4
	ErrServerFull    uint8 = 5
	ErrRateLimited   uint8 = 6
	ErrUnknownToken  uint8 = 7
)

// ErrorText is the human-readable text the server puts in each ERROR. Every
// text is at most 14 bytes, so an ERROR (8 + 14 = 22 bytes) is never larger
// than the smallest possible HELLO (22 bytes) that can trigger it.
var ErrorText = map[uint8]string{
	ErrRoomNotFound:  "room not found",
	ErrRoomFull:      "room full",
	ErrBuildMismatch: "build mismatch",
	ErrBadName:       "bad name",
	ErrServerFull:    "server full",
	ErrRateLimited:   "rate limited",
	ErrUnknownToken:  "unknown token",
	ErrNotAllowed:    "not allowed",
	ErrRoomState:     "in room/lobby",
}

// Token is the 8-byte session token the server hands out in WELCOME.
type Token [TokenLen]byte

// RoomCode is the 6-character ASCII room code.
type RoomCode [RoomCodeLen]byte

// Build is the 8-byte build fingerprint; both players of a room must match.
type Build [BuildLen]byte

func (c RoomCode) String() string { return string(c[:]) }

// Valid reports whether every character of the code is from RoomAlphabet.
func (c RoomCode) Valid() bool {
	for _, ch := range c {
		if !inAlphabet(ch) {
			return false
		}
	}
	return true
}

func inAlphabet(ch byte) bool {
	return (ch >= 'A' && ch <= 'Z' && ch != 'I' && ch != 'O') || (ch >= '2' && ch <= '9')
}

// ParseRoomCode converts a 6-character string into a RoomCode. It does not
// check the alphabet; use RoomCode.Valid for that.
func ParseRoomCode(s string) (RoomCode, error) {
	var c RoomCode
	if len(s) != RoomCodeLen {
		return c, fmt.Errorf("room code must be %d characters, got %d", RoomCodeLen, len(s))
	}
	copy(c[:], s)
	return c, nil
}

// Message is any protocol message. Concrete types are the structs below.
type Message interface {
	Type() MsgType
	// appendBody appends the body (everything after the header) to b.
	appendBody(b []byte) ([]byte, error)
}

// ---- client -> server ----

// Hello asks to create (ModeCreate) or join (ModeJoin) a room.
// Name is kept as raw bytes as received; the server validates it (ERROR 4).
type Hello struct {
	Mode  uint8
	Room  RoomCode // zeros when creating
	Build Build
	Name  string
}

// ClientData carries an opaque payload from a client to its peer.
type ClientData struct {
	Token   Token
	Payload []byte // 1..MaxPayload bytes
}

// Ping asks for a Pong and keeps the client alive.
type Ping struct {
	Token Token
	Nonce uint32
}

// Bye leaves the room.
type Bye struct {
	Token Token
}

// ---- server -> client ----

// Welcome confirms a HELLO.
type Welcome struct {
	Token Token
	Slot  uint8
	Room  RoomCode
}

// Peer announces the other player (Name non-empty) or that it left (Name empty).
type Peer struct {
	Slot uint8
	Name string
}

// ServerData is a payload forwarded from the peer in slot From.
type ServerData struct {
	From    uint8
	Payload []byte
}

// Pong echoes a Ping nonce.
type Pong struct {
	Nonce uint32
}

// Error reports a failure. Text is informational only and may be empty.
type Error struct {
	Code uint8
	Text string
}

func (Hello) Type() MsgType      { return TypeHello }
func (ClientData) Type() MsgType { return TypeData }
func (Ping) Type() MsgType       { return TypePing }
func (Bye) Type() MsgType        { return TypeBye }
func (Welcome) Type() MsgType    { return TypeWelcome }
func (Peer) Type() MsgType       { return TypePeer }
func (ServerData) Type() MsgType { return TypeData }
func (Pong) Type() MsgType       { return TypePong }
func (Error) Type() MsgType      { return TypeError }

// Encoding errors.
var (
	ErrStringTooLong = errors.New("string longer than 255 bytes")
	ErrPayloadSize   = errors.New("DATA payload must be 1..1150 bytes")
	ErrTooLarge      = errors.New("message larger than 1200 bytes")
)

func appendString(b []byte, s string) ([]byte, error) {
	if len(s) > 255 {
		return b, ErrStringTooLong
	}
	b = append(b, byte(len(s)))
	return append(b, s...), nil
}

func (m Hello) appendBody(b []byte) ([]byte, error) {
	b = append(b, m.Mode)
	b = append(b, m.Room[:]...)
	b = append(b, m.Build[:]...)
	return appendString(b, m.Name)
}

func (m ClientData) appendBody(b []byte) ([]byte, error) {
	if len(m.Payload) < 1 || len(m.Payload) > MaxPayload {
		return b, ErrPayloadSize
	}
	b = append(b, m.Token[:]...)
	return append(b, m.Payload...), nil
}

func (m Ping) appendBody(b []byte) ([]byte, error) {
	b = append(b, m.Token[:]...)
	return binary.LittleEndian.AppendUint32(b, m.Nonce), nil
}

func (m Bye) appendBody(b []byte) ([]byte, error) {
	return append(b, m.Token[:]...), nil
}

func (m Welcome) appendBody(b []byte) ([]byte, error) {
	b = append(b, m.Token[:]...)
	b = append(b, m.Slot)
	return append(b, m.Room[:]...), nil
}

func (m Peer) appendBody(b []byte) ([]byte, error) {
	b = append(b, m.Slot)
	return appendString(b, m.Name)
}

func (m ServerData) appendBody(b []byte) ([]byte, error) {
	if len(m.Payload) < 1 || len(m.Payload) > MaxPayload {
		return b, ErrPayloadSize
	}
	b = append(b, m.From)
	return append(b, m.Payload...), nil
}

func (m Pong) appendBody(b []byte) ([]byte, error) {
	return binary.LittleEndian.AppendUint32(b, m.Nonce), nil
}

func (m Error) appendBody(b []byte) ([]byte, error) {
	b = append(b, m.Code)
	return appendString(b, m.Text)
}

// Encode serialises m into a freshly allocated datagram (header included).
func Encode(m Message) ([]byte, error) {
	b := make([]byte, 0, 64)
	b = append(b, Magic...)
	b = append(b, Version, byte(m.Type()))
	b, err := m.appendBody(b)
	if err != nil {
		return nil, err
	}
	if len(b) > MaxDatagram {
		return nil, ErrTooLarge
	}
	return b, nil
}

// ErrMalformed is returned for anything that is not a well-formed message in
// the expected direction. The server drops such datagrams without replying.
var ErrMalformed = errors.New("malformed message")

// checkHeader validates magic, version and size, and returns the type and body.
func checkHeader(b []byte) (MsgType, []byte, error) {
	if len(b) < HeaderLen || len(b) > MaxDatagram {
		return 0, nil, ErrMalformed
	}
	if string(b[:4]) != Magic || b[4] != Version {
		return 0, nil, ErrMalformed
	}
	return MsgType(b[5]), b[HeaderLen:], nil
}

// readTrailingString reads a u8-length-prefixed string that must end exactly at the
// end of body.
func readTrailingString(body []byte) (string, bool) {
	if len(body) < 1 || int(body[0]) != len(body)-1 {
		return "", false
	}
	return string(body[1:]), true
}

// DecodeClient parses a client-to-server datagram (HELLO, DATA, PING, BYE and
// the lobby requests LIST, CREATE, JOIN, QUICK, SETTINGS, READY, VOTE, LEAVE, OVER).
// Lengths must be exact. Slices in the result (ClientData.Payload) alias b.
//
// A HELLO whose name length is 0 or above 24, or whose name is not UTF-8, is
// still returned (the server answers it with ERROR 4); only a name length that
// disagrees with the datagram size is malformed.
func DecodeClient(b []byte) (Message, error) {
	t, body, err := checkHeader(b)
	if err != nil {
		return nil, err
	}
	switch t {
	case TypeHello:
		// mode(1) room(6) build(8) nameLen(1) name(n)
		if len(body) < 1+RoomCodeLen+BuildLen+1 {
			return nil, ErrMalformed
		}
		var h Hello
		h.Mode = body[0]
		if h.Mode != ModeCreate && h.Mode != ModeJoin && h.Mode != ModeLobby {
			return nil, ErrMalformed
		}
		copy(h.Room[:], body[1:7])
		copy(h.Build[:], body[7:15])
		name, ok := readTrailingString(body[15:])
		if !ok {
			return nil, ErrMalformed
		}
		h.Name = name
		return h, nil
	case TypeData:
		if len(body) < TokenLen+1 || len(body) > TokenLen+MaxPayload {
			return nil, ErrMalformed
		}
		var d ClientData
		copy(d.Token[:], body[:TokenLen])
		d.Payload = body[TokenLen:]
		return d, nil
	case TypePing:
		if len(body) != TokenLen+4 {
			return nil, ErrMalformed
		}
		var p Ping
		copy(p.Token[:], body[:TokenLen])
		p.Nonce = binary.LittleEndian.Uint32(body[TokenLen:])
		return p, nil
	case TypeBye:
		if len(body) != TokenLen {
			return nil, ErrMalformed
		}
		var y Bye
		copy(y.Token[:], body)
		return y, nil
	case TypeCreatePrivate, TypeJoinPrivate, TypeList, TypeCreate, TypeJoin, TypeQuick, TypeSettings, TypeReady, TypeVote, TypeLeave, TypeOver, TypeWho, TypeRanked:
		return decodeLobbyClient(t, body)
	}
	return nil, ErrMalformed
}

// DecodeServer parses a server-to-client datagram (WELCOME, PEER, DATA, PONG,
// ERROR, ROOMS, ROOM, START). Used by clients and tests. ServerData.Payload
// aliases b; the lobby messages' slices are copies.
func DecodeServer(b []byte) (Message, error) {
	t, body, err := checkHeader(b)
	if err != nil {
		return nil, err
	}
	switch t {
	case TypeWelcome:
		if len(body) != TokenLen+1+RoomCodeLen {
			return nil, ErrMalformed
		}
		var w Welcome
		copy(w.Token[:], body[:TokenLen])
		w.Slot = body[TokenLen]
		copy(w.Room[:], body[TokenLen+1:])
		return w, nil
	case TypePeer:
		if len(body) < 2 {
			return nil, ErrMalformed
		}
		name, ok := readTrailingString(body[1:])
		if !ok {
			return nil, ErrMalformed
		}
		return Peer{Slot: body[0], Name: name}, nil
	case TypeData:
		if len(body) < 2 || len(body) > 1+MaxPayload {
			return nil, ErrMalformed
		}
		return ServerData{From: body[0], Payload: body[1:]}, nil
	case TypePong:
		if len(body) != 4 {
			return nil, ErrMalformed
		}
		return Pong{Nonce: binary.LittleEndian.Uint32(body)}, nil
	case TypeError:
		if len(body) < 2 {
			return nil, ErrMalformed
		}
		text, ok := readTrailingString(body[1:])
		if !ok {
			return nil, ErrMalformed
		}
		return Error{Code: body[0], Text: text}, nil
	case TypeRooms, TypeRoom, TypeStart, TypePlayers, TypeQueue:
		return decodeLobbyServer(t, body)
	}
	return nil, ErrMalformed
}
