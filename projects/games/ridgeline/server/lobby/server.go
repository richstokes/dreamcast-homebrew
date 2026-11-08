package lobby

import (
	"encoding/binary"
	"fmt"
	"log"
	"math"
	"net/netip"
	"time"

	"github.com/richstokes/dreamcast-homebrew/projects/common/gameserver/wordfilter"
)

// Timing.
const (
	PlayerTimeout    = 10 * time.Second
	BrowseTimeout    = 30 * time.Second // a console on the lobby list, in no lobby
	LobbyInterval    = 250 * time.Millisecond
	RelayInterval    = 83 * time.Millisecond
	MaxSessions      = 200
	maxPacketsPerSec = 60
	// The day in every lobby runs at the console's pace (src/tod.h: an
	// hour a minute). An empty lobby's day starts again at StartHour when
	// someone arrives.
	SecondsPerHour = 60.0
	StartHour      = 9.0
)

// rejectLogEvery is how often one address's rejected HELLO is logged for
// one reason: a console keeps saying hello every half second for 12 s.
const rejectLogEvery = time.Minute

// Player is one console session.
type Player struct {
	Token    uint32
	Addr     netip.AddrPort
	Name     string
	Build    uint16
	Lobby    int // -1 none
	Slot     int
	LastSeen time.Time
	PingMs   uint16

	Model, Livery byte
	Summit        bool // reached the summit since going online (sticky)

	Truck    [TruckLen]byte
	TruckAt  time.Time
	HasTruck bool

	rateWindow time.Time
	rateCount  int
}

// Lobby is one of the ten rooms: up to four trucks on the mountain, no
// leader, no race. Players come and go as they please.
type Lobby struct {
	ID       int
	Slots    [MaxPlayers]*Player
	dayFrom  time.Time // when its day started at StartHour
	lastSent time.Time
	dirty    bool
}

// Count is how many players are in the lobby.
func (l *Lobby) Count() int {
	n := 0
	for _, p := range l.Slots {
		if p != nil {
			n++
		}
	}
	return n
}

// Hour is the lobby's time of day, 0 to 24.
func (l *Lobby) Hour(now time.Time) float64 {
	h := StartHour + now.Sub(l.dayFrom).Seconds()/SecondsPerHour
	return math.Mod(h, 24)
}

// Server holds every session and lobby. It is not safe for concurrent use:
// one goroutine feeds it packets and ticks.
type Server struct {
	Latest, Min uint16
	Filter      *wordfilter.Filter // name filter; New leaves it empty
	Lobbies     [MaxLobbies]*Lobby
	Logf        func(string, ...any)
	players     map[uint32]*Player
	epoch       time.Time
	send        func(netip.AddrPort, []byte)
	rejected    map[string]time.Time
}

// New makes a server that writes packets through send.
func New(send func(netip.AddrPort, []byte), now time.Time) *Server {
	s := &Server{Latest: Build, Min: MinBuild, Filter: &wordfilter.Filter{}, Logf: log.Printf,
		players: map[uint32]*Player{}, epoch: now, send: send, rejected: map[string]time.Time{}}
	for i := range s.Lobbies {
		s.Lobbies[i] = &Lobby{ID: i, dayFrom: now}
	}
	return s
}

func (s *Server) ms(now time.Time) uint32 { return uint32(now.Sub(s.epoch) / time.Millisecond) }

// Players returns the number of live sessions.
func (s *Server) Players() int { return len(s.players) }

// Handle processes one datagram.
func (s *Server) Handle(from netip.AddrPort, pkt []byte, now time.Time) {
	h, body, err := ParseClient(pkt)
	if err != nil {
		// A console on another protocol gets told it is too old (or too
		// new: either way, not this server's). The header and REJECT body
		// stay put across protocols so it can always read why.
		if len(pkt) >= clientHeaderLen && pkt[0] == 'R' && pkt[1] == 'L' && pkt[2] != Proto && pkt[3] == TypeHello {
			s.logReject(from, helloName(pkt[clientHeaderLen:]), helloBuild(pkt),
				fmt.Sprintf("protocol %d, this server speaks %d", pkt[2], Proto), now)
			s.reject(from, RejectTooOld, now)
		}
		return
	}
	p := s.players[h.Token]
	if h.Type == TypeHello {
		s.hello(from, h, body, p, now)
		return
	}
	if p == nil {
		s.reject(from, RejectSession, now)
		return
	}
	if !s.allow(p, now) {
		return
	}
	p.Addr = from // a redialled modem keeps its seat
	p.LastSeen = now
	s.updatePing(p, h, now)
	switch h.Type {
	case TypeList:
		s.sendLobbies(p, now)
	case TypeJoin:
		if len(body) >= 3 {
			s.join(p, int(body[0]), body[1], body[2], now)
		}
	case TypeLeave:
		s.leave(p, "left", now)
	case TypeTruck:
		if len(body) >= TruckLen {
			s.truck(p, body[:TruckLen], now)
		}
	case TypePing:
	}
}

