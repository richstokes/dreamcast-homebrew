// Package lobby implements the Ridgeline online server and its wire format
// (see ../PROTOCOL.md). Everything is little-endian and packed by hand,
// matching the console's src/net.c.
package lobby

import (
	"encoding/binary"
	"errors"
	"strings"
)

// Versions; version_test.go checks these match ../../src/net_version.h.
const (
	Proto    = 1
	Build    = 18
	MinBuild = 17
)

const (
	MaxLobbies = 20 // five an area; builds before 18 list the first fifteen, before 16 the first ten
	MaxPlayers = 4
	NameLen    = 11 // 10 characters and a NUL
	TruckLen   = 30 // TRUCK body bytes, relayed as they came in TRUCKS packets
	PlayerLen  = 17 // a LOBBY packet's player entry
)

// Packet types.
const (
	TypeHello   = 1
	TypeList    = 2
	TypeJoin    = 3 // u8 lobby, u8 truck model, u8 livery
	TypeLeave   = 4
	TypeTruck   = 5 // TruckLen bytes
	TypePing    = 7
	TypeWelcome = 64
	TypeReject  = 65
	TypeLobbies = 66
	TypeLobby   = 67
	TypeTrucks  = 68
)

// Reject reasons.
const (
	RejectTooOld    = 1
	RejectFull      = 2
	RejectLobbyFull = 3
	RejectBadName   = 4
	RejectSession   = 5
	RejectIdle      = 6 // build 7 on: left the lobby for idling (IdleTimeout)
)

// IdleBuild is the first build that understands RejectIdle; older
// consoles are sent RejectSession instead, which puts them back on the
// lobby list.
const IdleBuild = 7

// Player flags in LOBBY and LOBBIES entries.
const FlagSummit = 1 << 0 // reached the summit this session: a trophy by the name

// TruckSummit is the bit in a TRUCK body's first byte (its flags) that
// says this console has reached the summit since going online.
const TruckSummit = 1 << 0

// Models and liveries the console offers; anything else is clamped.
const (
	Models   = 6
	Liveries = 4
)

// ClientHeader is the 18 bytes every console packet starts with.
type ClientHeader struct {
	Type  byte
	Token uint32
	Echo  uint32 // newest server_ms the console has seen
	Hold  uint16 // ms since it arrived
	Build uint16
}

const clientHeaderLen = 18

var errShort = errors.New("short packet")
var errMagic = errors.New("bad magic or protocol")

// ParseClient splits a console packet into its header and body.
func ParseClient(b []byte) (ClientHeader, []byte, error) {
	if len(b) < clientHeaderLen {
		return ClientHeader{}, nil, errShort
	}
	if b[0] != 'R' || b[1] != 'L' || b[2] != Proto {
		return ClientHeader{}, nil, errMagic
	}
	h := ClientHeader{
		Type:  b[3],
		Token: binary.LittleEndian.Uint32(b[4:]),
		Echo:  binary.LittleEndian.Uint32(b[8:]),
		Hold:  binary.LittleEndian.Uint16(b[12:]),
		Build: binary.LittleEndian.Uint16(b[14:]),
	}
	return h, b[clientHeaderLen:], nil
}

// ClientPacket builds a console packet (the bot and the tests use it).
func ClientPacket(typ byte, token, echo uint32, hold, build uint16, body []byte) []byte {
	b := []byte{'R', 'L', Proto, typ}
	b = binary.LittleEndian.AppendUint32(b, token)
	b = binary.LittleEndian.AppendUint32(b, echo)
	b = binary.LittleEndian.AppendUint16(b, hold)
	b = binary.LittleEndian.AppendUint16(b, build)
	b = append(b, 0, 0)
	return append(b, body...)
}

// serverHeader starts a server packet.
func serverHeader(dst []byte, typ byte, serverMs uint32) []byte {
	dst = append(dst, 'R', 'L', Proto, typ)
	return binary.LittleEndian.AppendUint32(dst, serverMs)
}

// CleanName keeps the characters the console's font and name entry offer.
func CleanName(raw []byte) string {
	var sb strings.Builder
	for _, c := range raw {
		if c == 0 {
			break
		}
		if c >= 'a' && c <= 'z' {
			c -= 32
		}
		ok := (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '.'
		if ok && sb.Len() < NameLen-1 {
			sb.WriteByte(c)
		}
	}
	return strings.TrimSpace(sb.String())
}

// PutName writes name NUL-padded into an 11-byte field.
func PutName(dst []byte, name string) {
	for i := range dst[:NameLen] {
		dst[i] = 0
	}
	copy(dst[:NameLen-1], name)
}
