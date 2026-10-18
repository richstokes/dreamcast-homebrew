// Package lobby implements the DreamKarts online lobby server and its wire
// format (see ../PROTOCOL.md). Everything is little-endian and packed by
// hand, matching the console's src/net.c.
package lobby

import (
	"encoding/binary"
	"errors"
	"strings"
)

// Versions; version_test.go checks these match ../../src/net_version.h.
const (
	Proto    = 1
	Build    = 6
	MinBuild = 3
)

const (
	MaxLobbies = 10
	MaxPlayers = 6
	NameLen    = 11 // 10 characters and a NUL
	KartLen    = 21 // KART body bytes relayed in RACE packets (offsets 19-39)
)

// Packet types.
const (
	TypeHello   = 1
	TypeList    = 2
	TypeJoin    = 3
	TypeLeave   = 4
	TypeWish    = 5
	TypeKart    = 6
	TypePing    = 7
	TypeWelcome = 64
	TypeReject  = 65
	TypeLobbies = 66
	TypeLobby   = 67
	TypeRace    = 68
)

// JOIN flags (body byte 1; a one-byte JOIN from build 3 is a player).
const JoinSpectate = 1 << 0

// Lobby flags (LOBBY after the players, LOBBIES after the lobby entries).
const LobbyDemo = 1 << 0 // the bots' demo lobby: spectate only

// Spectator slot in a LOBBY packet's "your slot".
const SpectatorSlot = 255

// Reject reasons.
const (
	RejectTooOld   = 1
	RejectFull     = 2
	RejectLobbyFul = 3
	RejectBadName  = 4
	RejectSession  = 5
)

// Lobby states.
const (
	StateWaiting   = 0
	StateCountdown = 1
	StateRacing    = 2
	StateResults   = 3
)

// Kart flags (KART offset 19).
const (
	KartFinished = 1 << 0
	KartWaving   = 1 << 5 // relayed as-is; consoles animate the wave
)

// ClientHeader is the block every console packet starts with.
type ClientHeader struct {
	Type  byte
	Token uint32
	Echo  uint32 // newest server_ms the console has seen
	Hold  uint16 // ms since that packet arrived
	Build uint16
}

const clientHeaderLen = 18

var errShort = errors.New("short packet")

// ParseClient splits a console packet into its header and body.
func ParseClient(b []byte) (ClientHeader, []byte, error) {
	var h ClientHeader
	if len(b) < 4 || b[0] != 'D' || b[1] != 'K' {
		return h, nil, errors.New("bad magic")
	}
	if b[2] != Proto {
		return h, nil, errors.New("protocol mismatch")
	}
	h.Type = b[3]
	if len(b) < clientHeaderLen {
		return h, nil, errShort
	}
	h.Token = binary.LittleEndian.Uint32(b[4:])
	h.Echo = binary.LittleEndian.Uint32(b[8:])
	h.Hold = binary.LittleEndian.Uint16(b[12:])
	h.Build = binary.LittleEndian.Uint16(b[14:])
	// b[16:18] reserved for alignment of the body at 18
	return h, b[clientHeaderLen:], nil
}

// AppendClient builds a console packet (used by the bot and tests).
func AppendClient(dst []byte, h ClientHeader, body []byte) []byte {
	dst = append(dst, 'D', 'K', Proto, h.Type)
	dst = binary.LittleEndian.AppendUint32(dst, h.Token)
	dst = binary.LittleEndian.AppendUint32(dst, h.Echo)
	dst = binary.LittleEndian.AppendUint16(dst, h.Hold)
	dst = binary.LittleEndian.AppendUint16(dst, h.Build)
	dst = append(dst, 0, 0)
	return append(dst, body...)
}

// serverHeader starts a server packet.
func serverHeader(dst []byte, typ byte, serverMs uint32) []byte {
	dst = append(dst, 'D', 'K', Proto, typ)
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

func appendName(dst []byte, name string) []byte {
	var n [NameLen]byte
	copy(n[:NameLen-1], name)
	return append(dst, n[:]...)
}