func (s *Server) allow(p *Player, now time.Time) bool {
	if now.Sub(p.rateWindow) >= time.Second {
		p.rateWindow = now
		p.rateCount = 0
	}
	p.rateCount++
	return p.rateCount <= maxPacketsPerSec
}

func (s *Server) updatePing(p *Player, h ClientHeader, now time.Time) {
	if h.Echo == 0 {
		return
	}
	rtt := int64(s.ms(now)) - int64(h.Echo) - int64(h.Hold)
	if rtt < 0 || rtt > 5000 {
		return
	}
	if p.PingMs == 0 {
		p.PingMs = uint16(rtt)
	} else {
		p.PingMs = uint16((int64(p.PingMs)*3 + rtt) / 4)
	}
}

func (s *Server) hello(from netip.AddrPort, h ClientHeader, body []byte, p *Player, now time.Time) {
	if h.Build < s.Min {
		s.logReject(from, helloName(body), h.Build, fmt.Sprintf("build %d is below the minimum %d", h.Build, s.Min), now)
		s.reject(from, RejectTooOld, now)
		return
	}
	if len(body) < NameLen {
		return
	}
	name := CleanName(body[:NameLen])
	if name == "" || s.Filter.Censor(name) != name {
		s.logReject(from, name, h.Build, "name refused", now)
		s.reject(from, RejectBadName, now)
		return
	}
	if p == nil {
		if len(s.players) >= MaxSessions {
			s.logReject(from, name, h.Build, fmt.Sprintf("server full (%d sessions)", len(s.players)), now)
			s.reject(from, RejectFull, now)
			return
		}
		p = &Player{Token: h.Token, Lobby: -1, Slot: -1}
		s.players[h.Token] = p
		s.Logf("hello %q build %d from %v (%d sessions)", name, h.Build, from, len(s.players))
	}
	p.Addr, p.Name, p.Build, p.LastSeen = from, name, h.Build, now
	status := byte(0)
	if h.Build < s.Latest {
		status = 1
	}
	b := serverHeader(nil, TypeWelcome, s.ms(now))
	b = append(b, status)
	b = binary.LittleEndian.AppendUint16(b, s.Latest)
	b = binary.LittleEndian.AppendUint16(b, s.Min)
	b = append(b, MaxLobbies, MaxPlayers)
	s.send(from, b)
}

func (s *Server) logReject(from netip.AddrPort, name string, build uint16, why string, now time.Time) {
	key := from.Addr().String() + "|" + why
	if t, ok := s.rejected[key]; ok && now.Sub(t) < rejectLogEvery {
		return
	}
	s.rejected[key] = now
	s.Logf("rejected hello %q build %d from %v: %s", name, build, from, why)
}

func (s *Server) reject(to netip.AddrPort, reason byte, now time.Time) {
	b := serverHeader(nil, TypeReject, s.ms(now))
	b = append(b, reason)
	b = binary.LittleEndian.AppendUint16(b, s.Latest)
	b = binary.LittleEndian.AppendUint16(b, s.Min)
	s.send(to, b)
}

// helloName reads the name from a HELLO body, for the log only.
func helloName(body []byte) string {
	if len(body) < NameLen {
		return ""
	}
	return CleanName(body[:NameLen])
}

// helloBuild reads the build from a whole packet of any protocol, for the log.
func helloBuild(pkt []byte) uint16 {
	if len(pkt) < clientHeaderLen {
		return 0
	}
	return binary.LittleEndian.Uint16(pkt[14:])
}

func (s *Server) join(p *Player, id int, model, livery byte, now time.Time) {
	if id < 0 || id >= MaxLobbies {
		return
	}
	p.Model, p.Livery = model%Models, livery%Liveries
	l := s.Lobbies[id]
	if p.Lobby == id {
		l.dirty = true // its resent JOIN: the state may have been lost
		return
	}
	if p.Lobby >= 0 {
		s.leave(p, "moved", now)
	}
	slot := -1
	for i, q := range l.Slots {
		if q == nil {
			slot = i
			break
		}
	}
	if slot < 0 {
		s.reject(p.Addr, RejectLobbyFull, now)
		return
	}
	if l.Count() == 0 {
		l.dayFrom = now // a fresh morning for the first one in
	}
	l.Slots[slot] = p
	p.Lobby, p.Slot = id, slot
	p.HasTruck = false
	l.dirty = true
	s.Logf("%q joined lobby %d slot %d (%d/%d)", p.Name, id+1, slot, l.Count(), MaxPlayers)
}

func (s *Server) leave(p *Player, why string, now time.Time) {
	if p.Lobby < 0 {
		return
	}
	l := s.Lobbies[p.Lobby]
	if p.Slot >= 0 && l.Slots[p.Slot] == p {
		l.Slots[p.Slot] = nil
	}
	s.Logf("%q %s lobby %d (%d/%d)", p.Name, why, l.ID+1, l.Count(), MaxPlayers)
	p.Lobby, p.Slot = -1, -1
	p.HasTruck = false
	l.dirty = true
}

func (s *Server) truck(p *Player, body []byte, now time.Time) {
	if p.Lobby < 0 {
		return
	}
	copy(p.Truck[:], body)
	p.TruckAt = now
	p.HasTruck = true
	if body[0]&TruckSummit != 0 && !p.Summit {
		p.Summit = true
		s.Lobbies[p.Lobby].dirty = true
		s.Logf("%q reached the summit in lobby %d", p.Name, p.Lobby+1)
	}
}

// Tick drops silent consoles and sends lobby state. Call it often (every
// 20 ms or so).
func (s *Server) Tick(now time.Time) {
	for tok, p := range s.players {
		limit := PlayerTimeout
		if p.Lobby < 0 {
			limit = BrowseTimeout
		}
		if now.Sub(p.LastSeen) > limit {
			if p.Lobby >= 0 {
				s.leave(p, "timed out of", now)
			}
			delete(s.players, tok)
			s.Logf("%q gone (%d sessions)", p.Name, len(s.players))
		}
	}
	for _, l := range s.Lobbies {
		if l.dirty || now.Sub(l.lastSent) >= LobbyInterval {
			s.sendLobby(l, now)
		}
	}
}

func (s *Server) sendLobby(l *Lobby, now time.Time) {
	l.dirty = false
	l.lastSent = now
	if l.Count() == 0 {
		return
	}
	b := serverHeader(nil, TypeLobby, s.ms(now))
	b = append(b, byte(l.ID), 0) // your slot, filled in per player
	b = binary.LittleEndian.AppendUint16(b, uint16(l.Hour(now)*2000))
	b = append(b, byte(l.Count()))
	for i, p := range l.Slots {
		if p == nil {
			continue
		}
		e := make([]byte, PlayerLen)
		e[0] = byte(i)
		PutName(e[1:], p.Name)
		e[12], e[13] = p.Model, p.Livery
		if p.Summit {
			e[14] |= FlagSummit
		}
		binary.LittleEndian.PutUint16(e[15:], p.PingMs)
		b = append(b, e...)
	}
	for i, p := range l.Slots {
		if p != nil {
			b[9] = byte(i)
			s.send(p.Addr, b)
		}
	}
}

// sendLobbies answers a console on the lobby browser: who is in each
// lobby, trophies and all, and the hour there.
func (s *Server) sendLobbies(p *Player, now time.Time) {
	b := serverHeader(nil, TypeLobbies, s.ms(now))
	b = append(b, MaxLobbies)
	for _, l := range s.Lobbies {
		b = append(b, byte(l.Count()), byte(l.Hour(now)*10))
		for _, q := range l.Slots {
			if q == nil {
				continue
			}
			var flags byte
			if q.Summit {
				flags |= FlagSummit
			}
			e := make([]byte, 1+NameLen)
			e[0] = flags
			PutName(e[1:], q.Name)
			b = append(b, e...)
		}
	}
	s.send(p.Addr, b)
}

// Relay sends every lobby member the other trucks' newest states, on the
// server's own beat (every RelayInterval).
func (s *Server) Relay(now time.Time) {
	for _, l := range s.Lobbies {
		for _, to := range l.Slots {
			if to == nil {
				continue
			}
			b := serverHeader(nil, TypeTrucks, s.ms(now))
			b = append(b, 0)
			n := 0
			for i, p := range l.Slots {
				if p == nil || p == to || !p.HasTruck {
					continue
				}
				age := now.Sub(p.TruckAt) / time.Millisecond
				if age > 65535 {
					age = 65535
				}
				b = append(b, byte(i))
				b = binary.LittleEndian.AppendUint16(b, uint16(age))
				b = append(b, p.Truck[:]...)
				n++
			}
			b[8] = byte(n)
			s.send(to.Addr, b)
		}
	}
}
